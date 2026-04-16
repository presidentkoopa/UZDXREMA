#include "oxr_loader.h"
#include "cmdlib.h"
#include "common/engine/printf.h"

#ifdef DYN_OPENXR

FModule OpenXRModule{ "OpenXR" };

#define OXR_PROC(name) TReqProc<OpenXRModule, PFN_##name> name{#name};
#define OXR_OPT_PROC(name) TOptProc<OpenXRModule, PFN_##name> name{#name};

#include "oxr_procs.h"

#undef OXR_PROC
#undef OXR_OPT_PROC

#ifdef _WIN32
#define OPENXRLIB "openxr_loader.dll"
#elif defined(__APPLE__)
#define OPENXRLIB "libopenxr_loader.dylib"
#else
#define OPENXRLIB "libopenxr_loader.so"
#endif

#endif

static FString GetWindowsErrorString()
{
#ifdef _WIN32
	DWORD error = GetLastError();
	if (error == 0)
	{
		return FString("unknown error");
	}

	LPSTR message = nullptr;
	DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
	DWORD len = FormatMessageA(flags, nullptr, error, 0, (LPSTR)&message, 0, nullptr);
	FString result;
	if (len > 0 && message != nullptr)
	{
		result = message;
		LocalFree(message);
	}
	else
	{
		result.AppendFormat("error %lu", error);
	}
	return result;
#else
	return FString("non-Windows");
#endif
}

bool IsOpenXRPresent()
{
#ifndef USE_OPENXR
	return false;
#elif !defined DYN_OPENXR
	return true;
#else
	static bool cached_result = false;
	static bool done = false;

	if (!done)
	{
		done = true;
		FString libname = NicePath("$PROGDIR/" OPENXRLIB);
		Printf("OpenXR loader: probing '%s' then '%s'.\n", libname.GetChars(), OPENXRLIB);
		auto probe = LoadLibraryA(libname.GetChars());
		if (probe != nullptr)
		{
			FreeLibrary(probe);
			Printf("OpenXR loader: probe load succeeded.\n");
		}
		else
		{
			Printf("OpenXR loader: probe load failed: %s\n", GetWindowsErrorString().GetChars());
		}
		cached_result = OpenXRModule.Load({ libname.GetChars(), OPENXRLIB });
		Printf("OpenXR loader: present=%d.\n", cached_result ? 1 : 0);
	}
	return cached_result;
#endif
}
