#ifndef V4X_IMAGE_H
#define V4X_IMAGE_H

#include <stdbool.h>
#include <stdint.h>

#include <vulkan/vulkan.h>

// The xsdk format an image of this Vulkan format is, if images of it are supported.
bool v4x_image_format(VkFormat format, uint32_t *xsdk_format);

#endif
