#ifndef V4X_BLIT_H
#define V4X_BLIT_H

#include <stdint.h>

#include <vulkan/vulkan.h>

#include "objects.h"
#include "xsdk/texture.h"

// Drawing an image back into eDRAM, for a render pass that loads it (docs/ROADMAP.md, milestone 4).

// Loads the layer's shaders and the rectangle they draw; needs the GPU open.
VkResult v4x_blit_create(Blit *blit);
void v4x_blit_destroy(Blit *blit);
// Draws the texture over the whole target, texel for texel; leaves shaders, state and fetch constant 0 changed.
void v4x_blit_draw(const Blit *blit, const XsdkTexture *texture, uint32_t width, uint32_t height);

#endif
