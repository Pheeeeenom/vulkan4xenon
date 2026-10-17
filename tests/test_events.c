#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "harness.h"
#include "shaders.h"
#include "tests.h"
#include "xsdk/gpu.h"
#include "xsdk/log.h"

// Milestone 19's step 6: events the GPU sets, resets and waits on, and time stamps of the GPU's clock.

enum {
	FENCE_TIMEOUT_NS = 1000000000,
	// How long the test lets the GPU wait on an event the host hasn't set.
	HELD_MS = 20,
	QUERIES = 8,
	// Two time stamps this far apart by the CPU's clock, which their difference must match within a percent.
	APART_MS = 50,
	APART_TOLERANCE_PERCENT = 1,
	// The GPU's 498.75 clocks a microsecond (xsdk:docs/HARDWARE.md, "Queries and events"), a period of 2.005 ns, in
	// picoseconds, within a quarter of a percent.
	PERIOD_MIN_PICOSECONDS = 2000,
	PERIOD_MAX_PICOSECONDS = 2010,
	// A quad over the screen, 921600 pixels at about 3424 million a second (xsdk:docs/HARDWARE.md), some 270 us.
	DRAW_MIN_CLOCKS = 100 * 498,
	// Results copied out: four queries of a 64-bit value and availability, one 32-bit value, and an unwritten query's.
	COPY_STRIDE_BYTES = 16,
	COPY_QUERIES = 4,
	NARROW_AT = 64,
	UNWRITTEN_AT = 80,
	UNWRITTEN_QUERY = 5,
	RESULTS_BYTES = 128,
	UNTOUCHED = 0xAAAAAAAA,
};

static const char *const step_6_commands[] = {
	"vkCmdSetEvent",
	"vkCmdResetEvent",
	"vkCmdWaitEvents",
	"vkCreateQueryPool",
	"vkDestroyQueryPool",
	"vkGetQueryPoolResults",
	"vkCmdResetQueryPool",
	"vkCmdBeginQuery",
	"vkCmdEndQuery",
	"vkCmdWriteTimestamp",
	"vkCmdCopyQueryPoolResults",
};

enum {
	EVENT_FIRST,
	EVENT_SECOND,
	EVENT_HOST,
	EVENT_AFTER,
	EVENTS,
};

typedef struct {
	VkCommandBuffer commands;
	VkEvent events[EVENTS];
	VkQueryPool pool;
	VkRenderPass render_pass;
	VkFramebuffer framebuffers[HARNESS_MAX_IMAGES];
	VkPipelineLayout layout;
	VkPipeline pipeline;
	VkBuffer vertices;
	VkDeviceMemory vertex_memory;
	VkBuffer results;
	VkDeviceMemory results_memory;
	// Write-combined memory, so 32-bit volatile stores (CLAUDE.md).
	volatile uint32_t *results_words;
} Scene;

static uint64_t now_us(void)
{
	struct timespec now;
	(void)clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * 1000000 + (uint64_t)now.tv_nsec / 1000;
}

static void sleep_ms(uint32_t ms)
{
	const struct timespec request = { .tv_sec = 0, .tv_nsec = (long)ms * 1000000 };
	(void)nanosleep(&request, NULL);
}

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

// The tests after submit with the fence and wait on it, so it's left unsignaled, as they leave it.
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

static bool status(const Harness *harness, const Scene *scene, uint32_t event, VkResult want)
{
	return vkGetEventStatus(harness->device, scene->events[event]) == want;
}

static void check_names(const Harness *harness)
{
	bool found = true;
	for (uint32_t i = 0; i < sizeof step_6_commands / sizeof step_6_commands[0]; i++)
		found = found && vkGetDeviceProcAddr(harness->device, step_6_commands[i]);
	check("events: step 6's 11 commands are found by name", found);
}

static void check_set_and_reset(const Harness *harness, const Scene *scene)
{
	const bool unset = status(harness, scene, EVENT_FIRST, VK_EVENT_RESET);
	bool ran = unset && begin(scene);
	if (ran) {
		vkCmdSetEvent(scene->commands, scene->events[EVENT_FIRST], VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
		vkCmdSetEvent(scene->commands, scene->events[EVENT_SECOND], VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
		vkCmdResetEvent(scene->commands, scene->events[EVENT_SECOND], VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
		ran = run(harness, scene);
	}
	check("events: vkCmdSetEvent and vkCmdResetEvent set and reset events as the GPU reaches them",
		ran && status(harness, scene, EVENT_FIRST, VK_EVENT_SET) &&
			status(harness, scene, EVENT_SECOND, VK_EVENT_RESET));
}

// The GPU passes a wait on the event it set, then holds at one the host sets only after the submit.
static void check_wait(const Harness *harness, const Scene *scene)
{
	bool ran = begin(scene);
	if (ran) {
		vkCmdWaitEvents(scene->commands, 1, &scene->events[EVENT_FIRST], VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
			VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, NULL, 0, NULL, 0, NULL);
		vkCmdWaitEvents(scene->commands, 1, &scene->events[EVENT_HOST], VK_PIPELINE_STAGE_HOST_BIT,
			VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, NULL, 0, NULL, 0, NULL);
		vkCmdSetEvent(scene->commands, scene->events[EVENT_AFTER], VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
		ran = end_and_submit(harness, scene);
	}
	sleep_ms(HELD_MS);
	const bool held = ran && vkGetFenceStatus(harness->device, harness->fence) == VK_NOT_READY &&
		status(harness, scene, EVENT_AFTER, VK_EVENT_RESET);
	(void)vkSetEvent(harness->device, scene->events[EVENT_HOST]);
	const bool passed = ran && finish(harness) && status(harness, scene, EVENT_AFTER, VK_EVENT_SET);
	xsdk_log_write("events: the wait held the GPU %s; once the host set the event, it passed %s", held ? "yes" : "no",
		passed ? "yes" : "no");
	check("events: vkCmdWaitEvents passes on an event the GPU set and holds the GPU till the host sets one",
		held && passed);
}

static void check_device(const Harness *harness)
{
	VkPhysicalDeviceProperties properties;
	vkGetPhysicalDeviceProperties(harness->physical_device, &properties);
	uint32_t count = 1;
	VkQueueFamilyProperties family;
	vkGetPhysicalDeviceQueueFamilyProperties(harness->physical_device, &count, &family);
	const float picoseconds = properties.limits.timestampPeriod * 1000.0f;
	check("time stamps: 64 valid bits, a period of 2.005 ns, and graphics queues time stamp",
		family.timestampValidBits == 64 && picoseconds >= PERIOD_MIN_PICOSECONDS &&
			picoseconds <= PERIOD_MAX_PICOSECONDS && properties.limits.timestampComputeAndGraphics);
}

// Occlusion pools are made since milestone 28 (test_occlusion.c).
static void check_refusals(const Harness *harness)
{
	const VkQueryPoolCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
		.queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS,
		.queryCount = 1,
	};
	VkQueryPool pool = VK_NULL_HANDLE;
	check("time stamps: pipeline statistics query pools are refused",
		vkCreateQueryPool(harness->device, &info, NULL, &pool) == VK_ERROR_FEATURE_NOT_PRESENT);
}

// Whether every query of a range reads unavailable, each availability 0 and the call VK_NOT_READY.
static bool unavailable(const Harness *harness, const Scene *scene, uint32_t first, uint32_t count)
{
	uint64_t results[QUERIES][2];
	memset(results, 0xFF, sizeof results);
	const VkResult result = vkGetQueryPoolResults(harness->device, scene->pool, first, count, sizeof results, results,
		sizeof results[0], VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
	bool none = result == VK_NOT_READY;
	for (uint32_t i = 0; i < count; i++)
		none = none && results[i][1] == 0;
	return none;
}

static bool timestamps(const Harness *harness, const Scene *scene, uint32_t first, uint32_t count, uint64_t *values)
{
	return harness_ok("vkGetQueryPoolResults",
		vkGetQueryPoolResults(harness->device, scene->pool, first, count, count * sizeof *values, values,
			sizeof *values, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
}

// Two time stamps a CPU-timed sleep apart, between the CPU's reads of the GPU's clock: each holds the clock's whole
// 64 bits, and their difference by the period is the sleep's.
static void check_apart(const Harness *harness, const Scene *scene, bool *made_unavailable)
{
	*made_unavailable = unavailable(harness, scene, 0, QUERIES);
	const uint64_t clock_before = xsdk_gpu_clock();
	const uint64_t start = now_us();
	bool ran = begin(scene);
	if (ran) {
		vkCmdResetQueryPool(scene->commands, scene->pool, 0, QUERIES);
		vkCmdWriteTimestamp(scene->commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, scene->pool, 0);
		ran = run(harness, scene);
	}
	sleep_ms(APART_MS);
	const uint64_t end = now_us();
	if (ran && begin(scene)) {
		vkCmdWriteTimestamp(scene->commands, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, scene->pool, 1);
		ran = run(harness, scene);
	}
	const uint64_t clock_after = xsdk_gpu_clock();
	uint64_t values[2] = { 0 };
	ran = ran && timestamps(harness, scene, 0, 2, values);
	VkPhysicalDeviceProperties properties;
	vkGetPhysicalDeviceProperties(harness->physical_device, &properties);
	const double apart_us = (double)(values[1] - values[0]) * properties.limits.timestampPeriod / 1000.0;
	const double cpu_us = (double)(end - start);
	xsdk_log_write(
		"time stamps: %llu and %llu, the clock %llu before and %llu after, high word %u; %.1f us apart, %.1f "
		"by the CPU",
		(unsigned long long)values[0], (unsigned long long)values[1], (unsigned long long)clock_before,
		(unsigned long long)clock_after, (unsigned)(values[0] >> 32), apart_us, cpu_us);
	check("time stamps: each holds the GPU's 64-bit clock",
		ran && clock_before <= values[0] && values[0] < values[1] && values[1] <= clock_after);
	const double off = apart_us > cpu_us ? apart_us - cpu_us : cpu_us - apart_us;
	check("time stamps: two a CPU-timed 50 ms apart differ by it, within a percent",
		ran && off * 100.0 <= cpu_us * APART_TOLERANCE_PERCENT);
}

// A quad over the screen between a time stamp at the top of the pipe and two after it, at the top and the bottom: only
// the bottom one waits for the draw. A pass's end won't do, as the command processor waits for its resolve itself.
static void check_after_work(const Harness *harness, const Scene *scene)
{
	bool ran = begin(scene);
	if (ran) {
		const VkClearValue clear = { .color = { .float32 = { 0.1f, 0.1f, 0.1f, 1.0f } } };
		const VkRenderPassBeginInfo pass = {
			.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
			.renderPass = scene->render_pass,
			.framebuffer = scene->framebuffers[0],
			.renderArea = { { 0, 0 }, { HARNESS_WIDTH, HARNESS_HEIGHT } },
			.clearValueCount = 1,
			.pClearValues = &clear,
		};
		const HarnessQuad quad = { 0.0f, 0.0f, HARNESS_WIDTH, HARNESS_HEIGHT, 0.5f, { 0.2f, 0.4f, 0.6f, 1.0f }, false };
		const VkDeviceSize offset = 0;
		vkCmdBeginRenderPass(scene->commands, &pass, VK_SUBPASS_CONTENTS_INLINE);
		vkCmdBindPipeline(scene->commands, VK_PIPELINE_BIND_POINT_GRAPHICS, scene->pipeline);
		vkCmdBindVertexBuffers(scene->commands, 0, 1, &scene->vertices, &offset);
		vkCmdWriteTimestamp(scene->commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, scene->pool, 2);
		harness_push_quad(scene->commands, scene->layout, &quad);
		vkCmdWriteTimestamp(scene->commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, scene->pool, 3);
		vkCmdWriteTimestamp(scene->commands, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, scene->pool, 4);
		vkCmdEndRenderPass(scene->commands);
		ran = run(harness, scene);
	}
	uint64_t values[3] = { 0 };
	ran = ran && timestamps(harness, scene, 2, 3, values);
	const uint64_t top = values[1] - values[0];
	const uint64_t bottom = values[2] - values[0];
	xsdk_log_write("time stamps: after the draw, %llu clocks at the top of the pipe and %llu at the bottom",
		(unsigned long long)top, (unsigned long long)bottom);
	check("time stamps: one at the bottom of the pipe waits for a draw, one at the top doesn't",
		ran && bottom >= DRAW_MIN_CLOCKS && top < bottom / 2);
}

static uint64_t result_at(const Scene *scene, uint32_t byte)
{
	return (uint64_t)scene->results_words[byte / 4] << 32 | scene->results_words[byte / 4 + 1];
}

// Queries 0 to 3 with their availability at 64 bits, query 1 alone at 32, and an unwritten query, whose value stays.
static void check_copy(const Harness *harness, const Scene *scene)
{
	for (uint32_t i = 0; i < RESULTS_BYTES / 4; i++)
		scene->results_words[i] = UNTOUCHED;
	bool ran = begin(scene);
	if (ran) {
		const VkQueryResultFlags wide = VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT;
		vkCmdCopyQueryPoolResults(
			scene->commands, scene->pool, 0, COPY_QUERIES, scene->results, 0, COPY_STRIDE_BYTES, wide);
		vkCmdCopyQueryPoolResults(scene->commands, scene->pool, 1, 1, scene->results, NARROW_AT, 4, 0);
		vkCmdCopyQueryPoolResults(
			scene->commands, scene->pool, UNWRITTEN_QUERY, 1, scene->results, UNWRITTEN_AT, COPY_STRIDE_BYTES, wide);
		ran = run(harness, scene);
	}
	uint64_t values[COPY_QUERIES] = { 0 };
	ran = ran && timestamps(harness, scene, 0, COPY_QUERIES, values);
	bool same = ran;
	for (uint32_t i = 0; i < COPY_QUERIES; i++)
		same = same && result_at(scene, i * COPY_STRIDE_BYTES) == values[i] &&
			result_at(scene, i * COPY_STRIDE_BYTES + 8) == 1;
	const bool narrow = scene->results_words[NARROW_AT / 4] == (uint32_t)values[1] &&
		scene->results_words[NARROW_AT / 4 + 1] == UNTOUCHED;
	const bool unwritten = scene->results_words[UNWRITTEN_AT / 4] == UNTOUCHED &&
		scene->results_words[UNWRITTEN_AT / 4 + 1] == UNTOUCHED && result_at(scene, UNWRITTEN_AT + 8) == 0;
	check("time stamps: vkCmdCopyQueryPoolResults copies them, at 64 bits with availability and at 32",
		same && narrow && unwritten);
}

static void check_reset(const Harness *harness, const Scene *scene, bool made_unavailable)
{
	bool ran = begin(scene);
	if (ran) {
		vkCmdResetQueryPool(scene->commands, scene->pool, 0, 2);
		ran = run(harness, scene);
	}
	uint64_t values[1] = { 0 };
	const bool others_kept = ran && timestamps(harness, scene, 2, 1, values);
	check("time stamps: a pool's queries are unavailable when it's made, and again once reset",
		made_unavailable && ran && unavailable(harness, scene, 0, 2) && others_kept);
}

static bool build(const Harness *harness, Scene *scene)
{
	const VkCommandBufferAllocateInfo allocate = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = harness->pool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1,
	};
	if (!harness_ok("vkAllocateCommandBuffers", vkAllocateCommandBuffers(harness->device, &allocate, &scene->commands)))
		return false;
	const VkEventCreateInfo event = { .sType = VK_STRUCTURE_TYPE_EVENT_CREATE_INFO };
	for (uint32_t i = 0; i < EVENTS; i++) {
		if (!harness_ok("vkCreateEvent", vkCreateEvent(harness->device, &event, NULL, &scene->events[i])))
			return false;
	}
	const VkQueryPoolCreateInfo pool = {
		.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
		.queryType = VK_QUERY_TYPE_TIMESTAMP,
		.queryCount = QUERIES,
	};
	if (!harness_ok("vkCreateQueryPool", vkCreateQueryPool(harness->device, &pool, NULL, &scene->pool)) ||
		!harness_ok("render pass", harness_render_pass(harness, &scene->render_pass, scene->framebuffers)) ||
		!harness_ok("push layout", harness_push_layout(harness, &scene->layout)))
		return false;
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
	};
	if (!harness_ok("pipeline", harness_pipeline(harness, &spec, scene->render_pass, &scene->pipeline)) ||
		!harness_ok("vertex buffer", harness_quad_buffer(harness, &scene->vertices, &scene->vertex_memory)))
		return false;
	const uint32_t zeros[RESULTS_BYTES / 4] = { 0 };
	void *mapped = NULL;
	if (!harness_ok("results",
			harness_buffer(harness, VK_BUFFER_USAGE_TRANSFER_DST_BIT, zeros, RESULTS_BYTES, &scene->results,
				&scene->results_memory)) ||
		!harness_ok("vkMapMemory", vkMapMemory(harness->device, scene->results_memory, 0, VK_WHOLE_SIZE, 0, &mapped)))
		return false;
	scene->results_words = mapped;
	return true;
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
	vkDestroyPipeline(device, scene->pipeline, NULL);
	vkDestroyPipelineLayout(device, scene->layout, NULL);
	if (scene->render_pass)
		harness_destroy_render_pass(harness, scene->render_pass, scene->framebuffers);
	vkDestroyQueryPool(device, scene->pool, NULL);
	for (uint32_t i = 0; i < EVENTS; i++)
		vkDestroyEvent(device, scene->events[i], NULL);
	if (scene->commands)
		vkFreeCommandBuffers(device, harness->pool, 1, &scene->commands);
}

void test_events(const Harness *harness)
{
	check_names(harness);
	check_device(harness);
	check_refusals(harness);
	Scene scene = { .commands = VK_NULL_HANDLE };
	const bool built = build(harness, &scene);
	check("events: the events, the time stamp pool, the pass, its pipeline and the results buffer made", built);
	if (built) {
		check_set_and_reset(harness, &scene);
		check_wait(harness, &scene);
		bool made_unavailable = false;
		check_apart(harness, &scene, &made_unavailable);
		check_after_work(harness, &scene);
		check_copy(harness, &scene);
		check_reset(harness, &scene, made_unavailable);
	}
	destroy(harness, &scene);
}
