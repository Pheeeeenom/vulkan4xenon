#include "harness.h"

#include <string.h>

#include "tiled_index.h"
#include "xsdk/log.h"
#include "xsdk/present.h"

static const char *const instance_extensions[] = { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_DISPLAY_EXTENSION_NAME };
static const char *const device_extensions[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME,
	VK_KHR_SAMPLER_MIRROR_CLAMP_TO_EDGE_EXTENSION_NAME };

bool harness_ok(const char *call, VkResult result)
{
	if (result != VK_SUCCESS)
		xsdk_log_write("%s returned %d", call, (int)result);
	return result == VK_SUCCESS;
}

VkResult harness_create_device(Harness *harness)
{
	const VkApplicationInfo app = {
		.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
		.pApplicationName = "vulkan4xenon tests",
		.apiVersion = VK_API_VERSION_1_0,
	};
	const VkInstanceCreateInfo instance_info = {
		.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
		.pApplicationInfo = &app,
		.enabledExtensionCount = sizeof instance_extensions / sizeof instance_extensions[0],
		.ppEnabledExtensionNames = instance_extensions,
	};
	VkResult result = vkCreateInstance(&instance_info, NULL, &harness->instance);
	if (!harness_ok("vkCreateInstance", result))
		return result;
	uint32_t count = 1;
	result = vkEnumeratePhysicalDevices(harness->instance, &count, &harness->physical_device);
	if (!harness_ok("vkEnumeratePhysicalDevices", result))
		return result;
	const float priority = 1.0f;
	const VkDeviceQueueCreateInfo queue_info = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
		.queueFamilyIndex = 0,
		.queueCount = 1,
		.pQueuePriorities = &priority,
	};
	// The features the device has: indirect draws' first instance for test_instancing, independent blending for
	// test_targets, fill modes for test_fill_modes, large points for test_point_sprites, sampler anisotropy for
	// test_anisotropy and clip distances for test_clip_distances.
	const VkPhysicalDeviceFeatures features = { .drawIndirectFirstInstance = VK_TRUE,
		.independentBlend = VK_TRUE,
		.fillModeNonSolid = VK_TRUE,
		.largePoints = VK_TRUE,
		.samplerAnisotropy = VK_TRUE,
		.shaderClipDistance = VK_TRUE };
	const VkDeviceCreateInfo device_info = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
		.queueCreateInfoCount = 1,
		.pQueueCreateInfos = &queue_info,
		.enabledExtensionCount = sizeof device_extensions / sizeof device_extensions[0],
		.ppEnabledExtensionNames = device_extensions,
		.pEnabledFeatures = &features,
	};
	result = vkCreateDevice(harness->physical_device, &device_info, NULL, &harness->device);
	if (!harness_ok("vkCreateDevice", result))
		return result;
	vkGetDeviceQueue(harness->device, 0, 0, &harness->queue);
	const VkCommandPoolCreateInfo pool_info = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
	};
	result = vkCreateCommandPool(harness->device, &pool_info, NULL, &harness->pool);
	if (!harness_ok("vkCreateCommandPool", result))
		return result;
	const VkFenceCreateInfo fence_info = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
	return vkCreateFence(harness->device, &fence_info, NULL, &harness->fence);
}

// The display's one mode on plane 0, as an app finds it through VK_KHR_display.
static VkResult create_surface(Harness *harness)
{
	uint32_t count = 1;
	VkDisplayPropertiesKHR display;
	VkResult result = vkGetPhysicalDeviceDisplayPropertiesKHR(harness->physical_device, &count, &display);
	if (!harness_ok("vkGetPhysicalDeviceDisplayPropertiesKHR", result))
		return result;
	count = 1;
	VkDisplayModePropertiesKHR mode;
	result = vkGetDisplayModePropertiesKHR(harness->physical_device, display.display, &count, &mode);
	if (!harness_ok("vkGetDisplayModePropertiesKHR", result))
		return result;
	xsdk_log_write("display: %s, %ux%u at %u mHz", display.displayName, (unsigned)mode.parameters.visibleRegion.width,
		(unsigned)mode.parameters.visibleRegion.height, (unsigned)mode.parameters.refreshRate);
	const VkDisplaySurfaceCreateInfoKHR surface_info = {
		.sType = VK_STRUCTURE_TYPE_DISPLAY_SURFACE_CREATE_INFO_KHR,
		.displayMode = mode.displayMode,
		.planeIndex = 0,
		.transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
		.alphaMode = VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR,
		.imageExtent = mode.parameters.visibleRegion,
	};
	return vkCreateDisplayPlaneSurfaceKHR(harness->instance, &surface_info, NULL, &harness->surface);
}

VkResult harness_create_swapchain(Harness *harness, uint32_t image_count)
{
	VkResult result = create_surface(harness);
	if (!harness_ok("vkCreateDisplayPlaneSurfaceKHR", result))
		return result;
	const VkSwapchainCreateInfoKHR swapchain_info = {
		.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
		.surface = harness->surface,
		.minImageCount = image_count,
		.imageFormat = VK_FORMAT_B8G8R8A8_UNORM,
		.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
		.imageExtent = { HARNESS_WIDTH, HARNESS_HEIGHT },
		.imageArrayLayers = 1,
		.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
		.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
		.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
		.presentMode = VK_PRESENT_MODE_FIFO_KHR,
		.clipped = VK_TRUE,
	};
	result = vkCreateSwapchainKHR(harness->device, &swapchain_info, NULL, &harness->swapchain);
	if (!harness_ok("vkCreateSwapchainKHR", result))
		return result;
	harness->image_count = HARNESS_MAX_IMAGES;
	result = vkGetSwapchainImagesKHR(harness->device, harness->swapchain, &harness->image_count, harness->images);
	if (!harness_ok("vkGetSwapchainImagesKHR", result))
		return result;
	for (uint32_t i = 0; i < harness->image_count; i++) {
		const VkImageViewCreateInfo view_info = {
			.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
			.image = harness->images[i],
			.viewType = VK_IMAGE_VIEW_TYPE_2D,
			.format = VK_FORMAT_B8G8R8A8_UNORM,
			.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
		};
		result = vkCreateImageView(harness->device, &view_info, NULL, &harness->views[i]);
		if (!harness_ok("vkCreateImageView", result))
			return result;
	}
	return VK_SUCCESS;
}

void harness_destroy(Harness *harness)
{
	if (harness->device) {
		(void)vkDeviceWaitIdle(harness->device);
		for (uint32_t i = 0; i < harness->image_count; i++)
			vkDestroyImageView(harness->device, harness->views[i], NULL);
		vkDestroySwapchainKHR(harness->device, harness->swapchain, NULL);
		vkDestroyFence(harness->device, harness->fence, NULL);
		vkDestroyCommandPool(harness->device, harness->pool, NULL);
		vkDestroyDevice(harness->device, NULL);
	}
	if (harness->instance) {
		vkDestroySurfaceKHR(harness->instance, harness->surface, NULL);
		vkDestroyInstance(harness->instance, NULL);
	}
	*harness = (Harness){ .instance = VK_NULL_HANDLE };
}

VkResult harness_buffer(const Harness *harness, VkBufferUsageFlags usage, const void *data, uint32_t size_bytes,
	VkBuffer *buffer, VkDeviceMemory *memory)
{
	const VkBufferCreateInfo buffer_info = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = size_bytes,
		.usage = usage,
	};
	VkResult result = vkCreateBuffer(harness->device, &buffer_info, NULL, buffer);
	if (!harness_ok("vkCreateBuffer", result))
		return result;
	VkMemoryRequirements requirements;
	vkGetBufferMemoryRequirements(harness->device, *buffer, &requirements);
	const VkMemoryAllocateInfo allocate_info = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = requirements.size,
		.memoryTypeIndex = 0,
	};
	result = vkAllocateMemory(harness->device, &allocate_info, NULL, memory);
	if (!harness_ok("vkAllocateMemory", result))
		return result;
	result = vkBindBufferMemory(harness->device, *buffer, *memory, 0);
	if (!harness_ok("vkBindBufferMemory", result))
		return result;
	void *mapped = NULL;
	result = vkMapMemory(harness->device, *memory, 0, VK_WHOLE_SIZE, 0, &mapped);
	if (!harness_ok("vkMapMemory", result))
		return result;
	memcpy(mapped, data, size_bytes);
	vkUnmapMemory(harness->device, *memory);
	return VK_SUCCESS;
}

VkResult harness_shader(const Harness *harness, const uint32_t *spirv, uint32_t size_bytes, VkShaderModule *module)
{
	const VkShaderModuleCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = size_bytes,
		.pCode = spirv,
	};
	return vkCreateShaderModule(harness->device, &info, NULL, module);
}

uint32_t harness_image_address(uint32_t index)
{
	return xsdk_present_buffer_address(index);
}

// A front buffer word holds B, G, R, A from the top byte down (xsdk:docs/HARDWARE.md).
Rgb harness_pixel(uint32_t address, uint32_t x, uint32_t y)
{
	const uint32_t word = xsdk_present_pixels(address)[tiled_index(x, y, HARNESS_WIDTH)];
	return (Rgb){ .red = word >> 8 & 0xFF, .green = word >> 16 & 0xFF, .blue = word >> 24 };
}
