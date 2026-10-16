#ifndef V4X_TRANSFER_H
#define V4X_TRANSFER_H

#include <vulkan/vulkan.h>

#include "objects.h"

// Copies, clears and blits, which the CPU runs as it replays them (docs/ROADMAP.md, section 3), each waiting first for
// the GPU to be done with what it reads or writes.

void v4x_transfer_buffer_to_image(VkQueue queue, const Command *command);
void v4x_transfer_image_to_buffer(VkQueue queue, const Command *command);
void v4x_transfer_copy_image(VkQueue queue, const Command *command);
void v4x_transfer_blit(VkQueue queue, const Command *command);
void v4x_transfer_clear(VkQueue queue, const Command *command);
// A copy into a buffer from source: another buffer's bytes, or vkCmdUpdateBuffer's in the command buffer's data.
void v4x_transfer_copy_buffer(VkQueue queue, const Command *command, volatile const void *source);
void v4x_transfer_fill_buffer(VkQueue queue, const Command *command);
// A sampled depth image's texture cleared, each texel's word depth's 24 bits above stencil's 8 (docs/SUPPORT.md,
// Images).
void v4x_transfer_fill_depth(VkQueue queue, Image *image, float depth, uint8_t stencil);

#endif
