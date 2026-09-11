
#include "vulkandevice.h"
#include "vulkanobjects.h"
#include "vulkancompatibledevice.h"
#include <algorithm>
#include <cstring>
#include <set>
#include <string>
#include <cstdio>
#include <vector>

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
	// RS FORK -- see VulkanDevice::DescribeDeviceLoss.
	if (SupportsExtension(VK_EXT_DEVICE_FAULT_EXTENSION_NAME))
	{
		*next = &EnabledFeatures.Fault;
		next = &EnabledFeatures.Fault.pNext;
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

// RS FORK -- see the declaration. Written to run on a DEAD device: every call
// here is a query the spec allows after VK_ERROR_DEVICE_LOST, nothing is
// created through the device, and a failure is reported as text rather than
// thrown, because the caller is already on its way to an exception.
static const char* FaultAddressTypeName(VkDeviceFaultAddressTypeEXT type)
{
	switch (type)
	{
	case VK_DEVICE_FAULT_ADDRESS_TYPE_NONE_EXT: return "no address";
	case VK_DEVICE_FAULT_ADDRESS_TYPE_READ_INVALID_EXT: return "invalid READ";
	case VK_DEVICE_FAULT_ADDRESS_TYPE_WRITE_INVALID_EXT: return "invalid WRITE";
	case VK_DEVICE_FAULT_ADDRESS_TYPE_EXECUTE_INVALID_EXT: return "invalid EXECUTE";
	case VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_UNKNOWN_EXT: return "instruction pointer (unknown)";
	case VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_INVALID_EXT: return "instruction pointer (invalid)";
	case VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_FAULT_EXT: return "instruction pointer (faulting)";
	default: return "unrecognised address type";
	}
}

std::string VulkanDevice::DescribeDeviceLoss() const
{
	std::string out;
	char line[1024];

	// WHERE THE GPU GOT TO. Each queue reports the last checkpoint that reached
	// the TOP of the pipe (started) and the last that reached the BOTTOM
	// (finished). Whatever killed it came after the last "finished" and no
	// later than the last "started".
	if (SupportsExtension(VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME) && vkGetQueueCheckpointDataNV)
	{
		const VkQueue queues[2] = { GraphicsQueue, PresentQueue };
		const char* names[2] = { "graphics", "present" };
		for (int q = 0; q < 2; q++)
		{
			if (queues[q] == VK_NULL_HANDLE || (q == 1 && queues[1] == queues[0]))
				continue;

			uint32_t count = 0;
			vkGetQueueCheckpointDataNV(queues[q], &count, nullptr);
			if (count == 0)
			{
				snprintf(line, sizeof(line), "  %s queue: no checkpoint reached\n", names[q]);
				out += line;
				continue;
			}

			std::vector<VkCheckpointDataNV> data(count);
			for (auto& d : data)
			{
				d.sType = VK_STRUCTURE_TYPE_CHECKPOINT_DATA_NV;
				d.pNext = nullptr;
			}
			vkGetQueueCheckpointDataNV(queues[q], &count, data.data());
			for (uint32_t i = 0; i < count; i++)
			{
				const char* stage =
					(data[i].stage & VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT) ? "last STARTED " :
					(data[i].stage & VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT) ? "last FINISHED" : "last reached ";
				// The marker is whatever was passed to vkCmdSetCheckpointNV.
				// This library's convention: a NUL-terminated string that
				// outlives the device.
				const char* marker = data[i].pCheckpointMarker ? (const char*)data[i].pCheckpointMarker : "(none)";
				snprintf(line, sizeof(line), "  %s queue, %s: %s\n", names[q], stage, marker);
				out += line;
			}
		}
	}
	else
	{
		out += "  GPU checkpoints were off, so there is no record of which pass was running.\n";
	}

	// WHAT THE DRIVER SAYS.
	if (SupportsExtension(VK_EXT_DEVICE_FAULT_EXTENSION_NAME) && EnabledFeatures.Fault.deviceFault && vkGetDeviceFaultInfoEXT)
	{
		VkDeviceFaultCountsEXT counts = { VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT };
		VkResult result = vkGetDeviceFaultInfoEXT(device, &counts, nullptr);
		if (result == VK_SUCCESS || result == VK_INCOMPLETE)
		{
			std::vector<VkDeviceFaultAddressInfoEXT> addresses(counts.addressInfoCount);
			std::vector<VkDeviceFaultVendorInfoEXT> vendor(counts.vendorInfoCount);
			VkDeviceFaultInfoEXT info = { VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT };
			info.pAddressInfos = addresses.empty() ? nullptr : addresses.data();
			info.pVendorInfos = vendor.empty() ? nullptr : vendor.data();
			info.pVendorBinaryData = nullptr;
			counts.vendorBinarySize = 0;   // the binary blob is for vendor tools, not a log
			result = vkGetDeviceFaultInfoEXT(device, &counts, &info);
			if (result == VK_SUCCESS || result == VK_INCOMPLETE)
			{
				snprintf(line, sizeof(line), "  driver: %s\n", info.description[0] ? info.description : "(no description)");
				out += line;
				for (uint32_t i = 0; i < counts.addressInfoCount && i < addresses.size(); i++)
				{
					snprintf(line, sizeof(line), "  fault: %s at 0x%016llx (+/- 0x%llx)\n",
						FaultAddressTypeName(addresses[i].addressType),
						(unsigned long long)addresses[i].reportedAddress,
						(unsigned long long)addresses[i].addressPrecision);
					out += line;
				}
				for (uint32_t i = 0; i < counts.vendorInfoCount && i < vendor.size(); i++)
				{
					snprintf(line, sizeof(line), "  vendor: %s (code 0x%llx, data 0x%llx)\n",
						vendor[i].description,
						(unsigned long long)vendor[i].vendorFaultCode,
						(unsigned long long)vendor[i].vendorFaultData);
					out += line;
				}
				if (counts.addressInfoCount == 0 && counts.vendorInfoCount == 0)
					out += "  (no fault addresses reported -- more likely a hang or timeout than a bad memory access)\n";
			}
			else
			{
				out += "  driver fault report failed: " + VkResultToString(result) + "\n";
			}
		}
		else
		{
			out += "  driver fault report unavailable: " + VkResultToString(result) + "\n";
		}
	}
	else
	{
		out += "  VK_EXT_device_fault is not available here, so the driver was not asked.\n";
	}
	return out;
}
