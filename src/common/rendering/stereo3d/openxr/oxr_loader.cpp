#include "oxr_loader.h"
#include "cmdlib.h"
#include "common/engine/printf.h"
#include <cstring>
#include <vector>
#include <string>

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
		Printf("OpenXR loader: loading '%s' then '%s'.\n", libname.GetChars(), OPENXRLIB);
		cached_result = OpenXRModule.Load({ libname.GetChars(), OPENXRLIB });
		if (!cached_result)
		{
			Printf("OpenXR loader: load failed: %s\n", GetWindowsErrorString().GetChars());
		}
		Printf("OpenXR loader: present=%d.\n", cached_result ? 1 : 0);
	}
	return cached_result;
#endif
}

namespace
{
	std::vector<std::string> SplitSpaceSeparatedList(const char* list)
	{
		std::vector<std::string> result;
		if (list == nullptr)
			return result;

		const char* cursor = list;
		while (*cursor != '\0')
		{
			while (*cursor == ' ')
				++cursor;
			if (*cursor == '\0')
				break;

			const char* end = cursor;
			while (*end != '\0' && *end != ' ')
				++end;
			result.emplace_back(cursor, end);
			cursor = end;
		}
		return result;
	}

	struct OpenXRTmpBootstrapContext
	{
		XrInstance instance = XR_NULL_HANDLE;
		XrSystemId systemId = XR_NULL_SYSTEM_ID;
		bool supportsVulkanEnable = false;
		bool supportsVulkanEnable2 = false;
		PFN_xrGetVulkanInstanceExtensionsKHR getVulkanInstanceExtensionsKHR = nullptr;
		PFN_xrGetVulkanDeviceExtensionsKHR getVulkanDeviceExtensionsKHR = nullptr;
		PFN_xrGetVulkanGraphicsRequirementsKHR getVulkanGraphicsRequirementsKHR = nullptr;
		PFN_xrGetVulkanGraphicsRequirements2KHR getVulkanGraphicsRequirements2KHR = nullptr;
		PFN_xrGetVulkanGraphicsDeviceKHR getVulkanGraphicsDeviceKHR = nullptr;
		PFN_xrGetVulkanGraphicsDevice2KHR getVulkanGraphicsDevice2KHR = nullptr;
	};

	void DestroyBootstrapContext(OpenXRTmpBootstrapContext& ctx)
	{
		if (ctx.instance != XR_NULL_HANDLE)
			xrDestroyInstance(ctx.instance);
		ctx = {};
	}

	bool LoadInstanceProc(XrInstance instance, const char* name, PFN_xrVoidFunction* out)
	{
		*out = nullptr;
		return XR_SUCCEEDED(xrGetInstanceProcAddr(instance, name, out)) && *out != nullptr;
	}

	bool CreateBootstrapContext(OpenXRTmpBootstrapContext& ctx, OpenXRVulkanBootstrapInfo* outInfo = nullptr)
	{
		if (!IsOpenXRPresent())
			return false;

		uint32_t extensionCount = 0;
		if (XR_FAILED(xrEnumerateInstanceExtensionProperties(nullptr, 0, &extensionCount, nullptr)) || extensionCount == 0)
			return false;

		std::vector<XrExtensionProperties> extensions(extensionCount, { XR_TYPE_EXTENSION_PROPERTIES });
		if (XR_FAILED(xrEnumerateInstanceExtensionProperties(nullptr, extensionCount, &extensionCount, extensions.data())))
			return false;

		auto hasExtension = [&](const char* name)
		{
			for (const auto& ext : extensions)
			{
				if (strcmp(ext.extensionName, name) == 0)
					return true;
			}
			return false;
		};

		ctx.supportsVulkanEnable = hasExtension(XR_KHR_VULKAN_ENABLE_EXTENSION_NAME);
		ctx.supportsVulkanEnable2 = hasExtension(XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME);
		if (!ctx.supportsVulkanEnable && !ctx.supportsVulkanEnable2)
			return false;

		std::vector<const char*> enabledExtensions;
		if (ctx.supportsVulkanEnable)
			enabledExtensions.push_back(XR_KHR_VULKAN_ENABLE_EXTENSION_NAME);
		if (ctx.supportsVulkanEnable2)
			enabledExtensions.push_back(XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME);

		XrApplicationInfo appInfo{};
		appInfo.apiVersion = XR_API_VERSION_1_0;
		appInfo.applicationVersion = 1;
		appInfo.engineVersion = 1;
		strncpy(appInfo.applicationName, "DoomXR", sizeof(appInfo.applicationName) - 1);
		strncpy(appInfo.engineName, "DoomXR", sizeof(appInfo.engineName) - 1);

		XrInstanceCreateInfo createInfo{ XR_TYPE_INSTANCE_CREATE_INFO };
		createInfo.applicationInfo = appInfo;
		createInfo.enabledExtensionCount = (uint32_t)enabledExtensions.size();
		createInfo.enabledExtensionNames = enabledExtensions.data();
		if (XR_FAILED(xrCreateInstance(&createInfo, &ctx.instance)))
			return false;

		XrSystemGetInfo systemInfo{ XR_TYPE_SYSTEM_GET_INFO };
		systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
		if (XR_FAILED(xrGetSystem(ctx.instance, &systemInfo, &ctx.systemId)))
		{
			DestroyBootstrapContext(ctx);
			return false;
		}

		LoadInstanceProc(ctx.instance, "xrGetVulkanInstanceExtensionsKHR", reinterpret_cast<PFN_xrVoidFunction*>(&ctx.getVulkanInstanceExtensionsKHR));
		LoadInstanceProc(ctx.instance, "xrGetVulkanDeviceExtensionsKHR", reinterpret_cast<PFN_xrVoidFunction*>(&ctx.getVulkanDeviceExtensionsKHR));
		LoadInstanceProc(ctx.instance, "xrGetVulkanGraphicsRequirementsKHR", reinterpret_cast<PFN_xrVoidFunction*>(&ctx.getVulkanGraphicsRequirementsKHR));
		LoadInstanceProc(ctx.instance, "xrGetVulkanGraphicsRequirements2KHR", reinterpret_cast<PFN_xrVoidFunction*>(&ctx.getVulkanGraphicsRequirements2KHR));
		LoadInstanceProc(ctx.instance, "xrGetVulkanGraphicsDeviceKHR", reinterpret_cast<PFN_xrVoidFunction*>(&ctx.getVulkanGraphicsDeviceKHR));
		LoadInstanceProc(ctx.instance, "xrGetVulkanGraphicsDevice2KHR", reinterpret_cast<PFN_xrVoidFunction*>(&ctx.getVulkanGraphicsDevice2KHR));

		if (outInfo != nullptr)
		{
			outInfo->available = true;
			outInfo->supportsVulkanEnable = ctx.supportsVulkanEnable;
			outInfo->supportsVulkanEnable2 = ctx.supportsVulkanEnable2;

			XrGraphicsRequirementsVulkanKHR requirements{ XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR };
			if (ctx.getVulkanGraphicsRequirements2KHR && XR_SUCCEEDED(ctx.getVulkanGraphicsRequirements2KHR(ctx.instance, ctx.systemId, &requirements)))
			{
				outInfo->minApiVersionSupported = requirements.minApiVersionSupported;
				outInfo->maxApiVersionSupported = requirements.maxApiVersionSupported;
			}
			else if (ctx.getVulkanGraphicsRequirementsKHR && XR_SUCCEEDED(ctx.getVulkanGraphicsRequirementsKHR(ctx.instance, ctx.systemId, &requirements)))
			{
				outInfo->minApiVersionSupported = requirements.minApiVersionSupported;
				outInfo->maxApiVersionSupported = requirements.maxApiVersionSupported;
			}

			if (ctx.getVulkanInstanceExtensionsKHR)
			{
				uint32_t size = 0;
				if (XR_SUCCEEDED(ctx.getVulkanInstanceExtensionsKHR(ctx.instance, ctx.systemId, 0, &size, nullptr)) && size > 0)
				{
					std::vector<char> buffer(size, '\0');
					if (XR_SUCCEEDED(ctx.getVulkanInstanceExtensionsKHR(ctx.instance, ctx.systemId, size, &size, buffer.data())))
						outInfo->requiredInstanceExtensions = SplitSpaceSeparatedList(buffer.data());
				}
			}

			if (ctx.getVulkanDeviceExtensionsKHR)
			{
				uint32_t size = 0;
				if (XR_SUCCEEDED(ctx.getVulkanDeviceExtensionsKHR(ctx.instance, ctx.systemId, 0, &size, nullptr)) && size > 0)
				{
					std::vector<char> buffer(size, '\0');
					if (XR_SUCCEEDED(ctx.getVulkanDeviceExtensionsKHR(ctx.instance, ctx.systemId, size, &size, buffer.data())))
						outInfo->requiredDeviceExtensions = SplitSpaceSeparatedList(buffer.data());
				}
			}
		}

		return true;
	}
}

bool QueryOpenXRVulkanBootstrapInfo(OpenXRVulkanBootstrapInfo& outInfo)
{
	static bool cached = false;
	static bool cachedOk = false;
	static OpenXRVulkanBootstrapInfo cachedInfo;

	if (!cached)
	{
		cached = true;
		cachedInfo = {};
		OpenXRTmpBootstrapContext ctx;
		cachedOk = CreateBootstrapContext(ctx, &cachedInfo);
		DestroyBootstrapContext(ctx);
	}

	outInfo = cachedInfo;
	return cachedOk;
}

bool QueryOpenXRVulkanPreferredPhysicalDevice(VkInstance instance, VkPhysicalDevice& outPhysicalDevice)
{
	outPhysicalDevice = VK_NULL_HANDLE;
	OpenXRTmpBootstrapContext ctx;
	if (!CreateBootstrapContext(ctx, nullptr))
		return false;

	bool ok = false;
	if (ctx.getVulkanGraphicsDevice2KHR)
	{
		XrVulkanGraphicsDeviceGetInfoKHR getInfo{ XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR };
		getInfo.systemId = ctx.systemId;
		getInfo.vulkanInstance = instance;
		ok = XR_SUCCEEDED(ctx.getVulkanGraphicsDevice2KHR(ctx.instance, &getInfo, &outPhysicalDevice));
	}
	else if (ctx.getVulkanGraphicsDeviceKHR)
	{
		ok = XR_SUCCEEDED(ctx.getVulkanGraphicsDeviceKHR(ctx.instance, ctx.systemId, instance, &outPhysicalDevice));
	}

	DestroyBootstrapContext(ctx);
	return ok && outPhysicalDevice != VK_NULL_HANDLE;
}
