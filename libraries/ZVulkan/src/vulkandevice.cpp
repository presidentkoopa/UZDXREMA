
#include "vulkandevice.h"
#include "vulkanobjects.h"
#include "vulkancompatibledevice.h"
#include <algorithm>
#include <cstring>
#include <set>
#include <string>

static int CreateOrModifyQueueInfo(std::vector<VkDeviceQueueCreateInfo>& infos, uint32_t family, float* priorities)
{
	for (auto& info : infos)
	{
		if (info.queueFamilyIndex == family)
		{
			info.queueCount++;
			return info.queueCount - 1;
		}
	}

	VkDeviceQueueCreateInfo queueCreateInfo = {};
	queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	queueCreateInfo.queueFamilyIndex = family;
	queueCreateInfo.queueCount = 1;
	queueCreateInfo.pQueuePriorities = priorities;
	infos.push_back(queueCreateInfo);
	return 0;
}

VulkanDevice::VulkanDevice(std::shared_ptr<VulkanInstance> instance, std::shared_ptr<VulkanSurface> surface, const VulkanCompatibleDevice& selectedDevice, int numUploadSlots, int flags) : Instance(instance), Surface(surface)
{
	PhysicalDevice = *selectedDevice.Device;
	EnabledDeviceExtensions = selectedDevice.EnabledDeviceExtensions;
	EnabledFeatures = selectedDevice.EnabledFeatures;

	GraphicsFamily = selectedDevice.GraphicsFamily;
	PresentFamily = selectedDevice.PresentFamily;
	UploadFamily = selectedDevice.UploadFamily;
	UploadFamilySupportsGraphics = selectedDevice.UploadFamilySupportsGraphics;
	GraphicsTimeQueries = selectedDevice.GraphicsTimeQueries;
	DebugLayerActive = instance->DebugLayerActive;

	if (flags & VK_DEVICE_FLAG_FORCE_EXCLUSIVE_PRESENT)
	{
		PresentFamily = -2;
	}

	if (UploadFamily >= 0 && UploadFamily < (int)selectedDevice.Device->QueueFamilies.size())
	{
		int reservedQueues = (UploadFamily == GraphicsFamily ? 1 : 0) + (PresentFamily == UploadFamily ? 1 : 0);
		UploadQueuesSupported = std::max(0, (int)selectedDevice.Device->QueueFamilies[UploadFamily].queueCount - reservedQueues);
	}

	try
	{
		CreateDevice(numUploadSlots);
		CreateAllocator();
	}
	catch (...)
	{
		ReleaseResources();
		throw;
	}
}

VulkanDevice::~VulkanDevice()
{
	ReleaseResources();
}

bool VulkanDevice::SupportsExtension(const char* ext) const
{
	return
		EnabledDeviceExtensions.find(ext) != EnabledDeviceExtensions.end() ||
		Instance->EnabledExtensions.find(ext) != Instance->EnabledExtensions.end();
}

void VulkanDevice::CreateAllocator()
{
	VmaAllocatorCreateInfo allocinfo = {};
	allocinfo.vulkanApiVersion = Instance->ApiVersion;
	if (SupportsExtension(VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME) && SupportsExtension(VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME))
		allocinfo.flags |= VMA_ALLOCATOR_CREATE_KHR_DEDICATED_ALLOCATION_BIT;
	if (SupportsExtension(VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME))
		allocinfo.flags |= VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
	allocinfo.physicalDevice = PhysicalDevice.Device;
	allocinfo.device = device;
	allocinfo.instance = Instance->Instance;
	allocinfo.preferredLargeHeapBlockSize = 64 * 1024 * 1024;
	if (vmaCreateAllocator(&allocinfo, &allocator) != VK_SUCCESS)
		VulkanError("Unable to create allocator");
}

void VulkanDevice::CreateDevice(int numUploadSlots)
{
	float queuePriority[] = { 1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f };
	std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;

	int graphicsFamilySlot = GraphicsFamily >= 0 ? CreateOrModifyQueueInfo(queueCreateInfos, GraphicsFamily, queuePriority) : -1;
	int presentFamilySlot = PresentFamily >= 0 ? CreateOrModifyQueueInfo(queueCreateInfos, PresentFamily, queuePriority) : -1;
	std::vector<int> uploadFamilySlots;
	int requestedUploadQueues = numUploadSlots >= 0 ? numUploadSlots : 2;
	for (int i = 0; i < requestedUploadQueues && i < UploadQueuesSupported; i++)
	{
		uploadFamilySlots.push_back(CreateOrModifyQueueInfo(queueCreateInfos, UploadFamily, queuePriority));
	}

	std::vector<const char*> extensionNames;
	extensionNames.reserve(EnabledDeviceExtensions.size());
	for (const auto& name : EnabledDeviceExtensions)
		extensionNames.push_back(name.c_str());

	VkDeviceCreateInfo deviceCreateInfo = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
	deviceCreateInfo.queueCreateInfoCount = (uint32_t)queueCreateInfos.size();
	deviceCreateInfo.pQueueCreateInfos = queueCreateInfos.data();
	deviceCreateInfo.enabledExtensionCount = (uint32_t)extensionNames.size();
	deviceCreateInfo.ppEnabledExtensionNames = extensionNames.data();
	deviceCreateInfo.enabledLayerCount = 0;

	VkPhysicalDeviceFeatures2 deviceFeatures2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
	deviceFeatures2.features = EnabledFeatures.Features;

	const bool canUseFeatures2 =
		Instance->ApiVersion >= VK_API_VERSION_1_1 ||
		SupportsExtension(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
	void** next = const_cast<void**>(&deviceCreateInfo.pNext);
	if (canUseFeatures2)
	{
		*next = &deviceFeatures2;
		next = &deviceFeatures2.pNext;
	}
	else // vulkan 1.0 specified features in a different way
	{
		deviceCreateInfo.pEnabledFeatures = &deviceFeatures2.features;
	}

	if (Instance->ApiVersion >= VK_API_VERSION_1_1 || SupportsExtension(VK_KHR_MULTIVIEW_EXTENSION_NAME))
	{
		*next = &EnabledFeatures.Multiview;
		next = &EnabledFeatures.Multiview.pNext;
	}
	if (SupportsExtension(VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME))
	{
		*next = &EnabledFeatures.BufferDeviceAddress;
		next = &EnabledFeatures.BufferDeviceAddress.pNext;
	}
	if (SupportsExtension(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME))
	{
		*next = &EnabledFeatures.AccelerationStructure;
		next = &EnabledFeatures.AccelerationStructure.pNext;
	}
	if (SupportsExtension(VK_KHR_RAY_QUERY_EXTENSION_NAME))
	{
		*next = &EnabledFeatures.RayQuery;
		next = &EnabledFeatures.RayQuery.pNext;
	}
	if (SupportsExtension(VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME))
	{
		*next = &EnabledFeatures.DescriptorIndexing;
		next = &EnabledFeatures.DescriptorIndexing.pNext;
	}

	VkResult result = vkCreateDevice(PhysicalDevice.Device, &deviceCreateInfo, nullptr, &device);
	CheckVulkanError(result, "Could not create vulkan device");

	volkLoadDevice(device);

	if (GraphicsFamily >= 0 && graphicsFamilySlot >= 0)
		vkGetDeviceQueue(device, GraphicsFamily, graphicsFamilySlot, &GraphicsQueue);
	if (PresentFamily >= 0 && presentFamilySlot >= 0)
		vkGetDeviceQueue(device, PresentFamily, presentFamilySlot, &PresentQueue);
	else if (PresentFamily == -2)
		vkGetDeviceQueue(device, GraphicsFamily, graphicsFamilySlot, &PresentQueue);

	for (int i = 0; i < (int)uploadFamilySlots.size(); i++)
	{
		VulkanUploadSlot slot = {};
		slot.queueFamily = UploadFamily;
		slot.queueIndex = uploadFamilySlots[i];
		slot.familySupportsGraphics = UploadFamilySupportsGraphics;
		vkGetDeviceQueue(device, UploadFamily, uploadFamilySlots[i], &slot.queue);
		if (slot.queue != VK_NULL_HANDLE)
		{
			uploadQueues.push_back(slot);
		}
	}
}

void VulkanDevice::ReleaseResources()
{
	if (device)
		vkDeviceWaitIdle(device);

	if (allocator)
		vmaDestroyAllocator(allocator);

	if (device)
		vkDestroyDevice(device, nullptr);
	device = nullptr;
}

void VulkanDevice::SetObjectName(const char* name, uint64_t handle, VkObjectType type)
{
	if (!DebugLayerActive) return;

	VkDebugUtilsObjectNameInfoEXT info = {};
	info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
	info.objectHandle = handle;
	info.objectType = type;
	info.pObjectName = name;
	vkSetDebugUtilsObjectNameEXT(device, &info);
}
