#include <string.h>

#include <vulkan/vulkan.h>

// Apps link the entry points directly; these look the same functions up by name, as the loader would.

typedef enum {
	SCOPE_GLOBAL,
	SCOPE_INSTANCE,
	SCOPE_DEVICE,
} Scope;

typedef struct {
	const char *name;
	PFN_vkVoidFunction function;
	Scope scope;
} Entry;

#define ENTRY(name, scope) { #name, (PFN_vkVoidFunction)name, scope }

static const Entry entries[] = {
	ENTRY(vkCreateInstance, SCOPE_GLOBAL),
	ENTRY(vkEnumerateInstanceExtensionProperties, SCOPE_GLOBAL),
	ENTRY(vkEnumerateInstanceLayerProperties, SCOPE_GLOBAL),
	ENTRY(vkGetInstanceProcAddr, SCOPE_GLOBAL),
	ENTRY(vkDestroyInstance, SCOPE_INSTANCE),
	ENTRY(vkEnumeratePhysicalDevices, SCOPE_INSTANCE),
	ENTRY(vkGetPhysicalDeviceProperties, SCOPE_INSTANCE),
	ENTRY(vkGetPhysicalDeviceFeatures, SCOPE_INSTANCE),
	ENTRY(vkGetPhysicalDeviceQueueFamilyProperties, SCOPE_INSTANCE),
	ENTRY(vkGetPhysicalDeviceMemoryProperties, SCOPE_INSTANCE),
	ENTRY(vkGetPhysicalDeviceFormatProperties, SCOPE_INSTANCE),
	ENTRY(vkEnumerateDeviceExtensionProperties, SCOPE_INSTANCE),
	ENTRY(vkEnumerateDeviceLayerProperties, SCOPE_INSTANCE),
	ENTRY(vkCreateDevice, SCOPE_INSTANCE),
	ENTRY(vkGetDeviceProcAddr, SCOPE_INSTANCE),
	ENTRY(vkGetPhysicalDeviceDisplayPropertiesKHR, SCOPE_INSTANCE),
	ENTRY(vkGetPhysicalDeviceDisplayPlanePropertiesKHR, SCOPE_INSTANCE),
	ENTRY(vkGetDisplayPlaneSupportedDisplaysKHR, SCOPE_INSTANCE),
	ENTRY(vkGetDisplayModePropertiesKHR, SCOPE_INSTANCE),
	ENTRY(vkCreateDisplayModeKHR, SCOPE_INSTANCE),
	ENTRY(vkGetDisplayPlaneCapabilitiesKHR, SCOPE_INSTANCE),
	ENTRY(vkCreateDisplayPlaneSurfaceKHR, SCOPE_INSTANCE),
	ENTRY(vkDestroySurfaceKHR, SCOPE_INSTANCE),
	ENTRY(vkGetPhysicalDeviceSurfaceSupportKHR, SCOPE_INSTANCE),
	ENTRY(vkGetPhysicalDeviceSurfaceCapabilitiesKHR, SCOPE_INSTANCE),
	ENTRY(vkGetPhysicalDeviceSurfaceFormatsKHR, SCOPE_INSTANCE),
	ENTRY(vkGetPhysicalDeviceSurfacePresentModesKHR, SCOPE_INSTANCE),
	ENTRY(vkDestroyDevice, SCOPE_DEVICE),
	ENTRY(vkGetDeviceQueue, SCOPE_DEVICE),
	ENTRY(vkDeviceWaitIdle, SCOPE_DEVICE),
	ENTRY(vkAllocateMemory, SCOPE_DEVICE),
	ENTRY(vkFreeMemory, SCOPE_DEVICE),
	ENTRY(vkMapMemory, SCOPE_DEVICE),
	ENTRY(vkUnmapMemory, SCOPE_DEVICE),
	ENTRY(vkFlushMappedMemoryRanges, SCOPE_DEVICE),
	ENTRY(vkInvalidateMappedMemoryRanges, SCOPE_DEVICE),
	ENTRY(vkCreateBuffer, SCOPE_DEVICE),
	ENTRY(vkDestroyBuffer, SCOPE_DEVICE),
	ENTRY(vkGetBufferMemoryRequirements, SCOPE_DEVICE),
	ENTRY(vkBindBufferMemory, SCOPE_DEVICE),
	ENTRY(vkCreateShaderModule, SCOPE_DEVICE),
	ENTRY(vkDestroyShaderModule, SCOPE_DEVICE),
	ENTRY(vkCreatePipelineLayout, SCOPE_DEVICE),
	ENTRY(vkDestroyPipelineLayout, SCOPE_DEVICE),
	ENTRY(vkCreateGraphicsPipelines, SCOPE_DEVICE),
	ENTRY(vkDestroyPipeline, SCOPE_DEVICE),
	ENTRY(vkCreateImageView, SCOPE_DEVICE),
	ENTRY(vkDestroyImageView, SCOPE_DEVICE),
	ENTRY(vkCreateRenderPass, SCOPE_DEVICE),
	ENTRY(vkDestroyRenderPass, SCOPE_DEVICE),
	ENTRY(vkCreateFramebuffer, SCOPE_DEVICE),
	ENTRY(vkDestroyFramebuffer, SCOPE_DEVICE),
	ENTRY(vkCreateCommandPool, SCOPE_DEVICE),
	ENTRY(vkDestroyCommandPool, SCOPE_DEVICE),
	ENTRY(vkResetCommandPool, SCOPE_DEVICE),
	ENTRY(vkAllocateCommandBuffers, SCOPE_DEVICE),
	ENTRY(vkFreeCommandBuffers, SCOPE_DEVICE),
	ENTRY(vkBeginCommandBuffer, SCOPE_DEVICE),
	ENTRY(vkEndCommandBuffer, SCOPE_DEVICE),
	ENTRY(vkResetCommandBuffer, SCOPE_DEVICE),
	ENTRY(vkCmdBeginRenderPass, SCOPE_DEVICE),
	ENTRY(vkCmdEndRenderPass, SCOPE_DEVICE),
	ENTRY(vkCmdBindPipeline, SCOPE_DEVICE),
	ENTRY(vkCmdBindVertexBuffers, SCOPE_DEVICE),
	ENTRY(vkCmdDraw, SCOPE_DEVICE),
	ENTRY(vkQueueSubmit, SCOPE_DEVICE),
	ENTRY(vkQueueWaitIdle, SCOPE_DEVICE),
	ENTRY(vkCreateFence, SCOPE_DEVICE),
	ENTRY(vkDestroyFence, SCOPE_DEVICE),
	ENTRY(vkResetFences, SCOPE_DEVICE),
	ENTRY(vkGetFenceStatus, SCOPE_DEVICE),
	ENTRY(vkWaitForFences, SCOPE_DEVICE),
	ENTRY(vkCreateSemaphore, SCOPE_DEVICE),
	ENTRY(vkDestroySemaphore, SCOPE_DEVICE),
	ENTRY(vkCreateSwapchainKHR, SCOPE_DEVICE),
	ENTRY(vkDestroySwapchainKHR, SCOPE_DEVICE),
	ENTRY(vkGetSwapchainImagesKHR, SCOPE_DEVICE),
	ENTRY(vkAcquireNextImageKHR, SCOPE_DEVICE),
	ENTRY(vkQueuePresentKHR, SCOPE_DEVICE),
};

static const Entry *find(const char *name)
{
	for (uint32_t i = 0; i < sizeof entries / sizeof entries[0]; i++) {
		if (!strcmp(name, entries[i].name))
			return &entries[i];
	}
	return NULL;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char *pName)
{
	const Entry *const entry = find(pName);
	if (!entry || (!instance && entry->scope != SCOPE_GLOBAL))
		return NULL;
	return entry->function;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char *pName)
{
	(void)device;
	const Entry *const entry = find(pName);
	return entry && entry->scope == SCOPE_DEVICE ? entry->function : NULL;
}
