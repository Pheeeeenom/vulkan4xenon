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
	default:
		return false;
	}
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
		return v4x_host_unsupported("image formats other than R8G8B8A8_UNORM and B8G8R8A8_UNORM, single-sampled");
	if (info->tiling != VK_IMAGE_TILING_OPTIMAL)
		return v4x_host_unsupported("linear images");
	if (info->usage & ~(VkImageUsageFlags)(VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT))
		return v4x_host_unsupported("image usages other than sampling and copying in");
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
	xsdk_texture_attach(&object->texture, bound->pointer + memoryOffset / 4);
	object->address = object->texture.address;
	return VK_SUCCESS;
}

static bool identity(VkComponentSwizzle swizzle, VkComponentSwizzle own)
{
	return swizzle == VK_COMPONENT_SWIZZLE_IDENTITY || swizzle == own;
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateImageView(VkDevice device, const VkImageViewCreateInfo *pCreateInfo,
	const VkAllocationCallbacks *pAllocator, VkImageView *pView)
{
	const Image *const image = v4x_object(pCreateInfo->image);
	const VkImageSubresourceRange *const range = &pCreateInfo->subresourceRange;
	const uint32_t levels = image->is_texture ? image->texture.levels : 1;
	const uint32_t level_count = range->levelCount == VK_REMAINING_MIP_LEVELS ? levels : range->levelCount;
	if (pCreateInfo->viewType != VK_IMAGE_VIEW_TYPE_2D || pCreateInfo->format != image->format || range->baseMipLevel ||
		level_count != levels || range->baseArrayLayer || range->layerCount != 1)
		return v4x_host_unsupported("image views other than the whole of a 2D image in its format");
	const VkComponentMapping *const mapping = &pCreateInfo->components;
	if (!identity(mapping->r, VK_COMPONENT_SWIZZLE_R) || !identity(mapping->g, VK_COMPONENT_SWIZZLE_G) ||
		!identity(mapping->b, VK_COMPONENT_SWIZZLE_B) || !identity(mapping->a, VK_COMPONENT_SWIZZLE_A))
		return v4x_host_unsupported("image view swizzles");
	ImageView *const view =
		v4x_host_alloc(v4x_host_device_allocator(device, pAllocator), sizeof *view, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
	if (!view)
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	view->image = image;
	*pView = v4x_handle(view);
	return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyImageView(
	VkDevice device, VkImageView imageView, const VkAllocationCallbacks *pAllocator)
{
	v4x_host_free(v4x_host_device_allocator(device, pAllocator), v4x_object(imageView));
}
