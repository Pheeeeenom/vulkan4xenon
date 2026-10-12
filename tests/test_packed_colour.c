#include <stdbool.h>
#include <stdint.h>

#include "harness.h"
#include "shaders.h"
#include "tests.h"
#include "xsdk/log.h"

enum {
	FRAMES = 60,
	COLOR_OFFSET_BYTES = 16,
	// triangle's corners, as test_triangle draws them.
	TRIANGLE_PIXELS = 960 * 540 / 2,
	TRIANGLE_TOLERANCE_PIXELS = 2000,
	CHANNEL_HIGH = 0xC0,
	CHANNEL_LOW = 0x40,
	MAX_MISSES_LOGGED = 4,
};

// A position and a VK_FORMAT_A8B8G8R8_UNORM_PACK32 colour: red in the word's low byte, alpha in its high one.
typedef struct {
	float position[4];
	uint32_t colour;
} Vertex;

static uint32_t pack(uint32_t red, uint32_t green, uint32_t blue, uint32_t alpha)
{
	return red | green << 8 | blue << 16 | alpha << 24;
}

// test_triangle's triangle in yellow, cyan and magenta, so the screen tells the two tests apart.
static const Vertex triangle[3] = {
	{ { 0.0f, -0.75f, 0.0f, 1.0f }, 0 },
	{ { -0.75f, 0.75f, 0.0f, 1.0f }, 0 },
	{ { 0.75f, 0.75f, 0.0f, 1.0f }, 0 },
};

static const VkVertexInputBindingDescription binding = { 0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX };
static const VkVertexInputAttributeDescription attributes[] = {
	{ .location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = 0 },
	{ .location = 1, .binding = 0, .format = VK_FORMAT_A8B8G8R8_UNORM_PACK32, .offset = COLOR_OFFSET_BYTES },
};

// A dark grey no other test clears to: 48 in each channel after rounding to 8 bits.
static const VkClearValue clear = { .color = { .float32 = { 0.1875f, 0.1875f, 0.1875f, 1.0f } } };
static const Rgb clear_rgb = { 48, 48, 48 };

typedef struct {
	const char *name;
	uint32_t x;
	uint32_t y;
	Rgb high;
} Corner;

// A pixel just inside each corner, and which channels should be bright there.
static const Corner corners[] = {
	{ "yellow", 640, 120, { 1, 1, 0 } },
	{ "cyan", 200, 615, { 0, 1, 1 } },
	{ "magenta", 1080, 615, { 1, 0, 1 } },
};

static const uint32_t outside_x = 20;
static const uint32_t outside_y = 20;

typedef struct {
	VkRenderPass render_pass;
	VkFramebuffer framebuffers[HARNESS_MAX_IMAGES];
	VkPipelineLayout layout;
	VkPipeline pipeline;
	VkBuffer vertices;
	VkDeviceMemory vertex_memory;
	VkCommandBuffer commands[HARNESS_MAX_IMAGES];
	uint32_t logged;
	uint32_t drawn_on_screen;
} Scene;

static bool channel_holds(uint32_t value, uint32_t high)
{
	return high ? value >= CHANNEL_HIGH : value <= CHANNEL_LOW;
}

// Each corner has its colour, and a pixel outside the triangle the clear colour.
static bool image_holds(uint32_t address, const char *what, void *context)
{
	Scene *const scene = context;
	bool holds = harness_near(harness_pixel(address, outside_x, outside_y), clear_rgb, 0);
	for (uint32_t i = 0; i < sizeof corners / sizeof corners[0]; i++) {
		const Corner *const corner = &corners[i];
		const Rgb pixel = harness_pixel(address, corner->x, corner->y);
		if (channel_holds(pixel.red, corner->high.red) && channel_holds(pixel.green, corner->high.green) &&
			channel_holds(pixel.blue, corner->high.blue))
			continue;
		if (scene->logged++ < MAX_MISSES_LOGGED)
			xsdk_log_write("packed_colour: %s: the %s corner held %02X %02X %02X", what, corner->name,
				(unsigned)pixel.red, (unsigned)pixel.green, (unsigned)pixel.blue);
		holds = false;
	}
	return holds;
}

// The corners, and how many pixels the image on screen drew.
static bool screen_holds(uint32_t address, const char *what, void *context)
{
	Scene *const scene = context;
	for (uint32_t y = 0; y < HARNESS_HEIGHT; y++) {
		for (uint32_t x = 0; x < HARNESS_WIDTH; x++)
			scene->drawn_on_screen += !harness_near(harness_pixel(address, x, y), clear_rgb, 0);
	}
	xsdk_log_write("packed_colour: the image on screen, %08X, drew %u pixels", (unsigned)address,
		(unsigned)scene->drawn_on_screen);
	return image_holds(address, what, context);
}

static bool build_pipeline(const Harness *harness, Scene *scene)
{
	Vertex vertices[3];
	const uint32_t colours[3] = { pack(255, 255, 0, 255), pack(0, 255, 255, 255), pack(255, 0, 255, 255) };
	for (uint32_t i = 0; i < 3; i++) {
		vertices[i] = triangle[i];
		vertices[i].colour = colours[i];
	}
	const VkPipelineLayoutCreateInfo layout_info = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
	bool ok = harness_ok(
		"vkCreatePipelineLayout", vkCreatePipelineLayout(harness->device, &layout_info, NULL, &scene->layout));
	const HarnessPipeline spec = {
		.vertex_spirv = triangle_vert_spirv,
		.vertex_bytes = TRIANGLE_VERT_SPIRV_BYTES,
		.fragment_spirv = triangle_frag_spirv,
		.fragment_bytes = TRIANGLE_FRAG_SPIRV_BYTES,
		.bindings = &binding,
		.binding_count = 1,
		.attributes = attributes,
		.attribute_count = 2,
		.layout = scene->layout,
	};
	ok = ok && harness_ok("render pass", harness_render_pass(harness, &scene->render_pass, scene->framebuffers));
	ok = ok && harness_ok("pipeline", harness_pipeline(harness, &spec, scene->render_pass, &scene->pipeline));
	return ok &&
		harness_ok("vertex buffer",
			harness_buffer(harness, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, vertices, sizeof vertices, &scene->vertices,
				&scene->vertex_memory));
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
		vkCmdBeginRenderPass(scene->commands[i], &pass, VK_SUBPASS_CONTENTS_INLINE);
		vkCmdBindPipeline(scene->commands[i], VK_PIPELINE_BIND_POINT_GRAPHICS, scene->pipeline);
		vkCmdBindVertexBuffers(scene->commands[i], 0, 1, &scene->vertices, &offset);
		vkCmdDraw(scene->commands[i], 3, 1, 0, 0);
		vkCmdEndRenderPass(scene->commands[i]);
		result = vkEndCommandBuffer(scene->commands[i]);
	}
	return result;
}

static void destroy_scene(const Harness *harness, Scene *scene)
{
	const VkDevice device = harness->device;
	vkFreeCommandBuffers(device, harness->pool, harness->image_count, scene->commands);
	vkDestroyBuffer(device, scene->vertices, NULL);
	vkFreeMemory(device, scene->vertex_memory, NULL);
	vkDestroyPipeline(device, scene->pipeline, NULL);
	vkDestroyPipelineLayout(device, scene->layout, NULL);
	harness_destroy_render_pass(harness, scene->render_pass, scene->framebuffers);
}

void test_packed_colour(const Harness *harness)
{
	VkFormatProperties properties;
	vkGetPhysicalDeviceFormatProperties(harness->physical_device, VK_FORMAT_A8B8G8R8_UNORM_PACK32, &properties);
	check("packed_colour: A8B8G8R8_UNORM_PACK32 is a vertex buffer format",
		(properties.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) != 0);
	Scene scene = { .render_pass = VK_NULL_HANDLE };
	const bool pipeline = build_pipeline(harness, &scene);
	check("packed_colour: a pipeline with 8-bit packed colours made", pipeline);
	const bool built = pipeline && harness->swapchain && harness_ok("recording", record(harness, &scene));
	check("packed_colour: framebuffers made and command buffers recorded", built);
	uint32_t misses = 0;
	const uint32_t frames = built ? harness_frames(harness, scene.commands, FRAMES, image_holds, &scene, &misses) : 0;
	check("packed_colour: every frame's image has yellow, cyan and magenta corners on the clear colour",
		frames == FRAMES && misses == 0);
	const bool on_screen = harness_screen(harness, screen_holds, &scene);
	check("packed_colour: the image on screen has yellow, cyan and magenta corners", on_screen);
	check("packed_colour: the image on screen covers the triangle's area",
		scene.drawn_on_screen + TRIANGLE_TOLERANCE_PIXELS >= TRIANGLE_PIXELS &&
			scene.drawn_on_screen <= TRIANGLE_PIXELS + TRIANGLE_TOLERANCE_PIXELS);
	destroy_scene(harness, &scene);
}
