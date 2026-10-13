#ifndef V4X_IMAGE_H
#define V4X_IMAGE_H

#include <stdbool.h>
#include <stdint.h>

#include <vulkan/vulkan.h>

enum {
	// What colour images may be used for.
	V4X_IMAGE_USAGE =
		VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
	// A base level this many texels or fewer on a side sits in the packed mip tail, where a resolve can't write
	// (xsdk:src/gpu_resolve.c, xsdk_gpu_resolve_texture).
	V4X_PACKED_SIDE = 16,
};

// The xsdk format an image of this Vulkan format is, if images of it are supported.
bool v4x_image_format(VkFormat format, uint32_t *xsdk_format);
// Whether images of this format can be colour attachments: R8G8B8A8_UNORM and B8G8R8A8_UNORM, which resolves write.
bool v4x_attachment_format(VkFormat format);

#endif
