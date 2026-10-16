#include <string.h>

#include <vulkan/vulkan.h>

#include "host.h"
#include "image.h"
#include "objects.h"
#include "xsdk/texture.h"

enum {
	// A budget for the layer, half the console's 512 MB that the CPU and GPU share; not a measurement.
	HEAP_BYTES = 256u << 20,
	// xsdk_gpu_alloc hands out 64 KB pages, so a mapping starts on one.
	MAP_ALIGNMENT = 64 << 10,
	// The CPU's cache line.
	CACHE_LINE_BYTES = 128,
	VIEWPORT_MAX = 4096,
	// Every register of a stage's constants (xsdk:include/xsdk/gpu.h).
	MAX_UNIFORM_BUFFER_BYTES = V4X_FLOAT_CONSTANTS * 16,
	// Vulkan 1.0's minimums, which descriptor sets in host memory meet.
	MIN_UNIFORM_BUFFERS_PER_STAGE = 12,
	MIN_UNIFORM_BUFFERS_PER_SET = 72,
	MIN_DYNAMIC_UNIFORM_BUFFERS_PER_SET = 8,
	MIN_SAMPLERS_PER_SET = 96,
	MIN_SAMPLER_ALLOCATIONS = 4000,
	// A level for each halving down to 1x1 (xsdk:src/texture.c, level_count).
	MAX_IMAGE_LEVELS = 14,
	MAX_LOD_BIAS = 15,
	// xsdk sets VGT_MAX_VTX_INDX to 24 bits (xsdk:src/gpu_draw.c).
	MAX_INDEX_VALUE = 0x00FFFFFF,
	DEPTH_IMAGE_BYTES = 4096,
};

static const VkExtensionProperties device_extensions[] = {
	{ VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_KHR_SWAPCHAIN_SPEC_VERSION },
};

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties(
	VkPhysicalDevice physicalDevice, VkPhysicalDeviceProperties *pProperties)
{
	(void)physicalDevice;
	*pProperties = (VkPhysicalDeviceProperties){
		.apiVersion = VK_MAKE_API_VERSION(0, 1, 0, VK_HEADER_VERSION),
		.driverVersion = 1,
		.deviceType = VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU,
		.deviceName = "Xenos (vulkan4xenon)",
		.limits = {
			.maxImageDimension2D = XSDK_TEXTURE_MAX_SIZE,
			.maxImageArrayLayers = 1,
			.maxUniformBufferRange = MAX_UNIFORM_BUFFER_BYTES,
			.maxPushConstantsSize = V4X_MAX_PUSH_CONSTANT_BYTES,
			.maxMemoryAllocationCount = HEAP_BYTES / MAP_ALIGNMENT,
			.maxSamplerAllocationCount = MIN_SAMPLER_ALLOCATIONS,
			.bufferImageGranularity = 1,
			.maxBoundDescriptorSets = V4X_MAX_DESCRIPTOR_SETS,
			.maxPerStageDescriptorSamplers = V4X_MAX_SAMPLERS,
			.maxPerStageDescriptorUniformBuffers = MIN_UNIFORM_BUFFERS_PER_STAGE,
			.maxPerStageDescriptorSampledImages = V4X_MAX_SAMPLERS,
			.maxPerStageResources = MIN_UNIFORM_BUFFERS_PER_STAGE + V4X_MAX_SAMPLERS,
			.maxDescriptorSetSamplers = MIN_SAMPLERS_PER_SET,
			.maxDescriptorSetUniformBuffers = MIN_UNIFORM_BUFFERS_PER_SET,
			.maxDescriptorSetUniformBuffersDynamic = MIN_DYNAMIC_UNIFORM_BUFFERS_PER_SET,
			.maxDescriptorSetSampledImages = MIN_SAMPLERS_PER_SET,
			.maxVertexInputAttributes = V4X_MAX_VERTEX_ATTRIBUTES,
			.maxVertexInputBindings = V4X_MAX_VERTEX_BINDINGS,
			.maxVertexInputAttributeOffset = V4X_MAX_VERTEX_STRIDE_BYTES - 4,
			.maxVertexInputBindingStride = V4X_MAX_VERTEX_STRIDE_BYTES,
			// 16 interpolators of four floats (xost:docs/xsc.md, "Interface conventions").
			.maxVertexOutputComponents = 64,
			.maxFragmentInputComponents = 64,
			.maxFragmentOutputAttachments = 1,
			.maxViewports = 1,
			.maxViewportDimensions = { VIEWPORT_MAX, VIEWPORT_MAX },
			.viewportBoundsRange = { -2.0f * VIEWPORT_MAX, 2.0f * VIEWPORT_MAX - 1.0f },
			// Vertices land on 1/16 pixel under xsdk's PA_SU_VTX_CNTL (xenia:src/xenia/gpu/xenos.h:877-884).
			.subPixelPrecisionBits = 4,
			.maxDrawIndexedIndexValue = MAX_INDEX_VALUE,
			// No multiDrawIndirect: an indirect command draws once.
			.maxDrawIndirectCount = 1,
			.maxSamplerLodBias = MAX_LOD_BIAS,
			.maxSamplerAnisotropy = XSDK_MAX_ANISOTROPY,
			.minMemoryMapAlignment = MAP_ALIGNMENT,
			.minTexelBufferOffsetAlignment = 256,
			.minUniformBufferOffsetAlignment = 256,
			.minStorageBufferOffsetAlignment = 256,
			.maxFramebufferWidth = V4X_DISPLAY_WIDTH,
			.maxFramebufferHeight = V4X_DISPLAY_HEIGHT,
			.maxFramebufferLayers = 1,
			.framebufferColorSampleCounts = VK_SAMPLE_COUNT_1_BIT,
			.framebufferDepthSampleCounts = VK_SAMPLE_COUNT_1_BIT,
			.framebufferStencilSampleCounts = VK_SAMPLE_COUNT_1_BIT,
			.maxColorAttachments = 1,
			.sampledImageColorSampleCounts = VK_SAMPLE_COUNT_1_BIT,
			.discreteQueuePriorities = 2,
			.pointSizeRange = { 1.0f, 1.0f },
			.lineWidthRange = { 1.0f, 1.0f },
			.standardSampleLocations = VK_TRUE,
			.optimalBufferCopyOffsetAlignment = 1,
			.optimalBufferCopyRowPitchAlignment = 1,
			.nonCoherentAtomSize = CACHE_LINE_BYTES,
		},
	};
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFeatures(
	VkPhysicalDevice physicalDevice, VkPhysicalDeviceFeatures *pFeatures)
{
	(void)physicalDevice;
	*pFeatures = (VkPhysicalDeviceFeatures){ .samplerAnisotropy = VK_TRUE };
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceQueueFamilyProperties(VkPhysicalDevice physicalDevice,
	uint32_t *pQueueFamilyPropertyCount, VkQueueFamilyProperties *pQueueFamilyProperties)
{
	(void)physicalDevice;
	const VkQueueFamilyProperties family = {
		.queueFlags = VK_QUEUE_GRAPHICS_BIT,
		.queueCount = 1,
		.minImageTransferGranularity = { 1, 1, 1 },
	};
	(void)v4x_host_enumerate(pQueueFamilyPropertyCount, pQueueFamilyProperties, &family, 1, sizeof family);
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceMemoryProperties(
	VkPhysicalDevice physicalDevice, VkPhysicalDeviceMemoryProperties *pMemoryProperties)
{
	(void)physicalDevice;
	// The GPU's write-combined memory, then the CPU's cached memory, which only transfer-source buffers take; both from
	// the one heap of the title's memory.
	*pMemoryProperties = (VkPhysicalDeviceMemoryProperties){
		.memoryTypeCount = 2,
		.memoryTypes = { { VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
								 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
							 0 },
			{ VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
					VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
				0 } },
		.memoryHeapCount = 1,
		.memoryHeaps = { { HEAP_BYTES, VK_MEMORY_HEAP_DEVICE_LOCAL_BIT } },
	};
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFormatProperties(
	VkPhysicalDevice physicalDevice, VkFormat format, VkFormatProperties *pFormatProperties)
{
	(void)physicalDevice;
	*pFormatProperties = (VkFormatProperties){ 0 };
	// Blits run on the CPU, for formats of 8-bit channels (vkCmdBlitImage).
	const VkFormatFeatureFlags blit = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
	const VkFormatFeatureFlags color = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
		VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
		VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | blit;
	switch (format) {
	case VK_FORMAT_R8G8B8A8_UNORM:
	case VK_FORMAT_B8G8R8A8_UNORM:
		pFormatProperties->optimalTilingFeatures = color;
		break;
	case VK_FORMAT_D24_UNORM_S8_UINT:
		// Sampled, its depth in red: nearest only, as filtering depth hasn't been tried on the console.
		pFormatProperties->optimalTilingFeatures =
			VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
		break;
	case VK_FORMAT_R8_UNORM:
	case VK_FORMAT_R8G8_UNORM:
		pFormatProperties->optimalTilingFeatures =
			VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | blit;
		break;
	case VK_FORMAT_R32_SFLOAT:
		// Sampled, as xsdk's R32F: nearest only, since filtering 32-bit floats hasn't been tried on the console.
		pFormatProperties->optimalTilingFeatures = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
		pFormatProperties->bufferFeatures = VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT;
		break;
	case VK_FORMAT_R32G32_SFLOAT:
	case VK_FORMAT_R32G32B32_SFLOAT:
	case VK_FORMAT_R32G32B32A32_SFLOAT:
	case VK_FORMAT_A8B8G8R8_UNORM_PACK32:
		pFormatProperties->bufferFeatures = VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT;
		break;
	default:
		break;
	}
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceImageFormatProperties(VkPhysicalDevice physicalDevice,
	VkFormat format, VkImageType type, VkImageTiling tiling, VkImageUsageFlags usage, VkImageCreateFlags flags,
	VkImageFormatProperties *pImageFormatProperties)
{
	(void)physicalDevice;
	if (format == VK_FORMAT_D24_UNORM_S8_UINT && type == VK_IMAGE_TYPE_2D && tiling == VK_IMAGE_TILING_OPTIMAL &&
		!(usage & ~(VkImageUsageFlags)V4X_DEPTH_IMAGE_USAGE) && !flags) {
		*pImageFormatProperties = (VkImageFormatProperties){
			.maxExtent = { V4X_DISPLAY_WIDTH, V4X_DISPLAY_HEIGHT, 1 },
			.maxMipLevels = 1,
			.maxArrayLayers = 1,
			.sampleCounts = VK_SAMPLE_COUNT_1_BIT,
			.maxResourceSize = (usage & VK_IMAGE_USAGE_SAMPLED_BIT) ? HEAP_BYTES : DEPTH_IMAGE_BYTES,
		};
		return VK_SUCCESS;
	}
	uint32_t xsdk_format;
	const bool linear = tiling == VK_IMAGE_TILING_LINEAR;
	const VkImageUsageFlags usages = linear ? V4X_LINEAR_IMAGE_USAGE : V4X_IMAGE_USAGE;
	if (!v4x_image_format(format, &xsdk_format) || type != VK_IMAGE_TYPE_2D || (usage & ~usages) || flags ||
		((usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) && !v4x_attachment_format(format))) {
		*pImageFormatProperties = (VkImageFormatProperties){ 0 };
		return VK_ERROR_FORMAT_NOT_SUPPORTED;
	}
	*pImageFormatProperties = (VkImageFormatProperties){
		.maxExtent = { XSDK_TEXTURE_MAX_SIZE, XSDK_TEXTURE_MAX_SIZE, 1 },
		.maxMipLevels = linear || (usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) ? 1 : MAX_IMAGE_LEVELS,
		.maxArrayLayers = 1,
		.sampleCounts = VK_SAMPLE_COUNT_1_BIT,
		.maxResourceSize = HEAP_BYTES,
	};
	return VK_SUCCESS;
}

// The features report no sparse residency, so no format has sparse properties.
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceSparseImageFormatProperties(VkPhysicalDevice physicalDevice,
	VkFormat format, VkImageType type, VkSampleCountFlagBits samples, VkImageUsageFlags usage, VkImageTiling tiling,
	uint32_t *pPropertyCount, VkSparseImageFormatProperties *pProperties)
{
	(void)physicalDevice;
	(void)format;
	(void)type;
	(void)samples;
	(void)usage;
	(void)tiling;
	(void)pProperties;
	*pPropertyCount = 0;
}

VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateDeviceExtensionProperties(VkPhysicalDevice physicalDevice,
	const char *pLayerName, uint32_t *pPropertyCount, VkExtensionProperties *pProperties)
{
	(void)physicalDevice;
	if (pLayerName)
		return VK_ERROR_LAYER_NOT_PRESENT;
	return v4x_host_enumerate(pPropertyCount, pProperties, device_extensions,
		sizeof device_extensions / sizeof device_extensions[0], sizeof device_extensions[0]);
}

VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateDeviceLayerProperties(
	VkPhysicalDevice physicalDevice, uint32_t *pPropertyCount, VkLayerProperties *pProperties)
{
	(void)physicalDevice;
	(void)pProperties;
	*pPropertyCount = 0;
	return VK_SUCCESS;
}

bool v4x_physical_device_has_extension(const char *name)
{
	for (uint32_t i = 0; i < sizeof device_extensions / sizeof device_extensions[0]; i++) {
		if (!strcmp(name, device_extensions[i].extensionName))
			return true;
	}
	return false;
}
