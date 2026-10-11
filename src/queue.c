#include <vulkan/vulkan.h>

#include "objects.h"
#include "sync.h"
#include "xsdk/gpu.h"
#include "xsdk/log.h"

enum {
	// DRAW_INDX_2 carries the vertex count in 16 bits (xsdk:src/gpu_draw.c:219), and this is whole triangles.
	MAX_DRAW_VERTICES = 65535,
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
	// The pipeline's registers must be set again: a resolve replaces the vertex shader and the viewport.
	bool pipeline_dirty;
	const RenderPass *render_pass;
	const Framebuffer *framebuffer;
	uint32_t clear_argb;
	VertexBinding bindings[V4X_MAX_VERTEX_BINDINGS];
} Replay;

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
static void apply_pipeline(const Pipeline *pipeline)
{
	xsdk_gpu_set_shaders(&pipeline->vertex, &pipeline->pixel);
	const VkViewport *const v = &pipeline->viewport;
	xsdk_gpu_set_viewport(v->x - 0.5f, v->y + v->height - 0.5f, v->width, -v->height, v->minDepth, v->maxDepth);
	const VkRect2D *const s = &pipeline->scissor;
	xsdk_gpu_set_scissor((uint32_t)s->offset.x, (uint32_t)s->offset.y, s->extent.width, s->extent.height);
	write_literals(pipeline->vertex_source, true);
	write_literals(pipeline->pixel_source, false);
}

// A clear load op needs eDRAM at the clear colour; a resolve into the attachment, overwritten at the pass's end, does
// it.
static void begin_render_pass(VkQueue queue, Replay *replay)
{
	const Framebuffer *const framebuffer = replay->framebuffer;
	xsdk_gpu_set_target(framebuffer->width, framebuffer->height);
	const bool clears = replay->render_pass->load_op == VK_ATTACHMENT_LOAD_OP_CLEAR;
	if (clears && !(queue->edram.known && queue->edram.argb == replay->clear_argb)) {
		xsdk_gpu_resolve(framebuffer->attachment->address, framebuffer->width, framebuffer->height, replay->clear_argb);
		xsdk_gpu_set_target(framebuffer->width, framebuffer->height);
		queue->edram = (Edram){ .known = true, .argb = replay->clear_argb };
	}
	replay->pipeline_dirty = true;
}

// The resolve clears eDRAM to this pass's clear colour, so the next frame's pass usually finds it ready.
static void end_render_pass(VkQueue queue, Replay *replay)
{
	const Framebuffer *const framebuffer = replay->framebuffer;
	if (replay->render_pass->store_op != VK_ATTACHMENT_STORE_OP_STORE) {
		queue->edram.known = false;
		return;
	}
	const bool clears = replay->render_pass->load_op == VK_ATTACHMENT_LOAD_OP_CLEAR;
	const uint32_t next_argb = clears ? replay->clear_argb : BLACK_ARGB;
	xsdk_gpu_resolve(framebuffer->attachment->address, framebuffer->width, framebuffer->height, next_argb);
	queue->edram = (Edram){ .known = true, .argb = next_argb };
}

static void draw(Replay *replay, uint32_t vertex_count, uint32_t first_vertex)
{
	const Pipeline *const pipeline = replay->pipeline;
	if (replay->pipeline_dirty) {
		apply_pipeline(pipeline);
		replay->pipeline_dirty = false;
	}
	// xsdk draws from vertex 0, so each stream starts at the draw's first vertex.
	for (uint32_t done = 0; done < vertex_count; done += MAX_DRAW_VERTICES) {
		const uint32_t first = first_vertex + done;
		for (uint32_t s = 0; s < pipeline->stream_count; s++) {
			const VertexBinding *const binding = &replay->bindings[pipeline->streams[s].binding];
			const uint32_t skipped = first * pipeline->streams[s].stride_bytes;
			xsdk_gpu_set_vertices(s, binding->address + skipped, binding->size_bytes - skipped);
		}
		const uint32_t left = vertex_count - done;
		xsdk_gpu_draw(XSDK_PRIMITIVE_TRIANGLE_LIST, left < MAX_DRAW_VERTICES ? left : MAX_DRAW_VERTICES);
	}
}

static void replay_commands(VkQueue queue, VkCommandBuffer buffer)
{
	Replay replay = { .pipeline_dirty = true };
	for (uint32_t i = 0; i < buffer->count; i++) {
		const Command *const command = &buffer->commands[i];
		switch (command->type) {
		case COMMAND_BEGIN_RENDER_PASS:
			replay.render_pass = command->begin.render_pass;
			replay.framebuffer = command->begin.framebuffer;
			replay.clear_argb = command->begin.clear_argb;
			begin_render_pass(queue, &replay);
			break;
		case COMMAND_END_RENDER_PASS:
			end_render_pass(queue, &replay);
			break;
		case COMMAND_BIND_PIPELINE:
			replay.pipeline = command->pipeline;
			replay.pipeline_dirty = true;
			break;
		case COMMAND_BIND_VERTEX_BUFFER:
			replay.bindings[command->vertex_buffer.binding] =
				(VertexBinding){ .address = command->vertex_buffer.address,
					.size_bytes = command->vertex_buffer.size_bytes };
			break;
		case COMMAND_DRAW:
			draw(&replay, command->draw.vertex_count, command->draw.first_vertex);
			break;
		}
	}
}

VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit(
	VkQueue queue, uint32_t submitCount, const VkSubmitInfo *pSubmits, VkFence fence)
{
	// Drain the CPU's write-combining, so the GPU reads what the app wrote to mapped memory.
	__asm__ volatile("sync" ::: "memory");
	for (uint32_t i = 0; i < submitCount; i++) {
		for (uint32_t j = 0; j < pSubmits[i].commandBufferCount; j++)
			replay_commands(queue, pSubmits[i].pCommandBuffers[j]);
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
