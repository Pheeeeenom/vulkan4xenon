#include <vulkan/vulkan.h>

#include "host.h"
#include "image.h"
#include "objects.h"
#include "xsdk/log.h"
#include "xsdk/texture.h"

enum {
	// A texture fetch constant holds its address in 4 KB units (xsdk:src/texture.c, FETCH_ADDRESS_ALIGNMENT).
	IMAGE_ALIGNMENT = 4096,
	MEMORY_TYPE_BITS = 1,
	// A depth image's memory is never used, since depth stays in eDRAM; a page keeps allocators happy.
	DEPTH_IMAGE_BYTES = 4096,
};

bool v4x_image_format(VkFormat format, uint32_t *xsdk_format)
{
	switch (format) {
	case VK_FORMAT_R8G8B8A8_UNORM:
		*xsdk_format = XSDK_FORMAT_R8G8B8A8;
		return true;
	case VK_FORMAT_B8G8R8A8_UNORM:
		*xsdk_format = XSDK_FORMAT_B8G8R8A8;
		return true;
	case VK_FORMAT_R32_SFLOAT:
		*xsdk_format = XSDK_FORMAT_R32F;
		return true;
	case VK_FORMAT_R8_UNORM:
		*xsdk_format = XSDK_FORMAT_R8;
		return true;
	case VK_FORMAT_R8G8_UNORM:
		*xsdk_format = XSDK_FORMAT_R8G8;
		return true;
	default:
		return false;
	}
}

uint32_t v4x_texel_bytes(VkFormat format)
{
	switch (format) {
	case VK_FORMAT_R8_UNORM:
		return 1;
	case VK_FORMAT_R8G8_UNORM:
		return 2;
	default:
		return 4;
	}
}

bool v4x_attachment_format(VkFormat format)
{
	return format == VK_FORMAT_R8G8B8A8_UNORM || format == VK_FORMAT_B8G8R8A8_UNORM;
}

static VkResult check_depth_create_info(const VkImageCreateInfo *info)
{
	const VkImageUsageFlags usage =
		VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;
	if (info->mipLevels != 1 || info->samples != VK_SAMPLE_COUNT_1_BIT || info->tiling != VK_IMAGE_TILING_OPTIMAL ||
		(info->usage & ~usage))
		return v4x_host_unsupported("depth images other than a single-sampled attachment of one level");
	return VK_SUCCESS;
}

static VkResult check_create_info(const VkImageCreateInfo *info)
{
	uint32_t format = 0;
	if (info->imageType != VK_IMAGE_TYPE_2D || info->extent.depth != 1 || info->arrayLayers != 1 || info->flags)
		return v4x_host_unsupported("images other than 2D with one layer");
	if (info->format == VK_FORMAT_D24_UNORM_S8_UINT)
		return check_depth_create_info(info);
	if (!v4x_image_format(info->format, &format) || info->samples != VK_SAMPLE_COUNT_1_BIT)
		return v4x_host_unsupported(
			"image formats other than R8G8B8A8_UNORM, B8G8R8A8_UNORM, R32_SFLOAT, R8_UNORM and "
			"R8G8_UNORM, single-sampled");
	if (info->tiling != VK_IMAGE_TILING_OPTIMAL)
		return v4x_host_unsupported("linear images");
	if (info->usage & ~(VkImageUsageFlags)V4X_IMAGE_USAGE)
		return v4x_host_unsupported("image usages other than sampling, copying in and colour attachments");
	if ((info->usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) && !v4x_attachment_format(info->format))
		return v4x_host_unsupported("colour attachments other than R8G8B8A8_UNORM and B8G8R8A8_UNORM");
	if ((info->usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) &&
		(info->mipLevels != 1 || info->extent.width <= V4X_PACKED_SIDE || info->extent.height <= V4X_PACKED_SIDE))
		return v4x_host_unsupported("colour attachments with mips, or 16 texels or less on a side");
	return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateImage(
	VkDevice device, const VkImageCreateInfo *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkImage *pImage)
{
	VkResult result = check_create_info(pCreateInfo);
	if (result != VK_SUCCESS)
		return result;
	const VkAllocationCallbacks *const allocator = v4x_host_device_allocator(device, pAllocator);
	Image *const image = v4x_host_alloc(allocator, sizeof *image, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
	if (!image)
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	image->width = pCreateInfo->extent.width;
	image->height = pCreateInfo->extent.height;
	image->format = pCreateInfo->format;
	if (pCreateInfo->format == VK_FORMAT_D24_UNORM_S8_UINT) {
		image->is_depth = true;
		*pImage = v4x_handle(image);
		return VK_SUCCESS;
	}
	uint32_t format = 0;
	(void)v4x_image_format(pCreateInfo->format, &format);
	const NTSTATUS status = xsdk_texture_init(
		&image->texture, format, pCreateInfo->extent.width, pCreateInfo->extent.height, pCreateInfo->mipLevels, true);
	if (!NT_SUCCESS(status)) {
		xsdk_log_write("vulkan4xenon: xsdk_texture_init failed: 0x%08X", (unsigned)status);
		v4x_host_free(allocator, image);
		return VK_ERROR_OUT_OF_DEVICE_MEMORY;
	}
	image->is_texture = true;
	image->is_attachment = (pCreateInfo->usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) != 0;
	*pImage = v4x_handle(image);
	return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyImage(VkDevice device, VkImage image, const VkAllocationCallbacks *pAllocator)
{
	v4x_host_free(v4x_host_device_allocator(device, pAllocator), v4x_object(image));
}

VKAPI_ATTR void VKAPI_CALL vkGetImageMemoryRequirements(
	VkDevice device, VkImage image, VkMemoryRequirements *pMemoryRequirements)
{
	(void)device;
	const Image *const object = v4x_object(image);
	*pMemoryRequirements = (VkMemoryRequirements){
		.size = object->is_depth ? DEPTH_IMAGE_BYTES : object->texture.size_bytes,
		.alignment = IMAGE_ALIGNMENT,
		.memoryTypeBits = MEMORY_TYPE_BITS,
	};
}

VKAPI_ATTR VkResult VKAPI_CALL vkBindImageMemory(
	VkDevice device, VkImage image, VkDeviceMemory memory, VkDeviceSize memoryOffset)
{
	(void)device;
	Image *const object = v4x_object(image);
	const DeviceMemory *const bound = v4x_object(memory);
	if (object->is_depth)
		return VK_SUCCESS;
	// Invalid use: an image's memory types are the GPU's alone (vkGetImageMemoryRequirements).
	if (bound->cached) {
		xsdk_log_write("vulkan4xenon: an image bound to cached memory, which the GPU doesn't read");
		return VK_ERROR_OUT_OF_DEVICE_MEMORY;
	}
	xsdk_texture_attach(&object->texture, bound->pointer + memoryOffset / 4);
	object->address = object->texture.address;
	return VK_SUCCESS;
}

// The XSDK_CHANNEL_* a component mapping takes for the component at index, red to alpha.
static uint32_t channel(VkComponentSwizzle swizzle, uint32_t index)
{
	switch (swizzle) {
	case VK_COMPONENT_SWIZZLE_ZERO:
		return XSDK_CHANNEL_ZERO;
	case VK_COMPONENT_SWIZZLE_ONE:
		return XSDK_CHANNEL_ONE;
	case VK_COMPONENT_SWIZZLE_R:
	case VK_COMPONENT_SWIZZLE_G:
	case VK_COMPONENT_SWIZZLE_B:
	case VK_COMPONENT_SWIZZLE_A:
		return XSDK_CHANNEL_RED + (uint32_t)(swizzle - VK_COMPONENT_SWIZZLE_R);
	default:
		return XSDK_CHANNEL_RED + index;
	}
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateImageView(VkDevice device, const VkImageViewCreateInfo *pCreateInfo,
	const VkAllocationCallbacks *pAllocator, VkImageView *pView)
{
	Image *const image = v4x_object(pCreateInfo->image);
	const VkImageSubresourceRange *const range = &pCreateInfo->subresourceRange;
	const uint32_t levels = image->is_texture ? image->texture.levels : 1;
	const uint32_t level_count = range->levelCount == VK_REMAINING_MIP_LEVELS ? levels : range->levelCount;
	if (pCreateInfo->viewType != VK_IMAGE_VIEW_TYPE_2D || pCreateInfo->format != image->format || range->baseMipLevel ||
		level_count != levels || range->baseArrayLayer || range->layerCount != 1)
		return v4x_host_unsupported("image views other than the whole of a 2D image in its format");
	const VkComponentMapping *const mapping = &pCreateInfo->components;
	const uint32_t channels[4] = { channel(mapping->r, 0), channel(mapping->g, 1), channel(mapping->b, 2),
		channel(mapping->a, 3) };
	const bool swizzled = channels[0] != XSDK_CHANNEL_RED || channels[1] != XSDK_CHANNEL_GREEN ||
		channels[2] != XSDK_CHANNEL_BLUE || channels[3] != XSDK_CHANNEL_ALPHA;
	if (swizzled && !image->is_texture)
		return v4x_host_unsupported("swizzled views of depth images");
	ImageView *const view =
		v4x_host_alloc(v4x_host_device_allocator(device, pAllocator), sizeof *view, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
	if (!view)
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	// The image's memory is bound before its views are made, so its texture is complete.
	view->image = image;
	view->texture = image->texture;
	if (swizzled)
		xsdk_texture_swizzle(&view->texture, channels);
	*pView = v4x_handle(view);
	return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyImageView(
	VkDevice device, VkImageView imageView, const VkAllocationCallbacks *pAllocator)
{
	v4x_host_free(v4x_host_device_allocator(device, pAllocator), v4x_object(imageView));
}
