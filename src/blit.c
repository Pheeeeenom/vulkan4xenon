#include "blit.h"

#include <string.h>

#include <vulkan/vulkan.h>

#include "blit_shaders.h"
#include "objects.h"
#include "shader.h"
#include "xsdk/gpu.h"
#include "xsdk/log.h"
#include "xsdk/shader.h"
#include "xsdk/texture.h"

enum {
	RECTANGLE_VERTICES = 6,
	// A position, then texture coordinates.
	VERTEX_FLOATS = 6,
	VERTEX_BYTES = VERTEX_FLOATS * 4,
	RECTANGLE_BYTES = RECTANGLE_VERTICES * VERTEX_BYTES,
	UV_OFFSET_BYTES = 16,
};

// Two triangles over clip space, Vulkan's y down, so texture coordinates (0, 0) land on the top left pixel.
static const float rectangle[RECTANGLE_VERTICES][VERTEX_FLOATS] = {
	{ -1.0f, -1.0f, 0.0f, 1.0f, 0.0f, 0.0f },
	{ 1.0f, -1.0f, 0.0f, 1.0f, 1.0f, 0.0f },
	{ -1.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f },
	{ 1.0f, -1.0f, 0.0f, 1.0f, 1.0f, 0.0f },
	{ 1.0f, 1.0f, 0.0f, 1.0f, 1.0f, 1.0f },
	{ -1.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f },
};

static VkResult load_shaders(Blit *blit, const V4xShader *vertex, const V4xShader *pixel, const V4xShader *clear)
{
	NTSTATUS status = xsdk_shader_load(&blit->vertex, vertex->container, vertex->container_size_bytes);
	if (NT_SUCCESS(status))
		status = xsdk_shader_load(&blit->pixel, pixel->container, pixel->container_size_bytes);
	if (NT_SUCCESS(status))
		status = xsdk_shader_load(&blit->clear, clear->container, clear->container_size_bytes);
	if (!NT_SUCCESS(status)) {
		xsdk_log_write("vulkan4xenon: xsdk_shader_load failed for the blit: 0x%08X", (unsigned)status);
		return VK_ERROR_OUT_OF_DEVICE_MEMORY;
	}
	// Locations 0 and 1 are the position and the colour usage to xost (xsdk:src/xsc_resolve.c:344-346).
	const XsdkVertexElement elements[] = {
		{ .usage = XSDK_USAGE_POSITION, .format = XSDK_VERTEX_FLOAT4, .stream = 0, .offset_bytes = 0 },
		{ .usage = XSDK_USAGE_COLOR, .format = XSDK_VERTEX_FLOAT2, .stream = 0, .offset_bytes = UV_OFFSET_BYTES },
	};
	const uint32_t strides[XSDK_VERTEX_STREAMS] = { VERTEX_BYTES };
	status = xsdk_shader_set_vertex_layout(&blit->vertex, elements, sizeof elements / sizeof elements[0], strides);
	if (!NT_SUCCESS(status)) {
		xsdk_log_write("vulkan4xenon: xsdk_shader_set_vertex_layout failed for the blit: 0x%08X", (unsigned)status);
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	blit->texture_slot = pixel->resources[0].first;
	blit->clear_constant = clear->resources[0].first;
	return VK_SUCCESS;
}

static VkResult write_rectangle(Blit *blit)
{
	blit->vertices = xsdk_gpu_alloc(RECTANGLE_BYTES);
	if (!blit->vertices) {
		xsdk_log_write("vulkan4xenon: no GPU memory for the blit's rectangle");
		return VK_ERROR_OUT_OF_DEVICE_MEMORY;
	}
	const float *const floats = &rectangle[0][0];
	for (uint32_t i = 0; i < RECTANGLE_BYTES / 4; i++) {
		uint32_t bits;
		memcpy(&bits, &floats[i], sizeof bits);
		blit->vertices[i] = bits;
	}
	blit->vertices_address = xsdk_gpu_address(blit->vertices);
	return VK_SUCCESS;
}

// Point sampling, edges clamped; no blending, culling, depth or stencil.
static void make_state(Blit *blit)
{
	const XsdkSampler sampler = { .mag_filter = XSDK_FILTER_POINT,
		.min_filter = XSDK_FILTER_POINT,
		.mip_filter = XSDK_FILTER_POINT,
		.address_u = XSDK_ADDRESS_CLAMP,
		.address_v = XSDK_ADDRESS_CLAMP };
	xsdk_texture_sampler_state(&blit->sampler, &sampler);
	const XsdkBlend blend = { .color_source = XSDK_BLEND_ONE,
		.color_dest = XSDK_BLEND_ZERO,
		.color_op = XSDK_BLEND_OP_ADD,
		.alpha_source = XSDK_BLEND_ONE,
		.alpha_dest = XSDK_BLEND_ZERO,
		.alpha_op = XSDK_BLEND_OP_ADD,
		.write_mask = XSDK_WRITE_ALL };
	xsdk_gpu_blend_state(&blit->blend, &blend);
	const XsdkRaster raster = { .cull = XSDK_CULL_NONE };
	xsdk_gpu_raster_state(&blit->raster, &raster);
	const XsdkDepthStencil depth_stencil = { .depth_compare = XSDK_COMPARE_ALWAYS };
	xsdk_gpu_depth_stencil_state(&blit->depth_stencil, &depth_stencil);
}

VkResult v4x_blit_create(Blit *blit)
{
	*blit = (Blit){ .vertices = NULL };
	const V4xShader *const vertex = v4x_shader_find(&v4x_blit_bundle, v4x_blit_vert_spirv, V4X_BLIT_VERT_SPIRV_BYTES);
	const V4xShader *const pixel = v4x_shader_find(&v4x_blit_bundle, v4x_blit_frag_spirv, V4X_BLIT_FRAG_SPIRV_BYTES);
	const V4xShader *const clear = v4x_shader_find(&v4x_blit_bundle, v4x_clear_frag_spirv, V4X_CLEAR_FRAG_SPIRV_BYTES);
	if (!vertex || !pixel || !clear)
		return VK_ERROR_INITIALIZATION_FAILED;
	VkResult result = load_shaders(blit, vertex, pixel, clear);
	if (result == VK_SUCCESS)
		result = write_rectangle(blit);
	if (result != VK_SUCCESS) {
		v4x_blit_destroy(blit);
		return result;
	}
	make_state(blit);
	return VK_SUCCESS;
}

void v4x_blit_destroy(Blit *blit)
{
	xsdk_gpu_free(blit->vertices);
	xsdk_shader_free(&blit->clear);
	xsdk_shader_free(&blit->pixel);
	xsdk_shader_free(&blit->vertex);
	*blit = (Blit){ .vertices = NULL };
}

// The viewport as the replay sets one over the whole target (src/queue.c, apply_viewport).
void v4x_blit_draw(const Blit *blit, const XsdkTexture *texture, uint32_t width, uint32_t height)
{
	xsdk_gpu_set_shaders(&blit->vertex, &blit->pixel);
	xsdk_gpu_set_viewport(-0.5f, (float)height - 0.5f, (float)width, -(float)height, 0.0f, 1.0f);
	xsdk_gpu_set_scissor(0, 0, width, height);
	xsdk_gpu_set_blend(&blit->blend);
	xsdk_gpu_set_raster(&blit->raster);
	xsdk_gpu_set_depth_stencil(&blit->depth_stencil);
	xsdk_gpu_set_vertices(0, blit->vertices_address, RECTANGLE_BYTES);
	xsdk_gpu_set_texture(blit->texture_slot, texture, &blit->sampler);
	xsdk_gpu_draw(XSDK_PRIMITIVE_TRIANGLE_LIST, RECTANGLE_VERTICES);
}

// The viewport's depth range is the clear's depth alone, so every pixel takes it; depth always passes, stencil always
// passes and takes the reference, and each writes only if it's cleared.
void v4x_blit_clear(const Blit *blit, const ClearRect *clear, uint32_t width, uint32_t height)
{
	const XsdkBlend blend = { .color_source = XSDK_BLEND_ONE,
		.color_dest = XSDK_BLEND_ZERO,
		.color_op = XSDK_BLEND_OP_ADD,
		.alpha_source = XSDK_BLEND_ONE,
		.alpha_dest = XSDK_BLEND_ZERO,
		.alpha_op = XSDK_BLEND_OP_ADD,
		.write_mask = clear->clears_color ? XSDK_WRITE_ALL : 0 };
	const XsdkDepthStencil depth_stencil = { .depth_test = clear->clears_depth,
		.depth_write = clear->clears_depth,
		.depth_compare = XSDK_COMPARE_ALWAYS,
		.stencil_test = clear->clears_stencil,
		.stencil_compare = XSDK_COMPARE_ALWAYS,
		.stencil_fail = XSDK_STENCIL_REPLACE,
		.stencil_depth_fail = XSDK_STENCIL_REPLACE,
		.stencil_pass = XSDK_STENCIL_REPLACE,
		.stencil_reference = (uint8_t)clear->stencil,
		.stencil_read_mask = 0xFF,
		.stencil_write_mask = 0xFF };
	XsdkBlendState blend_state;
	XsdkDepthStencilState depth_stencil_state;
	xsdk_gpu_blend_state(&blend_state, &blend);
	xsdk_gpu_depth_stencil_state(&depth_stencil_state, &depth_stencil);
	xsdk_gpu_set_shaders(&blit->vertex, &blit->clear);
	xsdk_gpu_set_viewport(-0.5f, (float)height - 0.5f, (float)width, -(float)height, clear->depth, clear->depth);
	xsdk_gpu_set_scissor((uint32_t)clear->rect.offset.x, (uint32_t)clear->rect.offset.y, clear->rect.extent.width,
		clear->rect.extent.height);
	xsdk_gpu_set_blend(&blend_state);
	xsdk_gpu_set_raster(&blit->raster);
	xsdk_gpu_set_depth_stencil(&depth_stencil_state);
	xsdk_gpu_set_pixel_constants(blit->clear_constant, clear->color, 1);
	xsdk_gpu_set_vertices(0, blit->vertices_address, RECTANGLE_BYTES);
	xsdk_gpu_draw(XSDK_PRIMITIVE_TRIANGLE_LIST, RECTANGLE_VERTICES);
}
