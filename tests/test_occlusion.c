#include <stdbool.h>
#include <stdint.h>

#include "harness.h"
#include "shaders.h"
#include "tests.h"
#include "xsdk/log.h"

// Milestone 28: occlusion queries on D3D's visibility query, nonzero if a query's draws rasterized anything, a draw
// hidden behind others too (docs/SUPPORT.md, Queries). Queries around quads, nothing, a quad off screen, one culled, a
// hidden one and clears; a query reused in later submissions, 80 in one, results copied and read with availability,
// one in a secondary command buffer, and the refusals. Xenia sets every query's bit at its end
// (xenia:src/xenia/gpu/command_processor.cc:1622-1630), so the checks of 0 fail there.

enum {
	FENCE_TIMEOUT_NS = 1000000000,
	// Past the 64 visibility ids (xsdk:include/xsdk/gpu.h).
	MANY_QUERIES = 80,
	QUERIES = MANY_QUERIES,
	// The copies: three queries of a 64-bit value and availability, then two 32-bit values and a word past them.
	WIDE_STRIDE_BYTES = 16,
	WIDE_QUERIES = 3,
	NARROW_AT = WIDE_QUERIES * WIDE_STRIDE_BYTES,
	NARROW_QUERIES = 2,
	RESULTS_BYTES = 64,
	UNTOUCHED = 0xAAAAAAAA,
};

// The scene's queries, by number.
enum {
	QUERY_QUAD,
	QUERY_NONE,
	QUERY_OFF_SCREEN,
	QUERY_CULLED,
	QUERY_HIDDEN,
	QUERY_CLEAR,
	QUERY_DRAW_THEN_CLEAR,
	QUERY_CLEAR_THEN_DRAW,
	SCENE_QUERIES,
};

typedef enum {
	// Depth tested less-than and written, nothing culled.
	PIPELINE_DEPTH,
	// The same, back faces culled, clockwise the front.
	PIPELINE_CULL,
	PIPELINES,
} PipelineKind;

#define RGBA(r, g, b) { (r) / 255.0f, (g) / 255.0f, (b) / 255.0f, 1.0f }

// The quad in front, the one behind it, one past the screen's right edge, a mirrored one, a back face to the culling
// pipeline, and two more in front elsewhere, around the clears.
static const HarnessQuad front = { 100, 100, 200, 150, 0.25f, RGBA(64, 224, 64), false };
static const HarnessQuad behind = { 100, 100, 200, 150, 0.75f, RGBA(224, 64, 64), false };
static const HarnessQuad off_screen = { 1400, 100, 200, 150, 0.25f, RGBA(224, 224, 64), false };
static const HarnessQuad back_face = { 400, 100, 200, 150, 0.25f, RGBA(64, 64, 224), true };
static const HarnessQuad before_clear = { 700, 100, 200, 150, 0.25f, RGBA(224, 64, 224), false };
static const HarnessQuad after_clear = { 1000, 100, 200, 150, 0.25f, RGBA(64, 224, 224), false };

typedef struct {
	VkCommandBuffer commands;
	VkCommandBuffer secondary;
	VkQueryPool pool;
	VkImage depth;
	VkDeviceMemory depth_memory;
	VkImageView depth_view;
	VkRenderPass render_pass;
	VkFramebuffer framebuffers[HARNESS_MAX_IMAGES];
	VkPipelineLayout layout;
	VkPipeline pipelines[PIPELINES];
	VkBuffer vertices;
	VkDeviceMemory vertex_memory;
	VkBuffer results;
	VkDeviceMemory results_memory;
	// Write-combined memory, so 32-bit volatile stores (CLAUDE.md).
	volatile uint32_t *results_words;
} Scene;

static bool begin(const Scene *scene)
{
	const VkCommandBufferBeginInfo info = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	return harness_ok("vkBeginCommandBuffer", vkBeginCommandBuffer(scene->commands, &info));
}

static bool end_and_submit(const Harness *harness, const Scene *scene)
{
	const VkSubmitInfo info = {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.commandBufferCount = 1,
		.pCommandBuffers = &scene->commands,
	};
	return harness_ok("vkEndCommandBuffer", vkEndCommandBuffer(scene->commands)) &&
		harness_ok("vkQueueSubmit", vkQueueSubmit(harness->queue, 1, &info, harness->fence));
}

// The fence is left unsignaled, as the tests after want it.
static bool finish(const Harness *harness)
{
	const bool done =
		harness_ok("vkWaitForFences", vkWaitForFences(harness->device, 1, &harness->fence, VK_TRUE, FENCE_TIMEOUT_NS));
	(void)vkResetFences(harness->device, 1, &harness->fence);
	return done;
}

static bool run(const Harness *harness, const Scene *scene)
{
	return end_and_submit(harness, scene) && finish(harness);
}

// The pass clears colour and depth to 1, the depth pipeline bound.
static void begin_pass(const Scene *scene, VkSubpassContents contents)
{
	const VkClearValue clears[2] = {
		{ .color = { .float32 = { 0.1f, 0.1f, 0.1f, 1.0f } } },
		{ .depthStencil = { 1.0f, 0 } },
	};
	const VkRenderPassBeginInfo pass = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		.renderPass = scene->render_pass,
		.framebuffer = scene->framebuffers[0],
		.renderArea = { { 0, 0 }, { HARNESS_WIDTH, HARNESS_HEIGHT } },
		.clearValueCount = 2,
		.pClearValues = clears,
	};
	vkCmdBeginRenderPass(scene->commands, &pass, contents);
	if (contents == VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS)
		return;
	const VkDeviceSize offset = 0;
	vkCmdBindPipeline(scene->commands, VK_PIPELINE_BIND_POINT_GRAPHICS, scene->pipelines[PIPELINE_DEPTH]);
	vkCmdBindVertexBuffers(scene->commands, 0, 1, &scene->vertices, &offset);
}

// A query around the quad, or around no draw.
static void around(const Scene *scene, uint32_t query, const HarnessQuad *quad)
{
	vkCmdBeginQuery(scene->commands, scene->pool, query, 0);
	if (quad)
		harness_push_quad(scene->commands, scene->layout, quad);
	vkCmdEndQuery(scene->commands, scene->pool, query);
}

static void clear_rect(const Scene *scene)
{
	const VkClearAttachment attachment = { VK_IMAGE_ASPECT_COLOR_BIT, 0,
		{ .color = { .float32 = { 1.0f, 1.0f, 1.0f, 1.0f } } } };
	const VkClearRect rect = { { { 600, 400 }, { 64, 64 } }, 0, 1 };
	vkCmdClearAttachments(scene->commands, 1, &attachment, 1, &rect);
}

static bool results(const Harness *harness, const Scene *scene, uint32_t first, uint32_t count, uint64_t *values)
{
	return harness_ok("vkGetQueryPoolResults",
		vkGetQueryPoolResults(harness->device, scene->pool, first, count, count * sizeof *values, values,
			sizeof *values, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
}

static void record_scene(const Scene *scene)
{
	vkCmdResetQueryPool(scene->commands, scene->pool, 0, QUERIES);
	begin_pass(scene, VK_SUBPASS_CONTENTS_INLINE);
	around(scene, QUERY_QUAD, &front);
	around(scene, QUERY_NONE, NULL);
	around(scene, QUERY_OFF_SCREEN, &off_screen);
	vkCmdBindPipeline(scene->commands, VK_PIPELINE_BIND_POINT_GRAPHICS, scene->pipelines[PIPELINE_CULL]);
	around(scene, QUERY_CULLED, &back_face);
	vkCmdBindPipeline(scene->commands, VK_PIPELINE_BIND_POINT_GRAPHICS, scene->pipelines[PIPELINE_DEPTH]);
	around(scene, QUERY_HIDDEN, &behind);
	vkCmdBeginQuery(scene->commands, scene->pool, QUERY_CLEAR, 0);
	clear_rect(scene);
	vkCmdEndQuery(scene->commands, scene->pool, QUERY_CLEAR);
	vkCmdBeginQuery(scene->commands, scene->pool, QUERY_DRAW_THEN_CLEAR, 0);
	harness_push_quad(scene->commands, scene->layout, &before_clear);
	clear_rect(scene);
	vkCmdEndQuery(scene->commands, scene->pool, QUERY_DRAW_THEN_CLEAR);
	vkCmdBeginQuery(scene->commands, scene->pool, QUERY_CLEAR_THEN_DRAW, 0);
	clear_rect(scene);
	harness_push_quad(scene->commands, scene->layout, &after_clear);
	vkCmdEndQuery(scene->commands, scene->pool, QUERY_CLEAR_THEN_DRAW);
	vkCmdEndRenderPass(scene->commands);
}

static void check_scene(const Harness *harness, const Scene *scene)
{
	bool ran = begin(scene);
	if (ran) {
		record_scene(scene);
		ran = run(harness, scene);
	}
	uint64_t values[SCENE_QUERIES] = { 0 };
	ran = ran && results(harness, scene, 0, SCENE_QUERIES, values);
	xsdk_log_write(
		"occlusion: quad %llu, none %llu, off screen %llu, culled %llu, hidden %llu, clear %llu, draw then "
		"clear %llu, clear then draw %llu",
		(unsigned long long)values[QUERY_QUAD], (unsigned long long)values[QUERY_NONE],
		(unsigned long long)values[QUERY_OFF_SCREEN], (unsigned long long)values[QUERY_CULLED],
		(unsigned long long)values[QUERY_HIDDEN], (unsigned long long)values[QUERY_CLEAR],
		(unsigned long long)values[QUERY_DRAW_THEN_CLEAR], (unsigned long long)values[QUERY_CLEAR_THEN_DRAW]);
	check("occlusion: a query around a quad is nonzero", ran && values[QUERY_QUAD]);
	check("occlusion: a query around no draw is 0", ran && !values[QUERY_NONE]);
	check("occlusion: a query around a quad off screen, or one culled, is 0",
		ran && !values[QUERY_OFF_SCREEN] && !values[QUERY_CULLED]);
	xsdk_log_write("occlusion: the hidden quad's query is %s; nonzero is the documented deviation",
		values[QUERY_HIDDEN] ? "nonzero" : "0");
	check("occlusion: a query around a quad hidden behind another is nonzero too, the documented deviation",
		ran && values[QUERY_HIDDEN]);
	check("occlusion: vkCmdClearAttachments inside a query doesn't count, and a draw either side of it does",
		ran && !values[QUERY_CLEAR] && values[QUERY_DRAW_THEN_CLEAR] && values[QUERY_CLEAR_THEN_DRAW]);
}

// Query 0, reset and reused in one submission after another: around no draw, a quad, then no draw again.
static uint64_t reused(const Harness *harness, const Scene *scene, const HarnessQuad *quad, bool *ran)
{
	*ran = *ran && begin(scene);
	if (!*ran)
		return 0;
	vkCmdResetQueryPool(scene->commands, scene->pool, QUERY_QUAD, 1);
	begin_pass(scene, VK_SUBPASS_CONTENTS_INLINE);
	around(scene, QUERY_QUAD, quad);
	vkCmdEndRenderPass(scene->commands);
	uint64_t value = 0;
	*ran = run(harness, scene) && results(harness, scene, QUERY_QUAD, 1, &value);
	return value;
}

static void check_reuse(const Harness *harness, const Scene *scene)
{
	bool ran = true;
	const uint64_t none = reused(harness, scene, NULL, &ran);
	const uint64_t quad = reused(harness, scene, &front, &ran);
	const uint64_t none_again = reused(harness, scene, NULL, &ran);
	xsdk_log_write("occlusion: query 0 reused: %llu, %llu, %llu", (unsigned long long)none, (unsigned long long)quad,
		(unsigned long long)none_again);
	check("occlusion: a query reused in later submissions is 0 around no draw and nonzero around a quad, each time",
		ran && !none && quad && !none_again);
}

// Even queries around quads each nearer than the last, so each shows, odd ones around no draw; read with WAIT before
// the fence is waited on, so the call itself waits for the submission.
static void check_many(const Harness *harness, const Scene *scene)
{
	bool ran = begin(scene);
	if (ran) {
		vkCmdResetQueryPool(scene->commands, scene->pool, 0, MANY_QUERIES);
		begin_pass(scene, VK_SUBPASS_CONTENTS_INLINE);
		for (uint32_t i = 0; i < MANY_QUERIES; i++) {
			HarnessQuad quad = front;
			quad.depth = 0.9f - 0.01f * (float)i;
			around(scene, i, i % 2 ? NULL : &quad);
		}
		vkCmdEndRenderPass(scene->commands);
		ran = end_and_submit(harness, scene);
	}
	uint32_t values[MANY_QUERIES] = { 0 };
	VkResult got = VK_ERROR_DEVICE_LOST;
	if (ran)
		got = vkGetQueryPoolResults(harness->device, scene->pool, 0, MANY_QUERIES, sizeof values, values,
			sizeof values[0], VK_QUERY_RESULT_WAIT_BIT);
	ran = ran && finish(harness);
	uint32_t wrong = 0;
	for (uint32_t i = 0; i < MANY_QUERIES; i++)
		wrong += (values[i] != 0) != (i % 2 == 0);
	xsdk_log_write("occlusion: of 80 queries in one submission, %u wrong; vkGetQueryPoolResults returned %d",
		(unsigned)wrong, (int)got);
	check("occlusion: 80 queries in one submission, quads nonzero and no draws 0, which a wait for results waits for",
		ran && got == VK_SUCCESS && !wrong);
}

static uint64_t result_at(const Scene *scene, uint32_t byte)
{
	return (uint64_t)scene->results_words[byte / 4] << 32 | scene->results_words[byte / 4 + 1];
}

// Query 0 around a quad, 1 around no draw and 2 left reset, copied in the same submission after the pass, then read.
static void check_copy(const Harness *harness, const Scene *scene)
{
	for (uint32_t i = 0; i < RESULTS_BYTES / 4; i++)
		scene->results_words[i] = UNTOUCHED;
	bool ran = begin(scene);
	if (ran) {
		vkCmdResetQueryPool(scene->commands, scene->pool, 0, WIDE_QUERIES);
		begin_pass(scene, VK_SUBPASS_CONTENTS_INLINE);
		around(scene, 0, &front);
		around(scene, 1, NULL);
		vkCmdEndRenderPass(scene->commands);
		vkCmdCopyQueryPoolResults(scene->commands, scene->pool, 0, WIDE_QUERIES, scene->results, 0, WIDE_STRIDE_BYTES,
			VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
		vkCmdCopyQueryPoolResults(scene->commands, scene->pool, 0, NARROW_QUERIES, scene->results, NARROW_AT, 4, 0);
		ran = run(harness, scene);
	}
	const bool wide = result_at(scene, 0) && result_at(scene, 8) == 1 && result_at(scene, 16) == 0 &&
		result_at(scene, 24) == 1 && scene->results_words[8] == UNTOUCHED && scene->results_words[9] == UNTOUCHED &&
		result_at(scene, 40) == 0;
	const uint32_t at = NARROW_AT / 4;
	const bool narrow =
		scene->results_words[at] && scene->results_words[at + 1] == 0 && scene->results_words[at + 2] == UNTOUCHED;
	check(
		"occlusion: vkCmdCopyQueryPoolResults copies results at 64 bits with availability and at 32, an unwritten "
		"query unavailable and its value untouched",
		ran && wide && narrow);
	uint32_t read[WIDE_QUERIES][2] = { { 0, 0 }, { 0, 0 }, { UNTOUCHED, UNTOUCHED } };
	const VkResult got = vkGetQueryPoolResults(harness->device, scene->pool, 0, WIDE_QUERIES, sizeof read, read,
		sizeof read[0], VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
	check(
		"occlusion: vkGetQueryPoolResults reads results with availability at 32 bits, an unwritten query unavailable "
		"and VK_NOT_READY",
		ran && got == VK_NOT_READY && read[0][0] && read[0][1] == 1 && read[1][0] == 0 && read[1][1] == 1 &&
			read[2][0] == UNTOUCHED && read[2][1] == 0);
}

// A secondary command buffer continuing the pass, its query around the quad.
static void check_secondary(const Harness *harness, const Scene *scene)
{
	const VkCommandBufferInheritanceInfo inheritance = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO,
		.renderPass = scene->render_pass,
		.framebuffer = scene->framebuffers[0],
	};
	const VkCommandBufferBeginInfo continues = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT,
		.pInheritanceInfo = &inheritance,
	};
	bool ran = harness_ok("vkBeginCommandBuffer", vkBeginCommandBuffer(scene->secondary, &continues));
	if (ran) {
		const VkDeviceSize offset = 0;
		vkCmdBindPipeline(scene->secondary, VK_PIPELINE_BIND_POINT_GRAPHICS, scene->pipelines[PIPELINE_DEPTH]);
		vkCmdBindVertexBuffers(scene->secondary, 0, 1, &scene->vertices, &offset);
		vkCmdBeginQuery(scene->secondary, scene->pool, QUERY_QUAD, 0);
		harness_push_quad(scene->secondary, scene->layout, &front);
		vkCmdEndQuery(scene->secondary, scene->pool, QUERY_QUAD);
		ran = harness_ok("vkEndCommandBuffer", vkEndCommandBuffer(scene->secondary)) && begin(scene);
	}
	if (ran) {
		vkCmdResetQueryPool(scene->commands, scene->pool, QUERY_QUAD, 1);
		begin_pass(scene, VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS);
		vkCmdExecuteCommands(scene->commands, 1, &scene->secondary);
		vkCmdEndRenderPass(scene->commands);
		ran = run(harness, scene);
	}
	uint64_t value = 0;
	ran = ran && results(harness, scene, QUERY_QUAD, 1, &value);
	check("occlusion: a query in a secondary command buffer continuing a render pass counts its quad", ran && value);
}

static void check_refusals(const Harness *harness, const Scene *scene)
{
	VkPhysicalDeviceFeatures features;
	vkGetPhysicalDeviceFeatures(harness->physical_device, &features);
	bool precise = !features.occlusionQueryPrecise && !features.inheritedQueries && begin(scene);
	if (precise) {
		begin_pass(scene, VK_SUBPASS_CONTENTS_INLINE);
		vkCmdBeginQuery(scene->commands, scene->pool, 0, VK_QUERY_CONTROL_PRECISE_BIT);
		vkCmdEndQuery(scene->commands, scene->pool, 0);
		vkCmdEndRenderPass(scene->commands);
		precise = vkEndCommandBuffer(scene->commands) == VK_ERROR_FEATURE_NOT_PRESENT;
	}
	check("occlusion: occlusionQueryPrecise and inheritedQueries aren't reported, and a precise query is refused",
		precise);
	bool outside = begin(scene);
	if (outside) {
		vkCmdBeginQuery(scene->commands, scene->pool, 0, 0);
		vkCmdEndQuery(scene->commands, scene->pool, 0);
		outside = vkEndCommandBuffer(scene->commands) == VK_ERROR_FEATURE_NOT_PRESENT;
	}
	check("occlusion: a query begun outside a render pass is refused", outside);
}

static VkResult create_pipeline(const Harness *harness, Scene *scene, PipelineKind kind)
{
	const VkPipelineRasterizationStateCreateInfo raster = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
		.polygonMode = VK_POLYGON_MODE_FILL,
		.cullMode = kind == PIPELINE_CULL ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE,
		.frontFace = VK_FRONT_FACE_CLOCKWISE,
		.lineWidth = 1.0f,
	};
	const VkPipelineDepthStencilStateCreateInfo depth_stencil = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
		.depthTestEnable = VK_TRUE,
		.depthWriteEnable = VK_TRUE,
		.depthCompareOp = VK_COMPARE_OP_LESS,
	};
	const HarnessPipeline spec = {
		.vertex_spirv = push_vert_spirv,
		.vertex_bytes = PUSH_VERT_SPIRV_BYTES,
		.fragment_spirv = push_frag_spirv,
		.fragment_bytes = PUSH_FRAG_SPIRV_BYTES,
		.bindings = &harness_quad_binding,
		.binding_count = 1,
		.attributes = &harness_quad_attribute,
		.attribute_count = 1,
		.layout = scene->layout,
		.raster = &raster,
		.depth_stencil = &depth_stencil,
	};
	return harness_pipeline(harness, &spec, scene->render_pass, &scene->pipelines[kind]);
}

static bool build(const Harness *harness, Scene *scene)
{
	const VkCommandBufferAllocateInfo primary = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = harness->pool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1,
	};
	VkCommandBufferAllocateInfo secondary = primary;
	secondary.level = VK_COMMAND_BUFFER_LEVEL_SECONDARY;
	const VkQueryPoolCreateInfo pool = {
		.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
		.queryType = VK_QUERY_TYPE_OCCLUSION,
		.queryCount = QUERIES,
	};
	bool ok =
		harness_ok("vkAllocateCommandBuffers", vkAllocateCommandBuffers(harness->device, &primary, &scene->commands)) &&
		harness_ok(
			"vkAllocateCommandBuffers", vkAllocateCommandBuffers(harness->device, &secondary, &scene->secondary)) &&
		harness_ok("vkCreateQueryPool", vkCreateQueryPool(harness->device, &pool, NULL, &scene->pool)) &&
		harness_ok("push layout", harness_push_layout(harness, &scene->layout)) &&
		harness_ok(
			"depth image", harness_depth_image(harness, &scene->depth, &scene->depth_memory, &scene->depth_view)) &&
		harness_ok("render pass",
			harness_depth_render_pass(harness, scene->depth_view, &scene->render_pass, scene->framebuffers));
	for (uint32_t i = 0; i < PIPELINES && ok; i++)
		ok = harness_ok("pipeline", create_pipeline(harness, scene, (PipelineKind)i));
	const uint32_t zeros[RESULTS_BYTES / 4] = { 0 };
	void *mapped = NULL;
	ok = ok && harness_ok("vertex buffer", harness_quad_buffer(harness, &scene->vertices, &scene->vertex_memory)) &&
		harness_ok("results",
			harness_buffer(harness, VK_BUFFER_USAGE_TRANSFER_DST_BIT, zeros, RESULTS_BYTES, &scene->results,
				&scene->results_memory)) &&
		harness_ok("vkMapMemory", vkMapMemory(harness->device, scene->results_memory, 0, VK_WHOLE_SIZE, 0, &mapped));
	scene->results_words = mapped;
	return ok;
}

static void destroy(const Harness *harness, Scene *scene)
{
	const VkDevice device = harness->device;
	if (scene->results_memory)
		vkUnmapMemory(device, scene->results_memory);
	vkDestroyBuffer(device, scene->results, NULL);
	vkFreeMemory(device, scene->results_memory, NULL);
	vkDestroyBuffer(device, scene->vertices, NULL);
	vkFreeMemory(device, scene->vertex_memory, NULL);
	for (uint32_t i = 0; i < PIPELINES; i++)
		vkDestroyPipeline(device, scene->pipelines[i], NULL);
	vkDestroyPipelineLayout(device, scene->layout, NULL);
	if (scene->render_pass)
		harness_destroy_render_pass(harness, scene->render_pass, scene->framebuffers);
	vkDestroyImageView(device, scene->depth_view, NULL);
	vkDestroyImage(device, scene->depth, NULL);
	vkFreeMemory(device, scene->depth_memory, NULL);
	vkDestroyQueryPool(device, scene->pool, NULL);
	if (scene->secondary)
		vkFreeCommandBuffers(device, harness->pool, 1, &scene->secondary);
	if (scene->commands)
		vkFreeCommandBuffers(device, harness->pool, 1, &scene->commands);
}

void test_occlusion(const Harness *harness)
{
	Scene scene = { .commands = VK_NULL_HANDLE };
	const bool built = build(harness, &scene) && harness->swapchain;
	check("occlusion: the query pool, the pass with depth, its two pipelines and the results buffer made", built);
	if (built) {
		check_scene(harness, &scene);
		check_reuse(harness, &scene);
		check_many(harness, &scene);
		check_copy(harness, &scene);
		check_secondary(harness, &scene);
		check_refusals(harness, &scene);
	}
	destroy(harness, &scene);
}
