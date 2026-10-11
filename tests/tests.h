#ifndef V4X_TESTS_H
#define V4X_TESTS_H

#include <stdint.h>

#include "harness.h"

// The logo shows from vkCreateDevice until the first present; called with the device made and no swapchain yet.
void test_boot_logo(Harness *harness, uint32_t shown_before_device);
// The proc-address table, memory types, buffer requirements and refusals.
void test_objects(const Harness *harness);
// A triangle through Vulkan, checked on the image the display shows.
void test_triangle(const Harness *harness);
// Images copied in from a buffer and sampled through descriptor sets (milestone 2).
void test_texture(const Harness *harness);
// Quads placed and coloured by push constants (milestone 2).
void test_push_constants(const Harness *harness);
// Quads placed by a matrix and coloured from uniform buffers, one dynamic, in two sets (milestone 2).
void test_uniform_buffers(const Harness *harness);
// Index buffers and every topology (milestone 3).
void test_indexed(const Harness *harness);
// A depth and stencil attachment: depth tests, a stencil mask and a depth bias (milestone 3).
void test_depth_stencil(const Harness *harness);
// Culling, blending, a write mask, dynamic viewport and scissor, and pixel centres (milestone 3).
void test_raster(const Harness *harness);
// Images rendered, copied, loaded and sampled in the passes after, depth kept in eDRAM (milestone 4).
void test_render_to_texture(const Harness *harness);

#endif
