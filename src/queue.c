#include <string.h>

#include <vulkan/vulkan.h>

#include "objects.h"
#include "sync.h"
#include "xsdk/gpu.h"
#include "xsdk/log.h"
#include "xsdk/texture.h"

enum {
	IDLE_TIMEOUT_MS = 10000,
	// Opaque black, for eDRAM after a pass that cleared nothing.
	BLACK_ARGB = 0xFF000000,
};

typedef struct {
	uint32_t address;
	uint32_t size_bytes;
} VertexBinding;

// What one replay has bound so far.
typedef struct {
	const Pipeline *pipeline;
	// The pipeline's registers must be set again: a resolve replaces the vertex shader, the viewport and the state.
	bool pipeline_dirty;
	// Only the viewport and scissor must, after vkCmdSetViewport or vkCmdSetScissor.
	bool viewport_dirty;
	VkViewport viewport;
	VkRect2D scissor;
	const RenderPass *render_pass;
	const Framebuffer *framebuffer;
	uint32_t clear_argb;
	float clear_depth;
	uint32_t clear_stencil;
	VertexBinding bindings[V4X_MAX_VERTEX_BINDINGS];
	uint32_t index_address;
	bool index_32_bit;
	const DescriptorSet *sets[V4X_MAX_DESCRIPTOR_SETS];
	const uint32_t *dynamic_offsets[V4X_MAX_DESCRIPTOR_SETS];
	float push[V4X_MAX_PUSH_CONSTANT_BYTES / 4];
} Replay;

// A block's registers on their way into the constants; the queue is externally synchronized, so one copy will do.
static float block[V4X_FLOAT_CONSTANTS * 4];

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

// Vulkan's y points down and its pixel centres sit on halves; xsdk's viewport is y-up with centres on whole numbers.
static void apply_viewport(const Pipeline *pipeline, const Replay *replay)
{
	const VkViewport *const v = pipeline->dynamic_viewport ? &replay->viewport : &pipeline->viewport;
	xsdk_gpu_set_viewport(v->x - 0.5f, v->y + v->height - 0.5f, v->width, -v->height, v->minDepth, v->maxDepth);
	const VkRect2D *const s = pipeline->dynamic_scissor ? &replay->scissor : &pipeline->scissor;
	xsdk_gpu_set_scissor((uint32_t)s->offset.x, (uint32_t)s->offset.y, s->extent.width, s->extent.height);
}

static void apply_pipeline(const Pipeline *pipeline, const Replay *replay)
{
	xsdk_gpu_set_shaders(&pipeline->vertex, &pipeline->pixel);
	apply_viewport(pipeline, replay);
	xsdk_gpu_set_blend(&pipeline->blend);
	xsdk_gpu_set_raster(&pipeline->raster);
	xsdk_gpu_set_depth_stencil(&pipeline->depth_stencil);
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
	for (uint32_t i = 0; i < wanted; i++) {
		const uint32_t word = i < present ? words[i] : 0;
		memcpy(&block[i], &word, sizeof word);
	}
}

// Each draw writes every block its shaders read into constants, and points their samplers' fetch constants.
static void apply_resources(const Pipeline *pipeline, const Replay *replay)
{
	for (uint32_t i = 0; i < pipeline->resource_count; i++) {
		const V4xShaderResource *const resource = &pipeline->resources[i].resource;
		const bool vertex = pipeline->resources[i].vertex;
		if (resource->kind == V4X_RESOURCE_PUSH_CONSTANTS) {
			write_constants(vertex, resource->first, replay->push, resource->count);
			continue;
		}
		if (!replay->sets[resource->set])
			continue;
		if (resource->kind == V4X_RESOURCE_UNIFORM_BUFFER) {
			read_uniform_buffer(replay, resource);
			write_constants(vertex, resource->first, block, resource->count);
			continue;
		}
		const Descriptor *const descriptor = &replay->sets[resource->set]->descriptors[resource->binding];
		xsdk_gpu_set_texture(resource->first, &descriptor->image->texture, &descriptor->sampler->state);
	}
}

// The CPU writes the image, so the GPU must be done with whatever came before (docs/ROADMAP.md, milestone 2).
static void copy_buffer_to_image(const Command *command)
{
	const NTSTATUS status = xsdk_gpu_wait(xsdk_gpu_fence(), IDLE_TIMEOUT_MS);
	if (!NT_SUCCESS(status))
		xsdk_log_write("vulkan4xenon: the GPU didn't go idle for a copy: 0x%08X", (unsigned)status);
	xsdk_texture_upload(&command->copy.image->texture, command->copy.level,
		(const void *)(uintptr_t)command->copy.source, command->copy.row_bytes);
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

// Whether eDRAM already holds what the pass's load ops clear it to.
static bool edram_ready(VkQueue queue, const Replay *replay)
{
	const RenderPass *const render_pass = replay->render_pass;
	const Edram *const edram = &queue->edram;
	const bool color = render_pass->load_op != VK_ATTACHMENT_LOAD_OP_CLEAR || edram->argb == replay->clear_argb;
	const bool depth =
		!render_pass->clears_depth || (edram->depth == replay->clear_depth && edram->stencil == replay->clear_stencil);
	return edram->known && color && depth;
}

// A clear load op needs eDRAM at the clear values; a resolve into the attachment, overwritten at the pass's end, sets
// them all.
static void begin_render_pass(VkQueue queue, Replay *replay)
{
	const RenderPass *const render_pass = replay->render_pass;
	const Framebuffer *const framebuffer = replay->framebuffer;
	xsdk_gpu_set_target(framebuffer->width, framebuffer->height);
	const bool clears = render_pass->load_op == VK_ATTACHMENT_LOAD_OP_CLEAR || render_pass->clears_depth;
	if (clears && !edram_ready(queue, replay)) {
		if (render_pass->clears_depth)
			set_depth_clear(queue, replay->clear_depth, replay->clear_stencil);
		xsdk_gpu_resolve(framebuffer->attachment->address, framebuffer->width, framebuffer->height, replay->clear_argb);
		xsdk_gpu_set_target(framebuffer->width, framebuffer->height);
		queue->edram = (Edram){
			.known = true, .argb = replay->clear_argb, .depth = queue->depth_clear, .stencil = queue->stencil_clear
		};
	}
	replay->pipeline_dirty = true;
}

// The resolve clears eDRAM to this pass's clear values, so the next frame's pass usually finds it ready.
static void end_render_pass(VkQueue queue, Replay *replay)
{
	const RenderPass *const render_pass = replay->render_pass;
	const Framebuffer *const framebuffer = replay->framebuffer;
	if (render_pass->store_op != VK_ATTACHMENT_STORE_OP_STORE) {
		queue->edram.known = false;
		return;
	}
	const uint32_t next_argb = render_pass->load_op == VK_ATTACHMENT_LOAD_OP_CLEAR ? replay->clear_argb : BLACK_ARGB;
	if (render_pass->clears_depth)
		set_depth_clear(queue, replay->clear_depth, replay->clear_stencil);
	xsdk_gpu_resolve(framebuffer->attachment->address, framebuffer->width, framebuffer->height, next_argb);
	queue->edram =
		(Edram){ .known = true, .argb = next_argb, .depth = queue->depth_clear, .stencil = queue->stencil_clear };
}

static void prepare_draw(Replay *replay)
{
	const Pipeline *const pipeline = replay->pipeline;
	if (replay->pipeline_dirty)
		apply_pipeline(pipeline, replay);
	else if (replay->viewport_dirty)
		apply_viewport(pipeline, replay);
	replay->pipeline_dirty = false;
	replay->viewport_dirty = false;
	apply_resources(pipeline, replay);
}

// xsdk draws from vertex 0, so each stream starts at the draw's first vertex, which may come before the binding's.
static void point_streams(const Replay *replay, int32_t first_vertex)
{
	const Pipeline *const pipeline = replay->pipeline;
	for (uint32_t s = 0; s < pipeline->stream_count; s++) {
		const VertexBinding *const binding = &replay->bindings[pipeline->streams[s].binding];
		const int32_t skipped = first_vertex * (int32_t)pipeline->streams[s].stride_bytes;
		xsdk_gpu_set_vertices(s, binding->address + (uint32_t)skipped, binding->size_bytes - (uint32_t)skipped);
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

static void draw_indexed(Replay *replay, const Command *command)
{
	prepare_draw(replay);
	point_streams(replay, command->draw_indexed.vertex_offset);
	const uint32_t index_bytes = replay->index_32_bit ? 4 : 2;
	const uint32_t chunk = chunk_vertices(replay->pipeline);
	const uint32_t count = command->draw_indexed.index_count;
	for (uint32_t done = 0; done < count; done += chunk) {
		const uint32_t address = replay->index_address + (command->draw_indexed.first_index + done) * index_bytes;
		const uint32_t left = count - done;
		xsdk_gpu_draw_indexed(replay->pipeline->primitive, address, left < chunk ? left : chunk, replay->index_32_bit);
	}
}

static void begin_command(VkQueue queue, Replay *replay, const Command *command)
{
	replay->render_pass = command->begin.render_pass;
	replay->framebuffer = command->begin.framebuffer;
	replay->clear_argb = command->begin.clear_argb;
	replay->clear_depth = command->begin.clear_depth;
	replay->clear_stencil = command->begin.clear_stencil;
	begin_render_pass(queue, replay);
}

static void replay_command(VkQueue queue, VkCommandBuffer buffer, Replay *replay, const Command *command)
{
	switch (command->type) {
	case COMMAND_BEGIN_RENDER_PASS:
		begin_command(queue, replay, command);
		break;
	case COMMAND_END_RENDER_PASS:
		end_render_pass(queue, replay);
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
		copy_buffer_to_image(command);
		break;
	case COMMAND_SET_VIEWPORT:
		memcpy(&replay->viewport, buffer->data + command->state_at, sizeof replay->viewport);
		replay->viewport_dirty = true;
		break;
	case COMMAND_SET_SCISSOR:
		memcpy(&replay->scissor, buffer->data + command->state_at, sizeof replay->scissor);
		replay->viewport_dirty = true;
		break;
	case COMMAND_BIND_INDEX_BUFFER:
		replay->index_address = command->index_buffer.address;
		replay->index_32_bit = command->index_buffer.is_32_bit;
		break;
	case COMMAND_DRAW_INDEXED:
		draw_indexed(replay, command);
		break;
	}
}

VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit(
	VkQueue queue, uint32_t submitCount, const VkSubmitInfo *pSubmits, VkFence fence)
{
	// Drain the CPU's write-combining, so the GPU reads what the app wrote to mapped memory.
	__asm__ volatile("sync" ::: "memory");
	for (uint32_t i = 0; i < submitCount; i++) {
		for (uint32_t j = 0; j < pSubmits[i].commandBufferCount; j++) {
			const VkCommandBuffer buffer = pSubmits[i].pCommandBuffers[j];
			Replay replay = { .pipeline_dirty = true };
			for (uint32_t c = 0; c < buffer->count; c++)
				replay_command(queue, buffer, &replay, &buffer->commands[c]);
		}
	}
	// One queue replays in order, so semaphores need nothing; the fence submits the ring either way.
	v4x_sync_submitted(v4x_object(fence), xsdk_gpu_fence());
	return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkQueueWaitIdle(VkQueue queue)
{
	(void)queue;
	const NTSTATUS status = xsdk_gpu_wait(xsdk_gpu_fence(), IDLE_TIMEOUT_MS);
	if (!NT_SUCCESS(status)) {
		xsdk_log_write("vulkan4xenon: the GPU didn't go idle: 0x%08X", (unsigned)status);
		return VK_ERROR_DEVICE_LOST;
	}
	return VK_SUCCESS;
}
