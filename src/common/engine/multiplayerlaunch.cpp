#include "multiplayerlaunch.h"

#include "c_dispatch.h"
#include "cmdlib.h"
#include "gamedata/g_mapinfo.h"
#include "gstrings.h"
#include "i_net.h"
#include "menu.h"
#include "p_setup.h"
#include "doomstat.h"

#include <string.h>

extern bool wantToRestart;

// [UZDXREMA] Upstream 5.0.0 removed the doomcom/netnode model, and with it
// ENetConstants::MAXNETNODES (the old "max computers in a game" cap of 8).
// The participant ceiling a host may request is now MAXPLAYERS in i_net.h -
// HostGame() in i_net.cpp fatals if the -host count exceeds it - so validate
// the pending launch against that instead.
static constexpr int MP_MAX_HOST_PLAYERS = (int)MAXPLAYERS;

enum EPendingMultiplayerLaunchMode
{
	MP_LAUNCH_None,
	MP_LAUNCH_Host,
	MP_LAUNCH_Join,
};

struct FPendingMultiplayerLaunch
{
	EPendingMultiplayerLaunchMode Mode = MP_LAUNCH_None;
	int PlayerCount = 2;
	int GameMode = 0;
	int NetMode = 0;
	int Skill = 1;
	FString MapName;
	FString JoinAddress;
};

static FPendingMultiplayerLaunch PendingMultiplayerLaunch;
static bool PendingMultiplayerHardRestart = false;

static void ClearPendingLaunchRequestInternal()
{
	PendingMultiplayerLaunch = FPendingMultiplayerLaunch();
	PendingMultiplayerHardRestart = false;
}

static bool IsLaunchableMapName(const char* mapName)
{
	if (mapName == nullptr || *mapName == 0)
	{
		return false;
	}

	for (unsigned i = 0; i < wadlevelinfos.Size(); ++i)
	{
		auto& info = wadlevelinfos[i];
		if (!info.isValid() || info.MapName.IsEmpty() || info.MapName.CompareNoCase(mapName) != 0)
		{
			continue;
		}
		if (!P_CheckMapData(info.MapName.GetChars()))
		{
			continue;
		}
		return true;
	}

	return false;
}

static bool BuildTransientLaunchArgs(TArray<FString>& outArgs)
{
	outArgs.Clear();

	switch (PendingMultiplayerLaunch.Mode)
	{
	case MP_LAUNCH_Host:
		outArgs.Push("-host");
		outArgs.Push(FStringf("%d", PendingMultiplayerLaunch.PlayerCount));
		outArgs.Push("-netmode");
		outArgs.Push(FStringf("%d", PendingMultiplayerLaunch.NetMode));
		if (PendingMultiplayerLaunch.GameMode == 1)
		{
			outArgs.Push("-deathmatch");
		}
		else if (PendingMultiplayerLaunch.GameMode == 2)
		{
			outArgs.Push("-altdeath");
		}
		outArgs.Push("-skill");
		outArgs.Push(FStringf("%d", PendingMultiplayerLaunch.Skill));
		if (PendingMultiplayerLaunch.MapName.IsNotEmpty())
		{
			outArgs.Push("+map");
			outArgs.Push(PendingMultiplayerLaunch.MapName);
		}
		return true;

	case MP_LAUNCH_Join:
		outArgs.Push("-join");
		outArgs.Push(PendingMultiplayerLaunch.JoinAddress);
		return true;

	default:
		return false;
	}
}

static FString BuildTransientLaunchCommandLine(const TArray<FString>& argsToPersist)
{
	// Match the regular commandline file format so FCommandLine parsing
	// can safely skip argv[0] and still retain the first real switch.
	FString commandLine = "doomxr";
	for (const FString& arg : argsToPersist)
	{
		commandLine += " ";
		commandLine += arg;
	}
	commandLine += "\n";
	return commandLine;
}

static bool PersistTransientLaunchArgs(const TArray<FString>& argsToPersist)
{
	FString path = progdir + "commandline_mp.txt";
	auto* writer = FileWriter::Open(path.GetChars());
	if (writer == nullptr)
	{
		return false;
	}

	const FString commandLine = BuildTransientLaunchCommandLine(argsToPersist);
	const bool ok = writer->Write(commandLine.GetChars(), commandLine.Len()) == (size_t)commandLine.Len();
	delete writer;
	return ok;
}

bool M_SetPendingMultiplayerLaunchHost(int playerCount, int netMode, int gameMode, int skill, const char* mapName)
{
	ClearPendingLaunchRequestInternal();

	PendingMultiplayerLaunch.Mode = MP_LAUNCH_Host;
	PendingMultiplayerLaunch.PlayerCount = playerCount;
	PendingMultiplayerLaunch.NetMode = netMode;
	PendingMultiplayerLaunch.GameMode = gameMode;
	PendingMultiplayerLaunch.Skill = skill;
	PendingMultiplayerLaunch.MapName = mapName != nullptr ? mapName : "";
	return true;
}

bool M_SetPendingMultiplayerLaunchJoin(const char* address)
{
	ClearPendingLaunchRequestInternal();

	PendingMultiplayerLaunch.Mode = MP_LAUNCH_Join;
	PendingMultiplayerLaunch.JoinAddress = address != nullptr ? address : "";
	return true;
}

bool M_HasPendingMultiplayerLaunch()
{
	return PendingMultiplayerLaunch.Mode != MP_LAUNCH_None;
}

bool M_ValidatePendingMultiplayerLaunch(FString& errorText)
{
	if (PendingMultiplayerLaunch.Mode == MP_LAUNCH_Host)
	{
		if (PendingMultiplayerLaunch.PlayerCount < 2 || PendingMultiplayerLaunch.PlayerCount > MP_MAX_HOST_PLAYERS)
		{
			errorText = GStrings.GetString("OPTMNU_MULTIPLAYER_HOST_INVALID_PLAYERS");
			return false;
		}
		if (!IsLaunchableMapName(PendingMultiplayerLaunch.MapName.GetChars()))
		{
			errorText = GStrings.GetString("OPTMNU_MULTIPLAYER_HOST_INVALID_MAP");
			return false;
		}
		if (PendingMultiplayerLaunch.Skill < 1 || PendingMultiplayerLaunch.Skill > (int)AllSkills.Size())
		{
			errorText = GStrings.GetString("OPTMNU_MULTIPLAYER_HOST_INVALID_SKILL");
			return false;
		}
		if (PendingMultiplayerLaunch.GameMode < 0 || PendingMultiplayerLaunch.GameMode > 2)
		{
			errorText = GStrings.GetString("OPTMNU_MULTIPLAYER_HOST_INVALID_GAMEMODE");
			return false;
		}
		if (PendingMultiplayerLaunch.NetMode < 0 || PendingMultiplayerLaunch.NetMode > 1)
		{
			errorText = GStrings.GetString("OPTMNU_MULTIPLAYER_HOST_INVALID_NETMODE");
			return false;
		}
		return true;
	}

	if (PendingMultiplayerLaunch.Mode == MP_LAUNCH_Join)
	{
		FString address = PendingMultiplayerLaunch.JoinAddress;
		address.StripLeftRight();
		if (address.IsEmpty())
		{
			errorText = GStrings.GetString("OPTMNU_MULTIPLAYER_JOIN_INVALID_ADDRESS");
			return false;
		}
		PendingMultiplayerLaunch.JoinAddress = address;
		return true;
	}

	errorText = GStrings.GetString("OPTMNU_MULTIPLAYER_NO_REQUEST");
	return false;
}

void M_ClearPendingMultiplayerLaunch()
{
	ClearPendingLaunchRequestInternal();
}

bool M_ConsumePendingMultiplayerHardRestart()
{
	if (!PendingMultiplayerHardRestart)
	{
		return false;
	}

	PendingMultiplayerHardRestart = false;
	return true;
}

bool M_BeginPendingMultiplayerLaunch()
{
	FString errorText;
	if (!M_ValidatePendingMultiplayerLaunch(errorText))
	{
		M_StartMessage(errorText.GetChars(), 1);
		M_ClearPendingMultiplayerLaunch();
		return false;
	}

	TArray<FString> transientArgs;
	if (!BuildTransientLaunchArgs(transientArgs))
	{
		M_ClearPendingMultiplayerLaunch();
		M_StartMessage(GStrings.GetString("OPTMNU_MULTIPLAYER_NO_REQUEST"), 1);
		return false;
	}

	if (!PersistTransientLaunchArgs(transientArgs))
	{
		M_ClearPendingMultiplayerLaunch();
		M_StartMessage(GStrings.GetString("OPTMNU_MULTIPLAYER_NO_REQUEST"), 1);
		return false;
	}
	ClearPendingLaunchRequestInternal();
	PendingMultiplayerHardRestart = true;
	M_ClearMenus();
	wantToRestart = true;
	return true;
}
