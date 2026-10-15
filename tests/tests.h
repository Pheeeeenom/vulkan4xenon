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
// The triangle again, its shaders read back from the layer's cache by a second device (milestone 13).
void test_triangle_from_cache(const Harness *harness);
// The triangle with 8-bit colours packed in a word, as Aurora decodes vertex colours (milestone 10).
void test_packed_colour(const Harness *harness);
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
// Uniform arrays read at run-time indices: matrices picked per vertex, colours by pixel column (milestone 11).
void test_run_time_index(const Harness *harness);
// Copies from cached memory, R8 and R8G8 through swizzled views, R8 filtered, and copies around a draw (milestone 12).
void test_uploads(const Harness *harness);
// Images one texel high, as palettes are, every texel drawn where it should be (milestone 14).
void test_thin_textures(const Harness *harness);
// Names the layer's cache, a folder emptied first; called before the device is made (milestone 13).
void test_shader_cache_start(void);
// The files the layer kept, and a second device reading the triangle's shaders back; called after the first goes.
void test_shader_cache_finish(void);

#endif
