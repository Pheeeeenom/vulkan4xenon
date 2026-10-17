#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "harness.h"
#include "shaders.h"
#include "tests.h"
#include "xsdk/gpu.h"

// Milestone 26's step 2: clip distances written as dot(gl_Position, a plane) cut quads at the planes' lines, the
// planes pushed or in a dynamic uniform buffer, read at each draw; a pipeline without them, and vkCmdClearAttachments's
// rectangle, are cut by none.

enum {
	FRAMES = 30,
	TOLERANCE = 2,
	// A plane in clip.vert's push constants, after push.frag's colour.
	PLANE_OFFSET_BYTES = 48,
	PLANE_BYTES = 16,
	// Each draw's planes sit 256 bytes on, minUniformBufferOffsetAlignment: two planes of 16 bytes.
	PLANES_STRIDE_BYTES = 256,
	PLANES_BYTES = 32,
	TOP_Y = 80,
	BOTTOM_Y = 300,
	CELL_HEIGHT = 160,
	// The lines the planes cut at: from them on, or before them.
	PUSH_COLUMN = 190,
	PUSH_ROW = TOP_Y + 80,
	UNIFORM_COLUMN = 870,
	UNIFORM_ROW = TOP_Y + 80,
	SECOND_COLUMN = 1150,
	AFTER_CLEAR_COLUMN = 870,
	// Past the screen: a plane keeping columns from here cuts every pixel.
	OFF_SCREEN_COLUMN = 4000,
};

typedef enum {
	PIPELINE_PUSH_PLANE,
	PIPELINE_UNIFORM_PLANES,
	PIPELINE_PLAIN,
	PIPELINES,
} PipelineKind;

static const VkClearValue clear = { .color = { .float32 = { 20.0f / 255, 20.0f / 255, 28.0f / 255, 1.0f } } };

#define RGBA(r, g, b) { (r) / 255.0f, (g) / 255.0f, (b) / 255.0f, 1.0f }

static const float red[4] = RGBA(224, 64, 64);
static const float blue[4] = RGBA(64, 64, 224);
static const float yellow[4] = RGBA(224, 224, 64);
static const float magenta[4] = RGBA(224, 64, 224);
static const float cyan[4] = RGBA(64, 224, 224);
static const float green[4] = RGBA(64, 224, 64);
static const float orange[4] = RGBA(240, 128, 0);

// Planes in clip space, the quads' w 1: Vulkan's x is the column / 640 - 1, its y the row / 360 - 1, y down. A pixel
// is kept where dot(its position, the plane) >= 0, at its centre, half a pixel on.
static void keep_columns_from(float plane[4], uint32_t column)
{
	const float line[4] = { 1.0f, 0.0f, 0.0f, 1.0f - column / (HARNESS_WIDTH / 2.0f) };
	memcpy(plane, line, sizeof line);
}

static void keep_rows_from(float plane[4], uint32_t row)
{
	const float line[4] = { 0.0f, 1.0f, 0.0f, 1.0f - row / (HARNESS_HEIGHT / 2.0f) };
	memcpy(plane, line, sizeof line);
}

static void keep_columns_before(float plane[4], uint32_t column)
{
	const float line[4] = { -1.0f, 0.0f, 0.0f, column / (HARNESS_WIDTH / 2.0f) - 1.0f };
	memcpy(plane, line, sizeof line);
}

static void keep_rows_before(float plane[4], uint32_t row)
{
	const float line[4] = { 0.0f, -1.0f, 0.0f, row / (HARNESS_HEIGHT / 2.0f) - 1.0f };
	memcpy(plane, line, sizeof line);
}

static const HarnessProbe push_probes[] = {
	{ "a pushed plane: the quad's left cut", 100, TOP_Y + 80, { 20, 20, 28 }, 0 },
	{ "a pushed plane: just left of its line", PUSH_COLUMN - 4, TOP_Y + 80, { 20, 20, 28 }, 0 },
	{ "a pushed plane: just right of its line", PUSH_COLUMN + 4, TOP_Y + 80, { 224, 64, 64 }, 0 },
	{ "a pushed plane: the quad's right kept", 300, TOP_Y + 80, { 224, 64, 64 }, 0 },
	{ "the next draw's pushed plane: the quad's top cut", 530, TOP_Y + 40, { 20, 20, 28 }, 0 },
	{ "the next draw's pushed plane: just above its line", 530, PUSH_ROW - 4, { 20, 20, 28 }, 0 },
	{ "the next draw's pushed plane: just below its line", 530, PUSH_ROW + 4, { 64, 64, 224 }, 0 },
	{ "the next draw's pushed plane: the quad's bottom kept", 530, TOP_Y + 140, { 64, 64, 224 }, 0 },
};

static const HarnessProbe uniform_probes[] = {
	{ "uniform planes at offset 0: the top left cut", 800, TOP_Y + 40, { 20, 20, 28 }, 0 },
	{ "uniform planes at offset 0: the top right cut", 950, TOP_Y + 40, { 20, 20, 28 }, 0 },
	{ "uniform planes at offset 0: the bottom left cut", 800, TOP_Y + 120, { 20, 20, 28 }, 0 },
	{ "uniform planes at offset 0: the bottom right kept", 950, TOP_Y + 120, { 224, 224, 64 }, 0 },
	{ "uniform planes at offset 256: the top left kept", 1100, TOP_Y + 40, { 224, 64, 224 }, 0 },
	{ "uniform planes at offset 256: the top right cut", 1200, TOP_Y + 40, { 20, 20, 28 }, 0 },
	{ "uniform planes at offset 256: the bottom left cut", 1100, TOP_Y + 120, { 20, 20, 28 }, 0 },
};

static const HarnessProbe after_probes[] = {
	{ "a pipeline without clip distances: its top left", 60, BOTTOM_Y + 20, { 64, 224, 224 }, 0 },
	{ "a pipeline without clip distances: its bottom right", 320, BOTTOM_Y + 140, { 64, 224, 224 }, 0 },
	{ "a clear's rectangle under a cutting pipeline: its top left", 400, BOTTOM_Y + 20, { 64, 224, 64 }, 0 },
	{ "a clear's rectangle under a cutting pipeline: its bottom right", 660, BOTTOM_Y + 140, { 64, 224, 64 }, 0 },
	{ "the draw after the clear: its plane's left cut", AFTER_CLEAR_COLUMN - 70, BOTTOM_Y + 80, { 20, 20, 28 }, 0 },
	{ "the draw after the clear: its plane's right kept", AFTER_CLEAR_COLUMN + 80, BOTTOM_Y + 80, { 240, 128, 0 }, 0 },
};

enum {
	GROUP_PUSH,
	GROUP_UNIFORM,
	GROUP_AFTER,
	GROUPS,
};

typedef struct {
	VkRenderPass render_pass;
	VkFramebuffer framebuffers[HARNESS_MAX_IMAGES];
	VkDescriptorSetLayout set_layout;
	VkPipelineLayout layout;
	VkPipeline pipelines[PIPELINES];
	VkBuffer vertices;
	VkDeviceMemory vertex_memory;
	VkBuffer planes;
	VkDeviceMemory plane_memory;
	VkDescriptorPool pool;
	VkDescriptorSet set;
	VkCommandBuffer commands[HARNESS_MAX_IMAGES];
	HarnessProbes checks[GROUPS];
	uint32_t misses[GROUPS];
} Scene;

// push.vert's ranges, a plane after the fragment range, and the planes' dynamic uniform buffer.
static VkResult create_layout(const Harness *harness, Scene *scene)
{
	const VkDescriptorSetLayoutBinding binding = { 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1,
		VK_SHADER_STAGE_VERTEX_BIT, NULL };
	const VkDescriptorSetLayoutCreateInfo set_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = 1,
		.pBindings = &binding,
	};
	VkResult result = vkCreateDescriptorSetLayout(harness->device, &set_info, NULL, &scene->set_layout);
	const VkPushConstantRange ranges[] = {
		{ VK_SHADER_STAGE_VERTEX_BIT, 0, 32 },
		{ VK_SHADER_STAGE_FRAGMENT_BIT, 32, 16 },
		{ VK_SHADER_STAGE_VERTEX_BIT, PLANE_OFFSET_BYTES, PLANE_BYTES },
	};
	const VkPipelineLayoutCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = 1,
		.pSetLayouts = &scene->set_layout,
		.pushConstantRangeCount = sizeof ranges / sizeof ranges[0],
		.pPushConstantRanges = ranges,
	};
	if (result == VK_SUCCESS)
		result = vkCreatePipelineLayout(harness->device, &info, NULL, &scene->layout);
	return result;
}

// Two draws' planes: from offset 0, keeping the bottom right of the third quad; from 256, the top left of the fourth.
static VkResult create_planes(const Harness *harness, Scene *scene)
{
	float planes[PLANES_STRIDE_BYTES * 2 / sizeof(float)] = { 0 };
	keep_columns_from(&planes[0], UNIFORM_COLUMN);
	keep_rows_from(&planes[4], UNIFORM_ROW);
	keep_columns_before(&planes[PLANES_STRIDE_BYTES / sizeof(float)], SECOND_COLUMN);
	keep_rows_before(&planes[PLANES_STRIDE_BYTES / sizeof(float) + 4], UNIFORM_ROW);
	VkResult result = harness_buffer(
		harness, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, planes, sizeof planes, &scene->planes, &scene->plane_memory);
	const VkDescriptorPoolSize size = { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1 };
	const VkDescriptorPoolCreateInfo pool_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.maxSets = 1,
		.poolSizeCount = 1,
		.pPoolSizes = &size,
	};
	if (result == VK_SUCCESS)
		result = vkCreateDescriptorPool(harness->device, &pool_info, NULL, &scene->pool);
	const VkDescriptorSetAllocateInfo allocate_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool = scene->pool,
		.descriptorSetCount = 1,
		.pSetLayouts = &scene->set_layout,
	};
	if (result == VK_SUCCESS)
		result = vkAllocateDescriptorSets(harness->device, &allocate_info, &scene->set);
	if (result != VK_SUCCESS)
		return result;
	const VkDescriptorBufferInfo buffer = { scene->planes, 0, PLANES_BYTES };
	const VkWriteDescriptorSet write = {
		.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		.dstSet = scene->set,
		.descriptorCount = 1,
		.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
		.pBufferInfo = &buffer,
	};
	vkUpdateDescriptorSets(harness->device, 1, &write, 0, NULL);
	return VK_SUCCESS;
}

static VkResult create_pipeline(const Harness *harness, Scene *scene, PipelineKind kind)
{
	const uint32_t *const vertex = kind == PIPELINE_PUSH_PLANE ? clip_vert_spirv
		: kind == PIPELINE_UNIFORM_PLANES                      ? clip_planes_vert_spirv
															   : push_vert_spirv;
	const uint32_t vertex_bytes = kind == PIPELINE_PUSH_PLANE ? CLIP_VERT_SPIRV_BYTES
		: kind == PIPELINE_UNIFORM_PLANES                     ? CLIP_PLANES_VERT_SPIRV_BYTES
															  : PUSH_VERT_SPIRV_BYTES;
	const HarnessPipeline spec = {
		.vertex_spirv = vertex,
		.vertex_bytes = vertex_bytes,
		.fragment_spirv = push_frag_spirv,
		.fragment_bytes = PUSH_FRAG_SPIRV_BYTES,
		.bindings = &harness_quad_binding,
		.binding_count = 1,
		.attributes = &harness_quad_attribute,
		.attribute_count = 1,
		.layout = scene->layout,
	};
	return harness_pipeline(harness, &spec, scene->render_pass, &scene->pipelines[kind]);
}

static bool build(const Harness *harness, Scene *scene)
{
	bool ok = harness_ok("layout", create_layout(harness, scene));
	ok = ok && harness_ok("planes", create_planes(harness, scene));
	ok = ok && harness_ok("render pass", harness_render_pass(harness, &scene->render_pass, scene->framebuffers));
	for (uint32_t i = 0; i < PIPELINES && ok; i++)
		ok = harness_ok("pipeline", create_pipeline(harness, scene, (PipelineKind)i));
	return ok && harness_ok("vertex buffer", harness_quad_buffer(harness, &scene->vertices, &scene->vertex_memory));
}

static void bind(VkCommandBuffer commands, const Scene *scene, PipelineKind kind)
{
	vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, scene->pipelines[kind]);
}

static void draw_quad(VkCommandBuffer commands, const Scene *scene, float x, float y, float width, const float color[4])
{
	HarnessQuad quad = { x, y, width, CELL_HEIGHT, 0.0f, { color[0], color[1], color[2], color[3] }, false };
	harness_push_quad(commands, scene->layout, &quad);
}

static void push_plane(VkCommandBuffer commands, const Scene *scene, const float plane[4])
{
	vkCmdPushConstants(commands, scene->layout, VK_SHADER_STAGE_VERTEX_BIT, PLANE_OFFSET_BYTES, PLANE_BYTES, plane);
}

// Two quads cut by planes pushed before each, then two by the uniform buffer's planes at each draw's offset.
static void record_top(VkCommandBuffer commands, const Scene *scene)
{
	float plane[4];
	bind(commands, scene, PIPELINE_PUSH_PLANE);
	keep_columns_from(plane, PUSH_COLUMN);
	push_plane(commands, scene, plane);
	draw_quad(commands, scene, 40, TOP_Y, 300, red);
	keep_rows_from(plane, PUSH_ROW);
	push_plane(commands, scene, plane);
	draw_quad(commands, scene, 380, TOP_Y, 300, blue);
	bind(commands, scene, PIPELINE_UNIFORM_PLANES);
	for (uint32_t i = 0; i < 2; i++) {
		const uint32_t offset = i * PLANES_STRIDE_BYTES;
		vkCmdBindDescriptorSets(
			commands, VK_PIPELINE_BIND_POINT_GRAPHICS, scene->layout, 0, 1, &scene->set, 1, &offset);
		draw_quad(commands, scene, i ? 1060 : 720, TOP_Y, i ? 180 : 300, i ? magenta : yellow);
	}
}

// A pipeline without clip distances after them; then a clear under one whose plane cuts everything, and its draw after.
static void record_bottom(VkCommandBuffer commands, const Scene *scene)
{
	bind(commands, scene, PIPELINE_PLAIN);
	draw_quad(commands, scene, 40, BOTTOM_Y, 300, cyan);
	float plane[4];
	bind(commands, scene, PIPELINE_PUSH_PLANE);
	keep_columns_from(plane, OFF_SCREEN_COLUMN);
	push_plane(commands, scene, plane);
	draw_quad(commands, scene, 380, BOTTOM_Y, 300, red);
	const VkClearAttachment attachment = {
		.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
		.clearValue = { .color = { .float32 = { green[0], green[1], green[2], green[3] } } },
	};
	const VkClearRect rect = { { { 380, BOTTOM_Y }, { 300, CELL_HEIGHT } }, 0, 1 };
	vkCmdClearAttachments(commands, 1, &attachment, 1, &rect);
	keep_columns_from(plane, AFTER_CLEAR_COLUMN);
	push_plane(commands, scene, plane);
	draw_quad(commands, scene, 720, BOTTOM_Y, 300, orange);
}

static VkResult record(const Harness *harness, Scene *scene)
{
	VkResult result = harness_command_buffers(harness, scene->commands);
	for (uint32_t i = 0; i < harness->image_count && result == VK_SUCCESS; i++) {
		const VkRenderPassBeginInfo pass = {
			.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
			.renderPass = scene->render_pass,
			.framebuffer = scene->framebuffers[i],
			.renderArea = { { 0, 0 }, { HARNESS_WIDTH, HARNESS_HEIGHT } },
			.clearValueCount = 1,
			.pClearValues = &clear,
		};
		const VkDeviceSize offset = 0;
		const VkCommandBuffer commands = scene->commands[i];
		vkCmdBeginRenderPass(commands, &pass, VK_SUBPASS_CONTENTS_INLINE);
		vkCmdBindVertexBuffers(commands, 0, 1, &scene->vertices, &offset);
		record_top(commands, scene);
		record_bottom(commands, scene);
		vkCmdEndRenderPass(commands);
		result = vkEndCommandBuffer(commands);
	}
	return result;
}

// Every group's probes, each group's misses counted apart.
static bool groups_hold(uint32_t address, const char *what, void *context)
{
	Scene *const scene = context;
	bool all = true;
	for (uint32_t g = 0; g < GROUPS; g++) {
		if (!harness_probes_hold(address, what, &scene->checks[g])) {
			scene->misses[g]++;
			all = false;
		}
	}
	return all;
}

// The feature and limits Xenos's six planes give, and no cull distances.
static void check_limits(const Harness *harness)
{
	VkPhysicalDeviceFeatures features;
	vkGetPhysicalDeviceFeatures(harness->physical_device, &features);
	VkPhysicalDeviceProperties properties;
	vkGetPhysicalDeviceProperties(harness->physical_device, &properties);
	const VkPhysicalDeviceLimits *const limits = &properties.limits;
	check(
		"clip distances: shaderClipDistance reported, maxClipDistances and maxCombinedClipAndCullDistances 6, no "
		"cull distances",
		features.shaderClipDistance && !features.shaderCullDistance && limits->maxClipDistances == XSDK_CLIP_PLANES &&
			limits->maxCullDistances == 0 && limits->maxCombinedClipAndCullDistances == XSDK_CLIP_PLANES);
}

static void destroy(const Harness *harness, Scene *scene)
{
	const VkDevice device = harness->device;
	vkFreeCommandBuffers(device, harness->pool, harness->image_count, scene->commands);
	vkDestroyBuffer(device, scene->vertices, NULL);
	vkFreeMemory(device, scene->vertex_memory, NULL);
	for (uint32_t i = 0; i < PIPELINES; i++)
		vkDestroyPipeline(device, scene->pipelines[i], NULL);
	vkDestroyDescriptorPool(device, scene->pool, NULL);
	vkDestroyBuffer(device, scene->planes, NULL);
	vkFreeMemory(device, scene->plane_memory, NULL);
	vkDestroyPipelineLayout(device, scene->layout, NULL);
	vkDestroyDescriptorSetLayout(device, scene->set_layout, NULL);
	harness_destroy_render_pass(harness, scene->render_pass, scene->framebuffers);
}

void test_clip_distances(const Harness *harness)
{
	check_limits(harness);
	Scene scene = { .render_pass = VK_NULL_HANDLE };
	scene.checks[GROUP_PUSH] =
		(HarnessProbes){ "clip distances", push_probes, sizeof push_probes / sizeof push_probes[0], TOLERANCE, 0 };
	scene.checks[GROUP_UNIFORM] = (HarnessProbes){ "clip distances", uniform_probes,
		sizeof uniform_probes / sizeof uniform_probes[0], TOLERANCE, 0 };
	scene.checks[GROUP_AFTER] =
		(HarnessProbes){ "clip distances", after_probes, sizeof after_probes / sizeof after_probes[0], TOLERANCE, 0 };
	const bool built = build(harness, &scene);
	check("clip distances: a plane buffer, a render pass and three pipelines made", built);
	const bool recorded = built && harness->swapchain && harness_ok("recording", record(harness, &scene));
	uint32_t misses = 0;
	const uint32_t frames =
		recorded ? harness_frames(harness, scene.commands, FRAMES, groups_hold, &scene, &misses) : 0;
	check("clip distances: 30 frames drawn and presented", frames == FRAMES);
	check("clip distances: every frame's quads are cut at the lines of the planes pushed before each draw",
		frames == FRAMES && scene.misses[GROUP_PUSH] == 0);
	check(
		"clip distances: every frame's quads are cut by distances 1 and 4, planes from a dynamic uniform buffer at "
		"each draw's offset",
		frames == FRAMES && scene.misses[GROUP_UNIFORM] == 0);
	check(
		"clip distances: every frame cuts nothing of a pipeline without them nor of vkCmdClearAttachments's "
		"rectangle, and cuts the draw after the clear again",
		frames == FRAMES && scene.misses[GROUP_AFTER] == 0);
	destroy(harness, &scene);
}
