#pragma once

#include "zstring.h"

bool M_SetPendingMultiplayerLaunchHost(int playerCount, int netMode, int gameMode, int skill, const char* mapName);
bool M_SetPendingMultiplayerLaunchJoin(const char* address);
bool M_HasPendingMultiplayerLaunch();
bool M_ValidatePendingMultiplayerLaunch(FString& errorText);
bool M_BeginPendingMultiplayerLaunch();
void M_ClearPendingMultiplayerLaunch();
bool M_ConsumePendingMultiplayerHardRestart();
