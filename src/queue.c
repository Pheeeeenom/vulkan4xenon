#include <altivec.h>
#include <assert.h>
#include <math.h>
#include <string.h>

#include <vulkan/vulkan.h>

#include "blit.h"
#include "host.h"
#include "objects.h"
#include "pipeline_state.h"
#include "sync.h"
#include "transfer.h"
#include "xsdk/gpu.h"
#include "xsdk/log.h"
#include "xsdk/texture.h"

enum {
	IDLE_TIMEOUT_MS = 10000,
	// All of physical memory, as libxenon invalidates it (libxenon:drivers/xenos/xe.c:992).
	ALL_MEMORY_BYTES = 0x20000000,
	// The GPU loads constants in whole groups of 4 from a multiple of 4, from 64-byte aligned memory
	// (xsdk:include/xsdk/gpu.h); xost gives each uniform block such groups to itself.
	CONSTANT_GROUP = 4,
	LOAD_ALIGNMENT_BYTES = 64,
};

typedef struct {
	uint32_t address;
	uint32_t size_bytes;
} VertexBinding;

typedef struct {
	const XsdkTexture *texture;
	const Sampler *sampler;
} PointedTexture;

// What one replay has bound so far.
typedef struct {
	VkQueue queue;
	// The submission it belongs to (VkQueue_T's serial).
	uint32_t serial;
	const Pipeline *pipeline;
	// The pipeline's registers must be set again: a resolve replaces the vertex shader, the viewport and the state.
	bool pipeline_dirty;
	// Only the viewport and scissor must, after vkCmdSetViewport or vkCmdSetScissor.
	bool viewport_dirty;
	VkViewport viewport;
	VkRect2D scissor;
	// And the raster, depth and stencil, and blend constants, after a command set the state they may take.
	bool dynamic_dirty;
	DynamicState dynamic;
	const RenderPass *render_pass;
	const Framebuffer *framebuffer;
	// Where the pass's depth is, in queue->depths and in eDRAM (place_depth).
	uint32_t depth_part;
	uint32_t depth_tile;
	uint32_t clear_argb;
	// The pass clears its depth: its load op says so, or its image's vkCmdClearDepthStencilImage waits (Image).
	bool clears_depth;
	float clear_depth;
	uint32_t clear_stencil;
	VertexBinding bindings[V4X_MAX_VERTEX_BINDINGS];
	uint32_t index_address;
	bool index_32_bit;
	const DescriptorSet *sets[V4X_MAX_DESCRIPTOR_SETS];
	const uint32_t *dynamic_offsets[V4X_MAX_DESCRIPTOR_SETS];
	float push[V4X_MAX_PUSH_CONSTANT_BYTES / 4];
	// What each stream and texture slot points at, so a draw points only what changed; zero where it isn't known.
	VertexBinding pointed_streams[XSDK_VERTEX_STREAMS];
	PointedTexture pointed_textures[V4X_TEXTURE_SLOTS];
} Replay;

// A block's registers on their way into the constants; the queue is externally synchronized, so one copy will do.
static float block[V4X_FLOAT_CONSTANTS * 4] __attribute__((aligned(16)));

static void write_literals(const V4xShader *shader, bool vertex)
{
	for (uint32_t i = 0; i < shader->literal_count; i++) {
		const V4xShaderLiteral *const literal = &shader->literals[i];
		if (vertex)
			xsdk_gpu_set_vertex_constants(literal->reg, literal->values, 1);
		else
			xsdk_gpu_set_pixel_constants(literal->reg, literal->values, 1);
	}
}

// The first pixel whose centre a viewport edge at coordinate covers, or the one past the last.
static int32_t first_pixel(float coordinate)
{
	return (int32_t)ceilf(coordinate - 0.5f);
}

static int32_t larger(int32_t a, int32_t b)
{
	return a > b ? a : b;
}

static int32_t smaller(int32_t a, int32_t b)
{
	return a < b ? a : b;
}

// Vulkan's y points down and its pixel centres sit on halves; xsdk's viewport is y-up with centres on whole numbers.
// Vulkan clips to the viewport, and Xenos draws past it as far as the scissor (docs/SUPPORT.md, Pipelines), so the
// scissor stops at the viewport's pixels too.
static void apply_viewport(const Pipeline *pipeline, const Replay *replay)
{
	const VkViewport *const v = pipeline->dynamic_viewport ? &replay->viewport : &pipeline->viewport;
	xsdk_gpu_set_viewport(v->x - 0.5f, v->y + v->height - 0.5f, v->width, -v->height, v->minDepth, v->maxDepth);
	const VkRect2D *const s = pipeline->dynamic_scissor ? &replay->scissor : &pipeline->scissor;
	const int32_t left = larger(s->offset.x, first_pixel(v->x));
	const int32_t top = larger(s->offset.y, first_pixel(v->y));
	const int32_t right = smaller(s->offset.x + (int32_t)s->extent.width, first_pixel(v->x + v->width));
	const int32_t bottom = smaller(s->offset.y + (int32_t)s->extent.height, first_pixel(v->y + v->height));
	xsdk_gpu_set_scissor((uint32_t)left, (uint32_t)top, right > left ? (uint32_t)(right - left) : 0,
		bottom > top ? (uint32_t)(bottom - top) : 0);
}

// The state blocks dynamic state may change: the pipeline's own, or worked out with what the commands set.
static void apply_dynamic(const Pipeline *pipeline, const Replay *replay)
{
	XsdkRasterState raster;
	XsdkDepthStencilState depth_stencil;
	xsdk_gpu_set_raster(v4x_pipeline_raster(pipeline, &replay->dynamic, &raster));
	xsdk_gpu_set_depth_stencil(v4x_pipeline_depth_stencil(pipeline, &replay->dynamic, &depth_stencil));
	const float *const constants = v4x_pipeline_blend_constants(pipeline, &replay->dynamic);
	if (constants)
		xsdk_gpu_set_blend_constants(constants);
}

static void apply_pipeline(const Pipeline *pipeline, const Replay *replay)
{
	xsdk_gpu_set_shaders(&pipeline->vertex, &pipeline->pixel);
	apply_viewport(pipeline, replay);
	xsdk_gpu_set_blend(&pipeline->blend);
	apply_dynamic(pipeline, replay);
	write_literals(pipeline->vertex_source, true);
	write_literals(pipeline->pixel_source, false);
}

static void write_constants(bool vertex, uint32_t first, const float *values, uint32_t count)
{
	if (vertex)
		xsdk_gpu_set_vertex_constants(first, values, count);
	else
		xsdk_gpu_set_pixel_constants(first, values, count);
}

// A uniform buffer's bytes as the block's registers, zero past the descriptor's range.
static void read_uniform_buffer(const Replay *replay, const V4xShaderResource *resource)
{
	const DescriptorSet *const set = replay->sets[resource->set];
	const Descriptor *const descriptor = &set->descriptors[resource->binding];
	const LayoutBinding *const binding = &set->layout->bindings[resource->binding];
	volatile const uint32_t *words = descriptor->buffer;
	if (binding->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC)
		words += replay->dynamic_offsets[resource->set][binding->dynamic_index] / 4;
	const uint32_t wanted = resource->count * 4;
	const uint32_t present = descriptor->range_bytes / 4 < wanted ? descriptor->range_bytes / 4 : wanted;
	uint32_t i = 0;
	// Mapped memory is write-combined, so each read goes to memory: a quad at a time is a quarter of the reads.
	if (((uintptr_t)words & 15) == 0) {
		for (; i + 4 <= present; i += 4)
			vec_st(vec_ld(0, (const vector unsigned int *)(uintptr_t)(words + i)), 0, (vector unsigned int *)&block[i]);
	}
	for (; i < wanted; i++) {
		const uint32_t word = i < present ? words[i] : 0;
		memcpy(&block[i], &word, sizeof word);
	}
}

// Each draw writes every block its shaders read into constants, and points their samplers' fetch constants.
// Has the GPU load the block straight from the buffer, when the block starts a group and the buffer covers it; the
// padding past the block takes whatever follows it, which nothing reads.
static bool load_uniform_buffer(const Replay *replay, const V4xShaderResource *resource, bool vertex)
{
	const DescriptorSet *const set = replay->sets[resource->set];
	const Descriptor *const descriptor = &set->descriptors[resource->binding];
	const LayoutBinding *const binding = &set->layout->bindings[resource->binding];
	uint32_t address = descriptor->buffer_address;
	if (binding->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC)
		address += replay->dynamic_offsets[resource->set][binding->dynamic_index];
	if (resource->first % CONSTANT_GROUP || address % LOAD_ALIGNMENT_BYTES ||
		descriptor->range_bytes < resource->count * 16)
		return false;
	const uint32_t count = (resource->count + CONSTANT_GROUP - 1) / CONSTANT_GROUP * CONSTANT_GROUP;
	if (vertex)
		xsdk_gpu_load_vertex_constants(resource->first, address, count);
	else
		xsdk_gpu_load_pixel_constants(resource->first, address, count);
	return true;
}

// Points a slot at the texture and sampler unless it already is. A replay points every slot afresh, so each submission
// that samples an image marks it.
static void point_texture(Replay *replay, uint32_t slot, const Descriptor *descriptor)
{
	assert(slot < V4X_TEXTURE_SLOTS);
	PointedTexture *const pointed = &replay->pointed_textures[slot];
	if (pointed->texture == descriptor->texture && pointed->sampler == descriptor->sampler)
		return;
	xsdk_gpu_set_texture(slot, descriptor->texture, &descriptor->sampler->state);
	descriptor->image->used_serial = replay->serial;
	*pointed = (PointedTexture){ .texture = descriptor->texture, .sampler = descriptor->sampler };
}

// A loop's count as the draw has its block: the int at the resource's byte offset, none where the block isn't bound or
// its range doesn't reach it. A loop constant runs 0 to 255 passes (xenia:src/xenia/gpu/xenos.h:1086-1097).
static uint32_t loop_count(const Replay *replay, const V4xShaderResource *resource)
{
	int32_t count = 0;
	if (resource->kind == V4X_RESOURCE_PUSH_LOOP) {
		memcpy(&count, (const uint8_t *)replay->push + resource->first, sizeof count);
	} else if (replay->sets[resource->set]) {
		const DescriptorSet *const set = replay->sets[resource->set];
		const Descriptor *const descriptor = &set->descriptors[resource->binding];
		const LayoutBinding *const binding = &set->layout->bindings[resource->binding];
		uint32_t offset = resource->first;
		if (binding->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC)
			offset += replay->dynamic_offsets[resource->set][binding->dynamic_index];
		if (resource->first + sizeof count <= descriptor->range_bytes)
			count = (int32_t)descriptor->buffer[offset / 4];
	}
	return count < 0 ? 0 : count > V4X_MAX_LOOP_PASSES ? V4X_MAX_LOOP_PASSES : (uint32_t)count;
}

// xsdk rewrites every boolean and loop word together (xsdk:include/xsdk/gpu.h), so they're written only when one of
// the loop constants changes.
static void write_loops(VkQueue queue, const uint32_t *values)
{
	if (queue->loops_written && !memcmp(values, queue->loop_constants, sizeof queue->loop_constants))
		return;
	xsdk_gpu_set_loop_constants(0, values, XSDK_LOOP_CONSTANTS);
	memcpy(queue->loop_constants, values, sizeof queue->loop_constants);
	queue->loops_written = true;
}

// The loop constants a pipeline's hardware loops read, each from its count.
static void apply_loops(const Pipeline *pipeline, const Replay *replay)
{
	uint32_t values[XSDK_LOOP_CONSTANTS];
	memcpy(values, replay->queue->loop_constants, sizeof values);
	for (uint32_t i = 0; i < pipeline->resource_count; i++) {
		const V4xShaderResource *const resource = &pipeline->resources[i].resource;
		if (resource->kind == V4X_RESOURCE_PUSH_LOOP || resource->kind == V4X_RESOURCE_UNIFORM_LOOP)
			values[resource->count] = xsdk_gpu_loop_constant(loop_count(replay, resource), 0, 1);
	}
	write_loops(replay->queue, values);
}

static void apply_resources(const Pipeline *pipeline, Replay *replay)
{
	if (pipeline->loop_count)
		apply_loops(pipeline, replay);
	for (uint32_t i = 0; i < pipeline->resource_count; i++) {
		const V4xShaderResource *const resource = &pipeline->resources[i].resource;
		const bool vertex = pipeline->resources[i].vertex;
		if (resource->kind == V4X_RESOURCE_PUSH_CONSTANTS) {
			write_constants(vertex, resource->first, replay->push, resource->count);
			continue;
		}
		if (resource->kind == V4X_RESOURCE_PUSH_LOOP || resource->kind == V4X_RESOURCE_UNIFORM_LOOP)
			continue;
		if (!replay->sets[resource->set])
			continue;
		if (resource->kind == V4X_RESOURCE_UNIFORM_BUFFER) {
			if (!load_uniform_buffer(replay, resource, vertex)) {
				read_uniform_buffer(replay, resource);
				write_constants(vertex, resource->first, block, resource->count);
			}
			continue;
		}
		if (resource->kind == V4X_RESOURCE_COMPARISON) {
			const Sampler *const sampler = replay->sets[resource->set]->descriptors[resource->binding].sampler;
			write_constants(vertex, resource->first, sampler->compare_weights, 1);
			continue;
		}
		point_texture(replay, resource->first, &replay->sets[resource->set]->descriptors[resource->binding]);
	}
}

// Resolves clear eDRAM's depth and stencil to what xsdk_gpu_set_depth_clear last took.
static void set_depth_clear(VkQueue queue, float depth, uint32_t stencil)
{
	if (queue->depth_clear == depth && queue->stencil_clear == stencil)
		return;
	xsdk_gpu_set_depth_clear(depth, (uint8_t)stencil);
	queue->depth_clear = depth;
	queue->stencil_clear = stencil;
}

// Colour from tile 0 and depth where place_depth put it (docs/ROADMAP.md, milestones 4 and 14).
static void set_target(const Replay *replay)
{
	const Framebuffer *const framebuffer = replay->framebuffer;
	xsdk_gpu_set_target_at(framebuffer->width, framebuffer->height, 0, replay->depth_tile);
}

// Copies eDRAM's colour into the attachment, then clears what's asked; the target must be set again after.
static void resolve(Image *attachment, uint32_t serial, bool clear_color, bool clear_depth, uint32_t argb)
{
	xsdk_gpu_set_resolve_clears(clear_color, clear_depth);
	xsdk_gpu_resolve_texture(&attachment->texture, argb);
	attachment->used_serial = serial;
}

static bool same_size(const EdramPart *part, const Framebuffer *framebuffer)
{
	return part->width == framebuffer->width && part->height == framebuffer->height;
}

static EdramPart cleared_color(const Framebuffer *framebuffer, uint32_t argb)
{
	return (EdramPart){
		.contents = EDRAM_CLEARED, .width = framebuffer->width, .height = framebuffer->height, .argb = argb
	};
}

static EdramPart cleared_depth(const Framebuffer *framebuffer, uint32_t tile, float depth, uint32_t stencil)
{
	return (EdramPart){ .contents = EDRAM_CLEARED,
		.width = framebuffer->width,
		.height = framebuffer->height,
		.depth = depth,
		.stencil = stencil,
		.tile = tile };
}

static EdramPart holding(const Image *image, uint32_t tile)
{
	return (EdramPart){
		.contents = EDRAM_IMAGE, .width = image->width, .height = image->height, .image = image, .tile = tile
	};
}

// Whether eDRAM's colour is what the pass's load op wants there.
static bool color_ready(const EdramPart *color, const Replay *replay)
{
	const Framebuffer *const framebuffer = replay->framebuffer;
	switch (replay->render_pass->load_op) {
	case VK_ATTACHMENT_LOAD_OP_CLEAR:
		return color->contents == EDRAM_CLEARED && same_size(color, framebuffer) && color->argb == replay->clear_argb;
	case VK_ATTACHMENT_LOAD_OP_LOAD:
		return color->contents == EDRAM_IMAGE && color->image == framebuffer->attachment;
	default:
		return true;
	}
}

static bool depth_ready(const EdramPart *depth, const Replay *replay)
{
	return depth->contents == EDRAM_CLEARED && same_size(depth, replay->framebuffer) &&
		depth->depth == replay->clear_depth && depth->stencil == replay->clear_stencil;
}

// Whether a depth eDRAM holds reaches into tiles first to first + tiles.
static bool part_overlaps(const EdramPart *part, uint32_t first, uint32_t tiles)
{
	return part->contents != EDRAM_UNKNOWN && part->tile < first + tiles &&
		first < part->tile + xsdk_gpu_target_tiles(part->width, part->height);
}

// Whether tiles first to first + tiles stay above the pass's colour and miss every depth image eDRAM holds.
static bool depth_tiles_free(VkQueue queue, uint32_t first, uint32_t tiles, uint32_t color_tiles)
{
	if (first < color_tiles)
		return false;
	for (uint32_t i = 0; i < V4X_EDRAM_DEPTHS; i++) {
		if (queue->depths[i].contents == EDRAM_IMAGE && part_overlaps(&queue->depths[i], first, tiles))
			return false;
	}
	return true;
}

// Where a depth eDRAM doesn't hold goes: ending at the top, as milestone 4 placed every depth, or else just under a
// depth image eDRAM holds, so that image keeps its depth for a later pass to load; at the top all the same when
// neither fits.
static uint32_t new_depth_tile(VkQueue queue, uint32_t tiles, uint32_t color_tiles)
{
	const uint32_t top = XSDK_EDRAM_TILES - tiles;
	if (depth_tiles_free(queue, top, tiles, color_tiles))
		return top;
	for (uint32_t i = 0; i < V4X_EDRAM_DEPTHS; i++) {
		const EdramPart *const part = &queue->depths[i];
		if (part->contents == EDRAM_IMAGE && part->tile >= tiles &&
			depth_tiles_free(queue, part->tile - tiles, tiles, color_tiles))
			return part->tile - tiles;
	}
	return top;
}

// Places the pass's depth: where eDRAM holds its image's, or the clear the pass wants at its size, or at new tiles.
// Whatever depth the pass's colour or depth then reaches is lost, as colour drawn over it overwrites it.
static void place_depth(VkQueue queue, Replay *replay)
{
	const RenderPass *const render_pass = replay->render_pass;
	const Framebuffer *const framebuffer = replay->framebuffer;
	const uint32_t tiles = xsdk_gpu_target_tiles(framebuffer->width, framebuffer->height);
	uint32_t part = V4X_EDRAM_DEPTHS;
	for (uint32_t i = 0; i < V4X_EDRAM_DEPTHS && render_pass->has_depth; i++) {
		const EdramPart *const depth = &queue->depths[i];
		if ((depth->contents == EDRAM_IMAGE && depth->image == framebuffer->depth) ||
			(part == V4X_EDRAM_DEPTHS && replay->clears_depth && depth_ready(depth, replay)))
			part = i;
	}
	const uint32_t tile = part < V4X_EDRAM_DEPTHS ? queue->depths[part].tile
												  : new_depth_tile(queue, tiles, render_pass->has_depth ? tiles : 0);
	for (uint32_t i = 0; i < V4X_EDRAM_DEPTHS; i++) {
		if (i != part &&
			(part_overlaps(&queue->depths[i], 0, tiles) ||
				(render_pass->has_depth && part_overlaps(&queue->depths[i], tile, tiles))))
			queue->depths[i] = (EdramPart){ .contents = EDRAM_UNKNOWN };
	}
	// A new depth takes a part eDRAM no longer holds, or else the last.
	for (uint32_t i = 0; i < V4X_EDRAM_DEPTHS && part == V4X_EDRAM_DEPTHS; i++) {
		if (queue->depths[i].contents == EDRAM_UNKNOWN)
			part = i;
	}
	if (part == V4X_EDRAM_DEPTHS)
		part = V4X_EDRAM_DEPTHS - 1;
	replay->depth_part = part;
	replay->depth_tile = render_pass->has_depth ? tile : XSDK_EDRAM_TILES - tiles;
}

// Depth never leaves eDRAM, so loading it after eDRAM lost it finds undefined contents.
static void check_depth_kept(VkQueue queue, const Replay *replay)
{
	static bool logged;
	const EdramPart *const depth = &queue->depths[replay->depth_part];
	if (logged || (depth->contents == EDRAM_IMAGE && depth->image == replay->framebuffer->depth))
		return;
	xsdk_log_write("vulkan4xenon: a depth attachment was loaded after eDRAM lost it; its contents are undefined");
	logged = true;
}

// A load draws the image back unless eDRAM holds it; a clear needs eDRAM at the clear values, which a resolve into the
// attachment sets, copying junk the pass overwrites, or the image itself after a load.
static void begin_render_pass(VkQueue queue, Replay *replay)
{
	const RenderPass *const render_pass = replay->render_pass;
	const Framebuffer *const framebuffer = replay->framebuffer;
	place_depth(queue, replay);
	set_target(replay);
	if (render_pass->has_depth && render_pass->loads_depth && !replay->clears_depth)
		check_depth_kept(queue, replay);
	if (!color_ready(&queue->color, replay) && render_pass->load_op == VK_ATTACHMENT_LOAD_OP_LOAD) {
		v4x_blit_draw(&queue->blit, &framebuffer->attachment->texture, framebuffer->width, framebuffer->height);
		framebuffer->attachment->used_serial = replay->serial;
		queue->color = holding(framebuffer->attachment, 0);
	}
	EdramPart *const depth = &queue->depths[replay->depth_part];
	const bool clear_color = !color_ready(&queue->color, replay);
	const bool clear_depth = replay->clears_depth && !depth_ready(depth, replay);
	if (clear_color || clear_depth) {
		if (clear_depth)
			set_depth_clear(queue, replay->clear_depth, replay->clear_stencil);
		resolve(framebuffer->attachment, replay->serial, clear_color, clear_depth, replay->clear_argb);
		set_target(replay);
		if (clear_color)
			queue->color = cleared_color(framebuffer, replay->clear_argb);
		if (clear_depth)
			*depth = cleared_depth(framebuffer, replay->depth_tile, replay->clear_depth, replay->clear_stencil);
	}
	replay->pipeline_dirty = true;
	// Drawing an image back and resolves point streams and slots of their own.
	memset(replay->pointed_streams, 0, sizeof replay->pointed_streams);
	memset(replay->pointed_textures, 0, sizeof replay->pointed_textures);
}

// The resolve clears colour as the next pass in the command buffer clears it, at this size, and keeps it otherwise;
// the last pass clears to its own colour, so the next frame finds it ready.
static bool end_color_clear(const Replay *replay, const Command *next, uint32_t *argb)
{
	if (!next) {
		*argb = replay->clear_argb;
		return replay->render_pass->load_op == VK_ATTACHMENT_LOAD_OP_CLEAR;
	}
	const Framebuffer *const framebuffer = next->begin.framebuffer;
	*argb = next->begin.clear_argb;
	return next->begin.render_pass->load_op == VK_ATTACHMENT_LOAD_OP_CLEAR &&
		framebuffer->width == replay->framebuffer->width && framebuffer->height == replay->framebuffer->height;
}

// The same for depth; a pass without depth never clears it, as a later pass may load an earlier one's.
static bool end_depth_clear(const Replay *replay, const Command *next, float *depth, uint32_t *stencil)
{
	if (!replay->render_pass->has_depth)
		return false;
	if (!next) {
		*depth = replay->clear_depth;
		*stencil = replay->clear_stencil;
		return replay->render_pass->clears_depth;
	}
	const Framebuffer *const framebuffer = next->begin.framebuffer;
	*depth = next->begin.clear_depth;
	*stencil = next->begin.clear_stencil;
	return next->begin.render_pass->clears_depth && framebuffer->width == replay->framebuffer->width &&
		framebuffer->height == replay->framebuffer->height;
}

// A sampled depth attachment the pass leaves to be read goes into its texture first, clearing nothing, so the colour's
// resolve clears as it would.
static void resolve_depth(Image *depth, uint32_t serial)
{
	xsdk_gpu_set_resolve_clears(false, false);
	xsdk_gpu_resolve_depth_texture(&depth->texture);
	depth->used_serial = serial;
}

static void end_render_pass(VkQueue queue, Replay *replay, const Command *next)
{
	const RenderPass *const render_pass = replay->render_pass;
	const Framebuffer *const framebuffer = replay->framebuffer;
	EdramPart *const depth_part = &queue->depths[replay->depth_part];
	if (render_pass->has_depth)
		*depth_part = holding(framebuffer->depth, replay->depth_tile);
	if (render_pass->has_depth && render_pass->leaves_depth_to_read && framebuffer->depth->is_sampled_depth)
		resolve_depth(framebuffer->depth, replay->serial);
	if (render_pass->store_op != VK_ATTACHMENT_STORE_OP_STORE) {
		queue->color = (EdramPart){ .contents = EDRAM_UNKNOWN };
		return;
	}
	uint32_t argb = 0;
	float depth = 0.0f;
	uint32_t stencil = 0;
	const bool clear_color = end_color_clear(replay, next, &argb);
	const bool clear_depth = end_depth_clear(replay, next, &depth, &stencil);
	if (clear_depth)
		set_depth_clear(queue, depth, stencil);
	resolve(framebuffer->attachment, replay->serial, clear_color, clear_depth, argb);
	queue->color = clear_color ? cleared_color(framebuffer, argb) : holding(framebuffer->attachment, 0);
	if (clear_depth)
		*depth_part = cleared_depth(framebuffer, replay->depth_tile, depth, stencil);
}

// The next render pass in the command buffer after this command, if there is one.
static const Command *next_render_pass(VkCommandBuffer buffer, uint32_t after)
{
	for (uint32_t c = after + 1; c < buffer->count; c++) {
		if (buffer->commands[c].type == COMMAND_BEGIN_RENDER_PASS)
			return &buffer->commands[c];
	}
	return NULL;
}

static void prepare_draw(Replay *replay)
{
	const Pipeline *const pipeline = replay->pipeline;
	replay->queue->drawn = true;
	if (replay->pipeline_dirty) {
		apply_pipeline(pipeline, replay);
	} else {
		if (replay->viewport_dirty)
			apply_viewport(pipeline, replay);
		if (replay->dynamic_dirty)
			apply_dynamic(pipeline, replay);
	}
	replay->pipeline_dirty = false;
	replay->viewport_dirty = false;
	replay->dynamic_dirty = false;
	apply_resources(pipeline, replay);
}

// xsdk draws from vertex 0, so each stream starts at the draw's first vertex, which may come before the binding's.
static void point_streams(Replay *replay, int32_t first_vertex)
{
	const Pipeline *const pipeline = replay->pipeline;
	for (uint32_t s = 0; s < pipeline->stream_count; s++) {
		const VertexBinding *const binding = &replay->bindings[pipeline->streams[s].binding];
		const int32_t skipped = first_vertex * (int32_t)pipeline->streams[s].stride_bytes;
		const VertexBinding stream = { .address = binding->address + (uint32_t)skipped,
			.size_bytes = binding->size_bytes - (uint32_t)skipped };
		if (replay->pointed_streams[s].address == stream.address &&
			replay->pointed_streams[s].size_bytes == stream.size_bytes)
			continue;
		xsdk_gpu_set_vertices(s, stream.address, stream.size_bytes);
		replay->pointed_streams[s] = stream;
	}
}

// Lists longer than a draw packet carries are split on whole primitives; longer strips and fans were refused.
static uint32_t chunk_vertices(const Pipeline *pipeline)
{
	return pipeline->list_vertices ? V4X_MAX_DRAW_VERTICES - V4X_MAX_DRAW_VERTICES % pipeline->list_vertices
								   : V4X_MAX_DRAW_VERTICES;
}

static void draw(Replay *replay, uint32_t vertex_count, uint32_t first_vertex)
{
	prepare_draw(replay);
	const uint32_t chunk = chunk_vertices(replay->pipeline);
	for (uint32_t done = 0; done < vertex_count; done += chunk) {
		point_streams(replay, (int32_t)(first_vertex + done));
		const uint32_t left = vertex_count - done;
		xsdk_gpu_draw(replay->pipeline->primitive, left < chunk ? left : chunk);
	}
}

static void draw_indexed(Replay *replay, uint32_t count, uint32_t first_index, int32_t vertex_offset)
{
	prepare_draw(replay);
	point_streams(replay, vertex_offset);
	const uint32_t index_bytes = replay->index_32_bit ? 4 : 2;
	const uint32_t chunk = chunk_vertices(replay->pipeline);
	for (uint32_t done = 0; done < count; done += chunk) {
		const uint32_t address = replay->index_address + (first_index + done) * index_bytes;
		const uint32_t left = count - done;
		xsdk_gpu_draw_indexed(replay->pipeline->primitive, address, left < chunk ? left : chunk, replay->index_32_bit);
	}
}

// What a direct draw refuses as it's recorded, an indirect one refuses as it's replayed, drawing nothing.
static bool drawable(const Replay *replay, uint32_t count, uint32_t instances, uint32_t first_instance)
{
	if (!count || !instances)
		return false;
	if (instances != 1 || first_instance) {
		(void)v4x_host_unsupported("instanced draws");
		return false;
	}
	if (count > V4X_MAX_DRAW_VERTICES && !replay->pipeline->list_vertices) {
		(void)v4x_host_unsupported("strips and fans of more than 65535 vertices");
		return false;
	}
	return true;
}

// Its parameters are read now, in order with the replay: the host wrote them before the submit, or a copy in it did.
// They're a VkDrawIndirectCommand: vertexCount, instanceCount, firstVertex, firstInstance.
static void draw_indirect(Replay *replay, volatile const uint32_t *parameters)
{
	const uint32_t count = parameters[0];
	if (drawable(replay, count, parameters[1], parameters[3]))
		draw(replay, count, parameters[2]);
}

// The same with a VkDrawIndexedIndirectCommand: indexCount, instanceCount, firstIndex, vertexOffset, firstInstance.
static void draw_indexed_indirect(Replay *replay, volatile const uint32_t *parameters)
{
	const uint32_t count = parameters[0];
	if (drawable(replay, count, parameters[1], parameters[4]))
		draw_indexed(replay, count, parameters[2], (int32_t)parameters[3]);
}

static void set_dynamic(Replay *replay, const Command *command)
{
	DynamicState *const dynamic = &replay->dynamic;
	if (command->type == COMMAND_SET_DEPTH_BIAS) {
		dynamic->depth_bias_constant = command->depth_bias.constant;
		dynamic->depth_bias_slope = command->depth_bias.slope;
	} else if (command->type == COMMAND_SET_BLEND_CONSTANTS) {
		memcpy(dynamic->blend_constants, command->blend_constants, sizeof dynamic->blend_constants);
	} else if (command->stencil.state == VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK) {
		dynamic->stencil_compare_mask = command->stencil.value;
	} else if (command->stencil.state == VK_DYNAMIC_STATE_STENCIL_WRITE_MASK) {
		dynamic->stencil_write_mask = command->stencil.value;
	} else {
		dynamic->stencil_reference = command->stencil.value;
	}
	replay->dynamic_dirty = true;
}

static void begin_command(VkQueue queue, Replay *replay, const Command *command)
{
	replay->render_pass = command->begin.render_pass;
	replay->framebuffer = command->begin.framebuffer;
	replay->clear_argb = command->begin.clear_argb;
	replay->clear_depth = command->begin.clear_depth;
	replay->clear_stencil = command->begin.clear_stencil;
	replay->clears_depth = replay->render_pass->clears_depth;
	Image *const depth = replay->render_pass->has_depth ? replay->framebuffer->depth : NULL;
	if (depth && depth->clear_pending && !replay->clears_depth) {
		replay->clears_depth = true;
		replay->clear_depth = depth->pending_depth;
		replay->clear_stencil = depth->pending_stencil;
	}
	if (depth)
		depth->clear_pending = false;
	begin_render_pass(queue, replay);
}

// A depth image's depth lives in eDRAM, so its clear waits for the next pass to load it; a sampled one's texture
// takes the clear now too, for a shader to sample.
static void clear_depth_image(VkQueue queue, const Command *command)
{
	Image *const image = command->depth_clear.image;
	image->clear_pending = true;
	image->pending_depth = command->depth_clear.depth;
	image->pending_stencil = command->depth_clear.stencil;
	if (image->is_sampled_depth)
		v4x_transfer_fill_depth(queue, image, command->depth_clear.depth, command->depth_clear.stencil);
}

// The clear draws with the layer's own shaders and state, and its rectangle in stream 0, so they're set again after.
static void clear_attachments(VkQueue queue, Replay *replay, const ClearRect *clear)
{
	v4x_blit_clear(&queue->blit, clear, replay->framebuffer->width, replay->framebuffer->height);
	replay->pipeline_dirty = true;
	replay->pointed_streams[0] = (VertexBinding){ 0 };
}

// The GPU waits for each event's word, then its caches take what the host wrote before it set one.
// shortcut: what the replay reads on the CPU at vkQueueSubmit (copies, indirect draws' parameters, uniform buffers the
// GPU can't load) doesn't wait for an event the host sets later; hold that work back for the event if a port needs it.
static void wait_events(VkCommandBuffer buffer, const Command *command)
{
	for (uint32_t i = 0; i < command->wait_events.count; i++) {
		VkEvent event;
		memcpy(&event, buffer->data + command->wait_events.events_at + i * sizeof event, sizeof event);
		xsdk_gpu_wait_word(((const Event *)v4x_object(event))->address, 1);
	}
	xsdk_gpu_invalidate(0, ALL_MEMORY_BYTES);
}

static void write_timestamp(const Replay *replay, const Command *command)
{
	QueryPool *const pool = command->timestamp.pool;
	const uint32_t slot_bytes = V4X_QUERY_SLOT_WORDS * sizeof(uint32_t);
	xsdk_gpu_write_clock(pool->address + command->timestamp.query * slot_bytes, command->timestamp.after_work);
	pool->used_serial = replay->serial;
}

static void replay_command(VkQueue queue, VkCommandBuffer buffer, Replay *replay, uint32_t index)
{
	const Command *const command = &buffer->commands[index];
	switch (command->type) {
	case COMMAND_BEGIN_RENDER_PASS:
		begin_command(queue, replay, command);
		break;
	case COMMAND_END_RENDER_PASS:
		end_render_pass(queue, replay, next_render_pass(buffer, index));
		break;
	case COMMAND_BIND_PIPELINE:
		replay->pipeline = command->pipeline;
		replay->pipeline_dirty = true;
		break;
	case COMMAND_BIND_VERTEX_BUFFER:
		replay->bindings[command->vertex_buffer.binding] = (VertexBinding){ .address = command->vertex_buffer.address,
			.size_bytes = command->vertex_buffer.size_bytes };
		break;
	case COMMAND_DRAW:
		draw(replay, command->draw.vertex_count, command->draw.first_vertex);
		break;
	case COMMAND_BIND_DESCRIPTOR_SET:
		replay->sets[command->descriptor_set.index] = command->descriptor_set.set;
		replay->dynamic_offsets[command->descriptor_set.index] =
			(const uint32_t *)(buffer->data + command->descriptor_set.offsets_at);
		break;
	case COMMAND_PUSH_CONSTANTS:
		memcpy((uint8_t *)replay->push + command->push_constants.offset_bytes,
			buffer->data + command->push_constants.data_at, command->push_constants.size_bytes);
		break;
	case COMMAND_COPY_BUFFER_TO_IMAGE:
		v4x_transfer_buffer_to_image(queue, (const LevelCopy *)(buffer->data + command->state_at));
		break;
	case COMMAND_COPY_IMAGE_TO_BUFFER:
		v4x_transfer_image_to_buffer(queue, (const LevelCopy *)(buffer->data + command->state_at));
		break;
	case COMMAND_COPY_IMAGE:
		v4x_transfer_copy_image(queue, command);
		break;
	case COMMAND_BLIT_IMAGE:
		v4x_transfer_blit(queue, command);
		break;
	case COMMAND_CLEAR_COLOR_IMAGE:
		v4x_transfer_clear(queue, command);
		break;
	case COMMAND_COPY_BUFFER:
		v4x_transfer_copy_buffer(queue, command, command->buffer_copy.source);
		break;
	case COMMAND_UPDATE_BUFFER:
		v4x_transfer_copy_buffer(queue, command, buffer->data + command->buffer_copy.data);
		break;
	case COMMAND_FILL_BUFFER:
		v4x_transfer_fill_buffer(queue, command);
		break;
	case COMMAND_SET_VIEWPORT:
		memcpy(&replay->viewport, buffer->data + command->state_at, sizeof replay->viewport);
		replay->viewport_dirty = true;
		break;
	case COMMAND_SET_SCISSOR:
		memcpy(&replay->scissor, buffer->data + command->state_at, sizeof replay->scissor);
		replay->viewport_dirty = true;
		break;
	case COMMAND_SET_STENCIL:
	case COMMAND_SET_DEPTH_BIAS:
	case COMMAND_SET_BLEND_CONSTANTS:
		set_dynamic(replay, command);
		break;
	case COMMAND_CLEAR_ATTACHMENTS:
		clear_attachments(queue, replay, (const ClearRect *)(buffer->data + command->state_at));
		break;
	case COMMAND_CLEAR_DEPTH_IMAGE:
		clear_depth_image(queue, command);
		break;
	// In place, with the state the replay has bound; a secondary command buffer begins and ends no render pass.
	case COMMAND_EXECUTE_COMMANDS:
		for (uint32_t c = 0; c < command->secondary->count; c++)
			replay_command(queue, command->secondary, replay, c);
		break;
	case COMMAND_BIND_INDEX_BUFFER:
		replay->index_address = command->index_buffer.address;
		replay->index_32_bit = command->index_buffer.is_32_bit;
		break;
	case COMMAND_DRAW_INDEXED:
		draw_indexed(replay, command->draw_indexed.index_count, command->draw_indexed.first_index,
			command->draw_indexed.vertex_offset);
		break;
	case COMMAND_DRAW_INDIRECT:
		draw_indirect(replay, command->parameters);
		break;
	case COMMAND_DRAW_INDEXED_INDIRECT:
		draw_indexed_indirect(replay, command->parameters);
		break;
	case COMMAND_WRITE_EVENT:
		xsdk_gpu_write_word(command->event.address, command->event.value);
		break;
	case COMMAND_WAIT_EVENTS:
		wait_events(buffer, command);
		break;
	case COMMAND_RESET_QUERIES:
		v4x_transfer_reset_queries(queue, command);
		break;
	case COMMAND_WRITE_TIMESTAMP:
		write_timestamp(replay, command);
		break;
	case COMMAND_COPY_QUERY_RESULTS:
		v4x_transfer_copy_queries(queue, (const QueryCopy *)(buffer->data + command->state_at));
		break;
	}
}

VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit(
	VkQueue queue, uint32_t submitCount, const VkSubmitInfo *pSubmits, VkFence fence)
{
	// Drain the CPU's write-combining, so the GPU reads what the app wrote to mapped memory.
	__asm__ volatile("sync" ::: "memory");
	// Mapped memory is coherent, so the app may have written any of it; one invalidate covers it all, as xsdk's set-up
	// covers memory (xsdk:src/gpu_setup.c).
	xsdk_gpu_invalidate(0, ALL_MEMORY_BYTES);
	queue->serial = queue->serial + 1 ? queue->serial + 1 : 1;
	for (uint32_t i = 0; i < submitCount; i++) {
		for (uint32_t j = 0; j < pSubmits[i].commandBufferCount; j++) {
			const VkCommandBuffer buffer = pSubmits[i].pCommandBuffers[j];
			Replay replay = { .queue = queue, .serial = queue->serial, .pipeline_dirty = true };
			for (uint32_t c = 0; c < buffer->count; c++)
				replay_command(queue, buffer, &replay, c);
		}
	}
	// One queue replays in order, so semaphores need nothing; the fence submits the ring either way.
	const uint32_t submitted = xsdk_gpu_fence();
	queue->submit_fences[queue->serial % V4X_SUBMIT_FENCES] = submitted;
	v4x_sync_submitted(v4x_object(fence), submitted);
	return VK_SUCCESS;
}

// The queue family has no sparse binding, as the GPU addresses physical memory (docs/SUPPORT.md, Device).
VKAPI_ATTR VkResult VKAPI_CALL vkQueueBindSparse(
	VkQueue queue, uint32_t bindInfoCount, const VkBindSparseInfo *pBindInfo, VkFence fence)
{
	(void)queue;
	(void)bindInfoCount;
	(void)pBindInfo;
	(void)fence;
	return v4x_host_unsupported("sparse binding");
}

VKAPI_ATTR VkResult VKAPI_CALL vkQueueWaitIdle(VkQueue queue)
{
	const NTSTATUS status = xsdk_gpu_wait(xsdk_gpu_fence(), IDLE_TIMEOUT_MS);
	if (!NT_SUCCESS(status)) {
		xsdk_log_write("vulkan4xenon: the GPU didn't go idle: 0x%08X", (unsigned)status);
		return VK_ERROR_DEVICE_LOST;
	}
	queue->drawn = false;
	return VK_SUCCESS;
}
