#include <string.h>

#include <vulkan/vulkan.h>

#include "host.h"
#include "objects.h"

static const VkExtensionProperties instance_extensions[] = {
	{ VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_SURFACE_SPEC_VERSION },
	{ VK_KHR_DISPLAY_EXTENSION_NAME, VK_KHR_DISPLAY_SPEC_VERSION },
};

static bool supported(const char *name)
{
	for (uint32_t i = 0; i < sizeof instance_extensions / sizeof instance_extensions[0]; i++) {
		if (!strcmp(name, instance_extensions[i].extensionName))
			return true;
	}
	return false;
}

VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceExtensionProperties(
	const char *pLayerName, uint32_t *pPropertyCount, VkExtensionProperties *pProperties)
{
	if (pLayerName)
		return VK_ERROR_LAYER_NOT_PRESENT;
	return v4x_host_enumerate(pPropertyCount, pProperties, instance_extensions,
		sizeof instance_extensions / sizeof instance_extensions[0], sizeof instance_extensions[0]);
}

VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceLayerProperties(
	uint32_t *pPropertyCount, VkLayerProperties *pProperties)
{
	(void)pProperties;
	*pPropertyCount = 0;
	return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateInstance(
	const VkInstanceCreateInfo *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkInstance *pInstance)
{
	// A 1.0 implementation refuses any later version an app asks for (Vulkan 1.0 spec, VkApplicationInfo).
	const VkApplicationInfo *const app = pCreateInfo->pApplicationInfo;
	if (app && app->apiVersion > VK_API_VERSION_1_0)
		return VK_ERROR_INCOMPATIBLE_DRIVER;
	if (pCreateInfo->enabledLayerCount)
		return VK_ERROR_LAYER_NOT_PRESENT;
	for (uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; i++) {
		if (!supported(pCreateInfo->ppEnabledExtensionNames[i]))
			return VK_ERROR_EXTENSION_NOT_PRESENT;
	}
	struct VkInstance_T *const instance =
		v4x_host_alloc(pAllocator, sizeof *instance, VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
	if (!instance)
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	if (pAllocator) {
		instance->allocator = *pAllocator;
		instance->has_allocator = true;
	}
	instance->physical_device.instance = instance;
	*pInstance = instance;
	return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyInstance(VkInstance instance, const VkAllocationCallbacks *pAllocator)
{
	if (!instance)
		return;
	v4x_host_free(pAllocator, instance);
}

VKAPI_ATTR VkResult VKAPI_CALL vkEnumeratePhysicalDevices(
	VkInstance instance, uint32_t *pPhysicalDeviceCount, VkPhysicalDevice *pPhysicalDevices)
{
	const VkPhysicalDevice device = &instance->physical_device;
	return v4x_host_enumerate(pPhysicalDeviceCount, pPhysicalDevices, &device, 1, sizeof device);
}
