#include <vulkan/vulkan.h>

#include "host.h"
#include "objects.h"
#include "xsdk/kernel.h"

// One display with one plane and one mode: xsdk_present's 1280x720, which the console scales to its output.

static const char display_object;
static const char mode_object;

static VkDisplayKHR display_handle(void)
{
	return v4x_handle(&display_object);
}

static VkDisplayModeKHR mode_handle(void)
{
	return v4x_handle(&mode_object);
}

static const VkExtent2D display_extent = { V4X_DISPLAY_WIDTH, V4X_DISPLAY_HEIGHT };

VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceDisplayPropertiesKHR(
	VkPhysicalDevice physicalDevice, uint32_t *pPropertyCount, VkDisplayPropertiesKHR *pProperties)
{
	(void)physicalDevice;
	const VkDisplayPropertiesKHR display = {
		.display = display_handle(),
		.displayName = "Xbox 360 video output",
		.physicalResolution = display_extent,
		.supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
	};
	return v4x_host_enumerate(pPropertyCount, pProperties, &display, 1, sizeof display);
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceDisplayPlanePropertiesKHR(
	VkPhysicalDevice physicalDevice, uint32_t *pPropertyCount, VkDisplayPlanePropertiesKHR *pProperties)
{
	(void)physicalDevice;
	const VkDisplayPlanePropertiesKHR plane = { .currentDisplay = display_handle() };
	return v4x_host_enumerate(pPropertyCount, pProperties, &plane, 1, sizeof plane);
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetDisplayPlaneSupportedDisplaysKHR(
	VkPhysicalDevice physicalDevice, uint32_t planeIndex, uint32_t *pDisplayCount, VkDisplayKHR *pDisplays)
{
	(void)physicalDevice;
	const VkDisplayKHR display = display_handle();
	return v4x_host_enumerate(pDisplayCount, pDisplays, &display, planeIndex == 0, sizeof display);
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetDisplayModePropertiesKHR(VkPhysicalDevice physicalDevice, VkDisplayKHR display,
	uint32_t *pPropertyCount, VkDisplayModePropertiesKHR *pProperties)
{
	(void)physicalDevice;
	(void)display;
	VIDEO_MODE video;
	VdQueryVideoMode(&video);
	const VkDisplayModePropertiesKHR mode = {
		.displayMode = mode_handle(),
		.parameters = { .visibleRegion = display_extent, .refreshRate = (uint32_t)(video.refresh_rate * 1000.0f) },
	};
	return v4x_host_enumerate(pPropertyCount, pProperties, &mode, 1, sizeof mode);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateDisplayModeKHR(VkPhysicalDevice physicalDevice, VkDisplayKHR display,
	const VkDisplayModeCreateInfoKHR *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkDisplayModeKHR *pMode)
{
	(void)physicalDevice;
	(void)display;
	(void)pCreateInfo;
	(void)pAllocator;
	(void)pMode;
	return VK_ERROR_INITIALIZATION_FAILED;
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetDisplayPlaneCapabilitiesKHR(VkPhysicalDevice physicalDevice, VkDisplayModeKHR mode,
	uint32_t planeIndex, VkDisplayPlaneCapabilitiesKHR *pCapabilities)
{
	(void)physicalDevice;
	(void)mode;
	(void)planeIndex;
	*pCapabilities = (VkDisplayPlaneCapabilitiesKHR){
		.supportedAlpha = VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR,
		.minSrcExtent = display_extent,
		.maxSrcExtent = display_extent,
		.maxDstPosition = { 0, 0 },
		.minDstExtent = display_extent,
		.maxDstExtent = display_extent,
	};
	return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateDisplayPlaneSurfaceKHR(VkInstance instance,
	const VkDisplaySurfaceCreateInfoKHR *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkSurfaceKHR *pSurface)
{
	if (pCreateInfo->displayMode != mode_handle() || pCreateInfo->planeIndex != 0)
		return VK_ERROR_INITIALIZATION_FAILED;
	if (pCreateInfo->transform != VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR ||
		pCreateInfo->alphaMode != VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR ||
		pCreateInfo->imageExtent.width != V4X_DISPLAY_WIDTH || pCreateInfo->imageExtent.height != V4X_DISPLAY_HEIGHT)
		return v4x_host_unsupported("display surfaces other than full-screen, opaque and untransformed");
	const VkAllocationCallbacks *const allocator =
		pAllocator ? pAllocator : (instance->has_allocator ? &instance->allocator : NULL);
	Surface *const surface = v4x_host_alloc(allocator, sizeof *surface, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
	if (!surface)
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	*surface = (Surface){ .width = V4X_DISPLAY_WIDTH, .height = V4X_DISPLAY_HEIGHT };
	*pSurface = v4x_handle(surface);
	return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroySurfaceKHR(
	VkInstance instance, VkSurfaceKHR surface, const VkAllocationCallbacks *pAllocator)
{
	const VkAllocationCallbacks *const allocator =
		pAllocator ? pAllocator : (instance->has_allocator ? &instance->allocator : NULL);
	v4x_host_free(allocator, v4x_object(surface));
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceSupportKHR(
	VkPhysicalDevice physicalDevice, uint32_t queueFamilyIndex, VkSurfaceKHR surface, VkBool32 *pSupported)
{
	(void)physicalDevice;
	(void)surface;
	*pSupported = queueFamilyIndex == 0;
	return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
	VkPhysicalDevice physicalDevice, VkSurfaceKHR surface, VkSurfaceCapabilitiesKHR *pSurfaceCapabilities)
{
	(void)physicalDevice;
	(void)surface;
	*pSurfaceCapabilities = (VkSurfaceCapabilitiesKHR){
		.minImageCount = V4X_MIN_SWAPCHAIN_IMAGES,
		.maxImageCount = V4X_MAX_SWAPCHAIN_IMAGES,
		.currentExtent = display_extent,
		.minImageExtent = display_extent,
		.maxImageExtent = display_extent,
		.maxImageArrayLayers = 1,
		.supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
		.currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
		.supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
		.supportedUsageFlags = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
	};
	return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceFormatsKHR(VkPhysicalDevice physicalDevice,
	VkSurfaceKHR surface, uint32_t *pSurfaceFormatCount, VkSurfaceFormatKHR *pSurfaceFormats)
{
	(void)physicalDevice;
	(void)surface;
	// Resolves store B, G, R, A in byte order, as the display reads them (xsdk:docs/HARDWARE.md).
	const VkSurfaceFormatKHR format = { VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
	return v4x_host_enumerate(pSurfaceFormatCount, pSurfaceFormats, &format, 1, sizeof format);
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfacePresentModesKHR(
	VkPhysicalDevice physicalDevice, VkSurfaceKHR surface, uint32_t *pPresentModeCount, VkPresentModeKHR *pPresentModes)
{
	(void)physicalDevice;
	(void)surface;
	const VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
	return v4x_host_enumerate(pPresentModeCount, pPresentModes, &mode, 1, sizeof mode);
}
