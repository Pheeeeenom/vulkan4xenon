#include <stdbool.h>
#include <stdint.h>

#include "harness.h"
#include "shaders.h"
#include "tests.h"

// Milestone 26's step 3: point sprites as wide as the vertex shader's gl_PointSize, coloured by gl_PointCoord, and a
// point without a size, which is 1 pixel.

enum {
	FRAMES = 30,
	TOLERANCE = 2,
	// Where in a pixel the hardware takes a sprite's coordinates isn't known: two pixels' worth of the smaller one's.
	COORD_TOLERANCE = 32,
	SPRITE_Y = 200,
	// The 1-pixel point, at a pixel's centre.
	PIXEL_X = 900,
	PIXEL_Y = 200,
};

typedef enum {
	PIPELINE_SPRITE,
	PIPELINE_PIXEL,
	PIPELINES,
} PipelineKind;

static const VkClearValue clear = { .color = { .float32 = { 20.0f / 255, 20.0f / 255, 28.0f / 255, 1.0f } } };

// Sprites centred on a pixel's corner, as wide as their size.
typedef struct {
	uint32_t x;
	float size;
} Sprite;

static const Sprite sprites[] = { { 300, 16.0f }, { 600, 48.0f } };

enum {
	SPRITES = sizeof sprites / sizeof sprites[0],
	PROBES_PER_SPRITE = 8,
};

typedef struct {
	VkRenderPass render_pass;
	VkFramebuffer framebuffers[HARNESS_MAX_IMAGES];
	VkPipelineLayout layout;
	VkPipeline pipelines[PIPELINES];
	VkBuffer vertices;
	VkDeviceMemory vertex_memory;
	VkCommandBuffer commands[HARNESS_MAX_IMAGES];
	HarnessProbe sprite_probes[SPRITES * PROBES_PER_SPRITE];
	HarnessProbes sprite_check;
	HarnessProbes pixel_check;
	uint32_t sprite_misses;
	uint32_t pixel_misses;
} Scene;

static const HarnessProbe pixel_probes[] = {
	{ "a point without gl_PointSize: its pixel", PIXEL_X, PIXEL_Y, { 255, 255, 255 }, 0 },
	{ "a point without gl_PointSize: left of it", PIXEL_X - 1, PIXEL_Y, { 20, 20, 28 }, 0 },
	{ "a point without gl_PointSize: right of it", PIXEL_X + 1, PIXEL_Y, { 20, 20, 28 }, 0 },
	{ "a point without gl_PointSize: above it", PIXEL_X, PIXEL_Y - 1, { 20, 20, 28 }, 0 },
	{ "a point without gl_PointSize: below it", PIXEL_X, PIXEL_Y + 1, { 20, 20, 28 }, 0 },
};

static uint32_t to_byte(float value)
{
	const float clamped = value < 0.0f ? 0.0f : value > 1.0f ? 1.0f : value;
	return (uint32_t)(clamped * 255.0f + 0.5f);
}

// gl_PointCoord's colour at the pixel dx, dy from a sprite's centre: 0 at its left and top edges to 1 at its right and
// bottom, at the pixel's centre, half a pixel on.
static Rgb sprite_rgb(const Sprite *s, int32_t dx, int32_t dy)
{
	const float u = (dx + 0.5f + s->size / 2.0f) / s->size;
	const float v = (dy + 0.5f + s->size / 2.0f) / s->size;
	return (Rgb){ to_byte(u), to_byte(v), 0 };
}

// Each sprite's corners, 3 pixels in, and the pixels 3 out from its edges, which stay clear.
static void make_probes(Scene *scene)
{
	const Rgb clear_rgb = { 20, 20, 28 };
	uint32_t n = 0;
	for (uint32_t i = 0; i < SPRITES; i++) {
		const Sprite *const s = &sprites[i];
		const int32_t in = (int32_t)(s->size / 2.0f) - 3;
		const int32_t out = (int32_t)(s->size / 2.0f) + 2;
		const HarnessProbe probes[PROBES_PER_SPRITE] = {
			{ "a sprite's top left", s->x - in, SPRITE_Y - in, sprite_rgb(s, -in, -in), 0 },
			{ "a sprite's top right", s->x + in - 1, SPRITE_Y - in, sprite_rgb(s, in - 1, -in), 0 },
			{ "a sprite's bottom left", s->x - in, SPRITE_Y + in - 1, sprite_rgb(s, -in, in - 1), 0 },
			{ "a sprite's bottom right", s->x + in - 1, SPRITE_Y + in - 1, sprite_rgb(s, in - 1, in - 1), 0 },
			{ "left of a sprite", s->x - out - 1, SPRITE_Y, clear_rgb, 0 },
			{ "right of a sprite", s->x + out, SPRITE_Y, clear_rgb, 0 },
			{ "above a sprite", s->x, SPRITE_Y - out - 1, clear_rgb, 0 },
			{ "below a sprite", s->x, SPRITE_Y + out, clear_rgb, 0 },
		};
		for (uint32_t p = 0; p < PROBES_PER_SPRITE; p++)
			scene->sprite_probes[n++] = probes[p];
	}
	scene->sprite_check = (HarnessProbes){ "point sprites", scene->sprite_probes, n, COORD_TOLERANCE, 0 };
	scene->pixel_check =
		(HarnessProbes){ "point sprites", pixel_probes, sizeof pixel_probes / sizeof pixel_probes[0], TOLERANCE, 0 };
}

static VkResult create_pipeline(const Harness *harness, Scene *scene, PipelineKind kind)
{
	const VkPipelineInputAssemblyStateCreateInfo points = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
		.topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST,
	};
	const bool sprite = kind == PIPELINE_SPRITE;
	const HarnessPipeline spec = {
		.vertex_spirv = sprite ? point_vert_spirv : push_vert_spirv,
		.vertex_bytes = sprite ? POINT_VERT_SPIRV_BYTES : PUSH_VERT_SPIRV_BYTES,
		.fragment_spirv = sprite ? point_coord_frag_spirv : push_frag_spirv,
		.fragment_bytes = sprite ? POINT_COORD_FRAG_SPIRV_BYTES : PUSH_FRAG_SPIRV_BYTES,
		.bindings = &harness_quad_binding,
		.binding_count = 1,
		.attributes = &harness_quad_attribute,
		.attribute_count = 1,
		.layout = scene->layout,
		.assembly = &points,
	};
	return harness_pipeline(harness, &spec, scene->render_pass, &scene->pipelines[kind]);
}

static bool build(const Harness *harness, Scene *scene)
{
	static const float point[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
	bool ok = harness_ok("push layout", harness_push_layout(harness, &scene->layout));
	ok = ok && harness_ok("render pass", harness_render_pass(harness, &scene->render_pass, scene->framebuffers));
	for (uint32_t i = 0; i < PIPELINES && ok; i++)
		ok = harness_ok("pipeline", create_pipeline(harness, scene, (PipelineKind)i));
	return ok &&
		harness_ok("vertex buffer",
			harness_buffer(harness, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, point, sizeof point, &scene->vertices,
				&scene->vertex_memory));
}

// Vulkan's clip-space x of a column's left edge, or y of a row's top, y down.
static float clip_x(float column)
{
	return column / (HARNESS_WIDTH / 2.0f) - 1.0f;
}

static float clip_y(float row)
{
	return row / (HARNESS_HEIGHT / 2.0f) - 1.0f;
}

// point.vert's offset and size; push.vert's scale of one and offset, then push.frag's white.
static void record_points(VkCommandBuffer commands, const Scene *scene)
{
	vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, scene->pipelines[PIPELINE_SPRITE]);
	for (uint32_t i = 0; i < SPRITES; i++) {
		const float place[5] = { clip_x((float)sprites[i].x), clip_y(SPRITE_Y), 0.0f, 0.0f, sprites[i].size };
		vkCmdPushConstants(commands, scene->layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof place, place);
		vkCmdDraw(commands, 1, 1, 0, 0);
	}
	vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, scene->pipelines[PIPELINE_PIXEL]);
	const float place[8] = { 1.0f, 1.0f, 1.0f, 1.0f, clip_x(PIXEL_X + 0.5f), clip_y(PIXEL_Y + 0.5f), 0.0f, 0.0f };
	const float white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
	vkCmdPushConstants(commands, scene->layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof place, place);
	vkCmdPushConstants(commands, scene->layout, VK_SHADER_STAGE_FRAGMENT_BIT, 32, sizeof white, white);
	vkCmdDraw(commands, 1, 1, 0, 0);
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
		record_points(commands, scene);
		vkCmdEndRenderPass(commands);
		result = vkEndCommandBuffer(commands);
	}
	return result;
}

static bool groups_hold(uint32_t address, const char *what, void *context)
{
	Scene *const scene = context;
	const bool sprites_hold = harness_probes_hold(address, what, &scene->sprite_check);
	const bool pixel_holds = harness_probes_hold(address, what, &scene->pixel_check);
	scene->sprite_misses += !sprites_hold;
	scene->pixel_misses += !pixel_holds;
	return sprites_hold && pixel_holds;
}

// What PA_SU_POINT_MINMAX allows a vertex's size: radii of 0 to 0xFFFF sixteenths of a pixel
// (xenia:src/xenia/gpu/registers.h:433-442).
static void check_limits(const Harness *harness)
{
	VkPhysicalDeviceFeatures features;
	vkGetPhysicalDeviceFeatures(harness->physical_device, &features);
	VkPhysicalDeviceProperties properties;
	vkGetPhysicalDeviceProperties(harness->physical_device, &properties);
	const VkPhysicalDeviceLimits *const limits = &properties.limits;
	check("point sprites: largePoints reported, pointSizeRange 0 to 8191.875, granularity 0.125",
		features.largePoints && limits->pointSizeRange[0] == 0.0f && limits->pointSizeRange[1] == 8191.875f &&
			limits->pointSizeGranularity == 0.125f);
}

static void destroy(const Harness *harness, Scene *scene)
{
	const VkDevice device = harness->device;
	vkFreeCommandBuffers(device, harness->pool, harness->image_count, scene->commands);
	vkDestroyBuffer(device, scene->vertices, NULL);
	vkFreeMemory(device, scene->vertex_memory, NULL);
	for (uint32_t i = 0; i < PIPELINES; i++)
		vkDestroyPipeline(device, scene->pipelines[i], NULL);
	vkDestroyPipelineLayout(device, scene->layout, NULL);
	harness_destroy_render_pass(harness, scene->render_pass, scene->framebuffers);
}

void test_point_sprites(const Harness *harness)
{
	check_limits(harness);
	Scene scene = { .render_pass = VK_NULL_HANDLE };
	make_probes(&scene);
	const bool built = build(harness, &scene);
	check("point sprites: a point's buffer, a render pass and two pipelines made", built);
	const bool recorded = built && harness->swapchain && harness_ok("recording", record(harness, &scene));
	uint32_t misses = 0;
	const uint32_t frames =
		recorded ? harness_frames(harness, scene.commands, FRAMES, groups_hold, &scene, &misses) : 0;
	check("point sprites: 30 frames drawn and presented", frames == FRAMES);
	check(
		"point sprites: every frame's sprites are as wide as their gl_PointSize, 16 and 48 pixels, gl_PointCoord "
		"across each from its top left",
		frames == FRAMES && scene.sprite_misses == 0);
	check(
		"point sprites: every frame's point without gl_PointSize is 1 pixel", frames == FRAMES && !scene.pixel_misses);
	destroy(harness, &scene);
}
