#include <string.h>

#include <vulkan/vulkan.h>

#include "blit.h"
#include "host.h"
#include "objects.h"
#include "physical_device.h"
#include "xsdk/gpu.h"
#include "xsdk/log.h"
#include "xsdk/splash.h"

// The GPU has one owner, so there's one device at a time.
static bool device_exists;

static bool features_off(const VkPhysicalDeviceFeatures *features)
{
	if (!features)
		return true;
	VkBool32 flags[sizeof *features / sizeof(VkBool32)];
	memcpy(flags, features, sizeof flags);
	for (uint32_t i = 0; i < sizeof flags / sizeof flags[0]; i++) {
		if (flags[i])
			return false;
	}
	return true;
}

static VkResult check_create_info(const VkDeviceCreateInfo *info)
{
	if (info->enabledLayerCount)
		return VK_ERROR_LAYER_NOT_PRESENT;
	for (uint32_t i = 0; i < info->enabledExtensionCount; i++) {
		if (!v4x_physical_device_has_extension(info->ppEnabledExtensionNames[i]))
			return VK_ERROR_EXTENSION_NOT_PRESENT;
	}
	if (!features_off(info->pEnabledFeatures))
		return VK_ERROR_FEATURE_NOT_PRESENT;
	if (info->queueCreateInfoCount != 1 || info->pQueueCreateInfos[0].queueFamilyIndex != 0 ||
		info->pQueueCreateInfos[0].queueCount != 1)
		return VK_ERROR_INITIALIZATION_FAILED;
	return device_exists ? VK_ERROR_TOO_MANY_OBJECTS : VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateDevice(VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo *pCreateInfo,
	const VkAllocationCallbacks *pAllocator, VkDevice *pDevice)
{
	const VkResult result = check_create_info(pCreateInfo);
	if (result != VK_SUCCESS)
		return result;
	const VkInstance instance = physicalDevice->instance;
	const VkAllocationCallbacks *const allocator =
		pAllocator ? pAllocator : (instance->has_allocator ? &instance->allocator : NULL);
	struct VkDevice_T *const device = v4x_host_alloc(allocator, sizeof *device, VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
	if (!device)
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	const NTSTATUS status = xsdk_gpu_open();
	if (!NT_SUCCESS(status)) {
		xsdk_log_write("vulkan4xenon: xsdk_gpu_open failed: 0x%08X", (unsigned)status);
		v4x_host_free(allocator, device);
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	// The boot logo; without a display, as in Xenia, the device works without it.
	const NTSTATUS splash = xsdk_splash_begin();
	if (!NT_SUCCESS(splash))
		xsdk_log_write("vulkan4xenon: no boot logo: 0x%08X", (unsigned)splash);
	VkResult made = v4x_blit_create(&device->queue.blit);
	if (made == VK_SUCCESS) {
		made = v4x_ring_create(&device->queue.ring);
		if (made != VK_SUCCESS)
			v4x_blit_destroy(&device->queue.blit);
	}
	if (made != VK_SUCCESS) {
		xsdk_splash_end();
		const NTSTATUS closed = xsdk_gpu_close();
		if (!NT_SUCCESS(closed))
			xsdk_log_write("vulkan4xenon: xsdk_gpu_close failed: 0x%08X", (unsigned)closed);
		v4x_host_free(allocator, device);
		return made;
	}
	if (allocator) {
		device->allocator = *allocator;
		device->has_allocator = true;
	}
	device->queue.device = device;
	// Resolves clear depth to far and stencil to 0 until a render pass says otherwise.
	xsdk_gpu_set_depth_clear(1.0f, 0);
	device->queue.depth_clear = 1.0f;
	device->queue.stencil_clear = 0;
	device_exists = true;
	*pDevice = device;
	return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyDevice(VkDevice device, const VkAllocationCallbacks *pAllocator)
{
	if (!device)
		return;
	v4x_ring_destroy(&device->queue.ring);
	v4x_blit_destroy(&device->queue.blit);
	xsdk_splash_end();
	const NTSTATUS status = xsdk_gpu_close();
	if (!NT_SUCCESS(status))
		xsdk_log_write("vulkan4xenon: xsdk_gpu_close failed: 0x%08X", (unsigned)status);
	device_exists = false;
	v4x_host_free(v4x_host_device_allocator(device, pAllocator), device);
}

VKAPI_ATTR void VKAPI_CALL vkGetDeviceQueue(
	VkDevice device, uint32_t queueFamilyIndex, uint32_t queueIndex, VkQueue *pQueue)
{
	(void)queueFamilyIndex;
	(void)queueIndex;
	*pQueue = &device->queue;
}

VKAPI_ATTR VkResult VKAPI_CALL vkDeviceWaitIdle(VkDevice device)
{
	return vkQueueWaitIdle(&device->queue);
}
