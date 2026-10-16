#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "harness.h"
#include "shaders.h"
#include "tests.h"
#include "xsdk/log.h"

// Milestone 20's step 2: vertex attributes of half floats, bytes and shorts, normalized or scaled. Each format's cell
// draws four squares coloured from it, each component of each apart, then again with the alpha in red, green and blue;
// and most formats place a triangle that x and y swapped would mirror, or w and z swapped would lose. The vertex shader
// scales and moves the colours so signed and scaled values land within 0 to 1.

enum {
	FRAMES = 30,
	FENCE_TIMEOUT_NS = 1000000000,
	SQUARES = 4,
	SQUARE = 28,
	CELL_STEP = 66,
	LEFT = 13,
	COLOUR_TOP = 40,
	ALPHA_TOP = 110,
	// A triangle cell is 64 pixels a side, across from the left.
	TRIANGLE_TOP = 200,
	TRIANGLE_SIDE = 64,
	TRIANGLE_STEP = 80,
	CELL_VERTICES = SQUARES * 6,
	TRIANGLE_VERTICES = 3,
	// Positions are four floats or the format's; a colour, the format's or four floats, sits 4 bytes into its vertex,
	// after 4 bytes it doesn't read.
	POSITION_STRIDE = 16,
	COLOUR_OFFSET = 4,
	COLOUR_BYTES_MAX = 16,
	COLOUR_STRIDE = COLOUR_OFFSET + COLOUR_BYTES_MAX,
	TOLERANCE = 1,
	MAX_MISSES_LOGGED = 8,
	MAX_PROBES = 512,
};

enum {
	UNORM,
	SNORM,
	USCALED,
	SSCALED,
	HALF,
};

// A vertex format as Vulkan defines it: components of bits each, red to alpha at places an index each, each in the
// CPU's byte order; or one 32-bit word, red in its low byte.
typedef struct {
	VkFormat format;
	const char *name;
	uint8_t components;
	uint8_t bits;
	uint8_t number;
	uint8_t places[4];
	bool packed;
	// It places a triangle too.
	bool positions;
} Spec;

static const Spec specs[] = {
	{ VK_FORMAT_A8B8G8R8_UNORM_PACK32, "A8B8G8R8_UNORM_PACK32", 4, 8, UNORM, { 0, 1, 2, 3 }, true, false },
	{ VK_FORMAT_A8B8G8R8_SNORM_PACK32, "A8B8G8R8_SNORM_PACK32", 4, 8, SNORM, { 0, 1, 2, 3 }, true, true },
	{ VK_FORMAT_A8B8G8R8_USCALED_PACK32, "A8B8G8R8_USCALED_PACK32", 4, 8, USCALED, { 0, 1, 2, 3 }, true, false },
	{ VK_FORMAT_A8B8G8R8_SSCALED_PACK32, "A8B8G8R8_SSCALED_PACK32", 4, 8, SSCALED, { 0, 1, 2, 3 }, true, false },
	{ VK_FORMAT_R8G8B8A8_UNORM, "R8G8B8A8_UNORM", 4, 8, UNORM, { 0, 1, 2, 3 }, false, true },
	{ VK_FORMAT_R8G8B8A8_SNORM, "R8G8B8A8_SNORM", 4, 8, SNORM, { 0, 1, 2, 3 }, false, false },
	{ VK_FORMAT_R8G8B8A8_USCALED, "R8G8B8A8_USCALED", 4, 8, USCALED, { 0, 1, 2, 3 }, false, true },
	{ VK_FORMAT_R8G8B8A8_SSCALED, "R8G8B8A8_SSCALED", 4, 8, SSCALED, { 0, 1, 2, 3 }, false, false },
	{ VK_FORMAT_B8G8R8A8_UNORM, "B8G8R8A8_UNORM", 4, 8, UNORM, { 2, 1, 0, 3 }, false, true },
	{ VK_FORMAT_R16G16_UNORM, "R16G16_UNORM", 2, 16, UNORM, { 0, 1 }, false, false },
	{ VK_FORMAT_R16G16_SNORM, "R16G16_SNORM", 2, 16, SNORM, { 0, 1 }, false, true },
	{ VK_FORMAT_R16G16_USCALED, "R16G16_USCALED", 2, 16, USCALED, { 0, 1 }, false, false },
	{ VK_FORMAT_R16G16_SSCALED, "R16G16_SSCALED", 2, 16, SSCALED, { 0, 1 }, false, true },
	{ VK_FORMAT_R16G16_SFLOAT, "R16G16_SFLOAT", 2, 16, HALF, { 0, 1 }, false, true },
	{ VK_FORMAT_R16G16B16A16_UNORM, "R16G16B16A16_UNORM", 4, 16, UNORM, { 0, 1, 2, 3 }, false, true },
	{ VK_FORMAT_R16G16B16A16_SNORM, "R16G16B16A16_SNORM", 4, 16, SNORM, { 0, 1, 2, 3 }, false, false },
	{ VK_FORMAT_R16G16B16A16_USCALED, "R16G16B16A16_USCALED", 4, 16, USCALED, { 0, 1, 2, 3 }, false, false },
	{ VK_FORMAT_R16G16B16A16_SSCALED, "R16G16B16A16_SSCALED", 4, 16, SSCALED, { 0, 1, 2, 3 }, false, true },
	{ VK_FORMAT_R16G16B16A16_SFLOAT, "R16G16B16A16_SFLOAT", 4, 16, HALF, { 0, 1, 2, 3 }, false, true },
};

enum {
	SPECS = sizeof specs / sizeof specs[0],
};

// What each spec draws: its colour cell, its alpha cell and its triangle.
enum {
	DRAW_COLOUR,
	DRAW_ALPHA,
	DRAW_TRIANGLE,
	DRAWS,
};

// Where each draw's vertices sit in the two vertex buffers.
enum {
	DRAW_VERTICES = CELL_VERTICES,
	POSITION_BYTES = SPECS * DRAWS * DRAW_VERTICES * POSITION_STRIDE,
	COLOUR_BYTES = SPECS * DRAWS * DRAW_VERTICES * COLOUR_STRIDE,
};

typedef struct {
	uint32_t spec;
	uint32_t x;
	uint32_t y;
	Rgb want;
	// For the spec's triangle check, not its colours'.
	bool triangle;
} Probe;

typedef struct {
	VkRenderPass render_pass;
	VkFramebuffer framebuffers[HARNESS_MAX_IMAGES];
	VkPipelineLayout layout;
	VkPipeline pipelines[SPECS][DRAWS];
	VkBuffer positions;
	VkDeviceMemory position_memory;
	VkBuffer colours;
	VkDeviceMemory colour_memory;
	VkCommandBuffer commands[HARNESS_MAX_IMAGES];
	Probe probes[MAX_PROBES];
	uint32_t probe_count;
	// Misses for each spec's colours and its triangle.
	uint32_t missed[SPECS][2];
	uint32_t logged;
} Scene;

static int32_t max_of(const Spec *spec)
{
	const uint32_t bits = spec->number == SNORM || spec->number == SSCALED ? spec->bits - 1u : spec->bits;
	return (int32_t)((1u << bits) - 1);
}

// The nearest half to a value in its normal range.
static uint16_t to_half(float value)
{
	uint32_t bits;
	memcpy(&bits, &value, sizeof bits);
	const uint32_t sign = bits >> 16 & 0x8000;
	const int32_t exponent = (int32_t)(bits >> 23 & 0xFF) - 127 + 15;
	if (exponent <= 0)
		return (uint16_t)sign;
	return (uint16_t)(sign | (((uint32_t)exponent << 10 | (bits & 0x7FFFFF) >> 13) + (bits >> 12 & 1)));
}

static float from_half(uint32_t half)
{
	const uint32_t exponent = half >> 10 & 0x1F;
	const float magnitude =
		exponent ? (float)(0x400 | (half & 0x3FF)) * (float)(1u << exponent) / (float)(1u << 25) : 0.0f;
	return half & 0x8000 ? -magnitude : magnitude;
}

// The value a component stores for a square: apart for each component and square, a signed format's most negative
// in one.
static int32_t stored_value(const Spec *spec, uint32_t square, uint32_t component)
{
	const int32_t step = (int32_t)((square * 5 + component * 3 + 1) % 16);
	if (spec->number == HALF)
		return to_half((float)step / 16.0f);
	const int32_t max = max_of(spec);
	if (spec->number == UNORM || spec->number == USCALED)
		return step * max / 15;
	if (square == 0 && component == 0)
		return -max - 1;
	return (step - 8) * max / 8;
}

// The value a position stores for 0 or 1.
static int32_t stored_unit(const Spec *spec, uint32_t value)
{
	if (spec->number == HALF)
		return value ? to_half(1.0f) : 0;
	return spec->number == USCALED || spec->number == SSCALED ? (int32_t)value : (int32_t)value * max_of(spec);
}

// What Vulkan reads: fixed point as 0 to 1, or -1 to 1 with the most negative read as -1; whole numbers; halves.
static float read_value(const Spec *spec, int32_t stored)
{
	if (spec->number == HALF)
		return from_half((uint32_t)stored);
	const float max = (float)max_of(spec);
	if (spec->number == UNORM)
		return (float)stored / max;
	if (spec->number == SNORM)
		return (float)stored / max < -1.0f ? -1.0f : (float)stored / max;
	return (float)stored;
}

// The shader's scale and bias, which bring each format's range to 0 to 1.
static void colour_scale(const Spec *spec, float *scale, float *bias)
{
	const float max = (float)max_of(spec);
	*scale = 1.0f;
	*bias = 0.0f;
	if (spec->number == SNORM) {
		*scale = 0.5f;
		*bias = 0.5f;
	} else if (spec->number == USCALED) {
		*scale = 1.0f / max;
	} else if (spec->number == SSCALED) {
		*scale = 1.0f / (2.0f * max + 1.0f);
		*bias = (max + 1.0f) / (2.0f * max + 1.0f);
	}
}

// A component's bits at its place in the attribute, each unit in the CPU's byte order; or in one word, red low.
static void put_component(const Spec *spec, uint8_t *attribute, uint32_t component, int32_t value)
{
	const uint32_t field = (uint32_t)value & ((1u << spec->bits) - 1);
	if (spec->packed) {
		attribute[3 - component] = (uint8_t)field;
		return;
	}
	uint8_t *const unit = attribute + spec->places[component] * spec->bits / 8;
	if (spec->bits == 8) {
		unit[0] = (uint8_t)field;
		return;
	}
	unit[0] = (uint8_t)(field >> 8);
	unit[1] = (uint8_t)field;
}

static uint32_t to_byte(float value)
{
	const float clamped = !(value > 0.0f) ? 0.0f : value > 1.0f ? 1.0f : value;
	return (uint32_t)(clamped * 255.0f + 0.5f);
}

// A square's colour as the shader writes it, components the format lacks 0 and alpha 1; or its alpha in all three.
static Rgb square_colour(const Spec *spec, uint32_t square, bool alpha)
{
	float scale;
	float bias;
	colour_scale(spec, &scale, &bias);
	float rgba[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
	for (uint32_t c = 0; c < spec->components; c++)
		rgba[c] = read_value(spec, stored_value(spec, square, c));
	if (alpha)
		rgba[0] = rgba[1] = rgba[2] = rgba[3];
	return (Rgb){ to_byte(rgba[0] * scale + bias), to_byte(rgba[1] * scale + bias), to_byte(rgba[2] * scale + bias) };
}

// The specs that place a triangle, in order, from the left.
static uint32_t triangle_left(uint32_t index)
{
	uint32_t place = 0;
	for (uint32_t i = 0; i < index; i++)
		place += specs[i].positions;
	return LEFT + place * TRIANGLE_STEP;
}

static void add_probe(Scene *scene, uint32_t spec, uint32_t x, uint32_t y, Rgb want, bool triangle)
{
	if (scene->probe_count < MAX_PROBES)
		scene->probes[scene->probe_count++] = (Probe){ spec, x, y, want, triangle };
}

static const VkClearValue clear = { .color = { .float32 = { 0.125f, 0.125f, 0.125f, 1.0f } } };
static const Rgb background = { 32, 32, 32 };
static const Rgb white = { 255, 255, 255 };

// Each square's middle in the colour and alpha cells; in a triangle's cell, below its diagonal where it is, and above
// where x and y swapped would put it.
static void make_probes(Scene *scene)
{
	for (uint32_t i = 0; i < SPECS; i++) {
		for (uint32_t square = 0; square < SQUARES; square++) {
			const uint32_t x = LEFT + i * CELL_STEP + square % 2 * SQUARE + SQUARE / 2;
			const uint32_t y = square / 2 * SQUARE + SQUARE / 2;
			add_probe(scene, i, x, COLOUR_TOP + y, square_colour(&specs[i], square, false), false);
			add_probe(scene, i, x, ALPHA_TOP + y, square_colour(&specs[i], square, true), false);
		}
		if (!specs[i].positions)
			continue;
		const uint32_t left = triangle_left(i);
		add_probe(scene, i, left + 48, TRIANGLE_TOP + 16, white, true);
		add_probe(scene, i, left + 56, TRIANGLE_TOP + 40, white, true);
		add_probe(scene, i, left + 16, TRIANGLE_TOP + 48, background, true);
		add_probe(scene, i, left + 40, TRIANGLE_TOP + 56, background, true);
	}
}

// A unit square's six corners in two triangles, and the triangle below its diagonal.
static const uint32_t square_corners[6][2] = { { 0, 0 }, { 1, 0 }, { 0, 1 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
static const uint32_t triangle_corners[TRIANGLE_VERTICES][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 } };

static void put_float4(uint8_t *out, float x, float y, float z, float w)
{
	const float values[4] = { x, y, z, w };
	memcpy(out, values, sizeof values);
}

// The colour cells' squares in clip space from four-float positions, each square's six vertices the colour of the
// square, then the triangle's positions in the spec's format with white four-float colours.
static void fill(const Spec *spec, uint32_t index, uint8_t *positions, uint8_t *colours)
{
	for (uint32_t draw = 0; draw < DRAWS; draw++) {
		uint8_t *const position = positions + (index * DRAWS + draw) * DRAW_VERTICES * POSITION_STRIDE;
		uint8_t *const colour = colours + (index * DRAWS + draw) * DRAW_VERTICES * COLOUR_STRIDE;
		for (uint32_t v = 0; v < DRAW_VERTICES; v++) {
			uint8_t *const p = position + v * POSITION_STRIDE;
			uint8_t *const c = colour + v * COLOUR_STRIDE + COLOUR_OFFSET;
			memset(p, 0, POSITION_STRIDE);
			memset(c, 0, COLOUR_BYTES_MAX);
			if (draw == DRAW_TRIANGLE) {
				if (v >= TRIANGLE_VERTICES || !spec->positions)
					continue;
				const uint32_t place[4] = { triangle_corners[v][0], triangle_corners[v][1], 0, 1 };
				for (uint32_t k = 0; k < spec->components; k++)
					put_component(spec, p, k, stored_unit(spec, place[k]));
				put_float4(c, 1.0f, 1.0f, 1.0f, 1.0f);
				continue;
			}
			const uint32_t square = v / 6;
			const uint32_t x = square % 2 * SQUARE + square_corners[v % 6][0] * SQUARE;
			const uint32_t y = square / 2 * SQUARE + square_corners[v % 6][1] * SQUARE;
			put_float4(p, (float)x, (float)y, 0.0f, 1.0f);
			for (uint32_t k = 0; k < spec->components; k++)
				put_component(spec, c, k, stored_value(spec, square, k));
		}
	}
}

// A HarnessCheck: every probe; misses are counted for their spec and check, and the first few logged.
static bool probes_hold(uint32_t address, const char *what, void *context)
{
	Scene *const scene = context;
	bool holds = true;
	for (uint32_t i = 0; i < scene->probe_count; i++) {
		const Probe *const probe = &scene->probes[i];
		const Rgb pixel = harness_pixel(address, probe->x, probe->y);
		if (harness_near(pixel, probe->want, TOLERANCE))
			continue;
		scene->missed[probe->spec][probe->triangle]++;
		if (scene->logged++ < MAX_MISSES_LOGGED)
			xsdk_log_write("vertex_formats: %s: %s %s at %u, %u held %02X %02X %02X, not %02X %02X %02X", what,
				specs[probe->spec].name, probe->triangle ? "triangle" : "colour", (unsigned)probe->x,
				(unsigned)probe->y, (unsigned)pixel.red, (unsigned)pixel.green, (unsigned)pixel.blue,
				(unsigned)probe->want.red, (unsigned)probe->want.green, (unsigned)probe->want.blue);
		holds = false;
	}
	return holds;
}

// A colour cell takes four-float positions and the spec's colours; a triangle the spec's positions and four-float
// colours.
static VkResult make_pipeline(const Harness *harness, Scene *scene, uint32_t index, uint32_t draw)
{
	const bool triangle = draw == DRAW_TRIANGLE;
	const VkVertexInputBindingDescription bindings[] = {
		{ 0, POSITION_STRIDE, VK_VERTEX_INPUT_RATE_VERTEX },
		{ 1, COLOUR_STRIDE, VK_VERTEX_INPUT_RATE_VERTEX },
	};
	const VkVertexInputAttributeDescription attributes[] = {
		{ .location = 0,
			.binding = 0,
			.format = triangle ? specs[index].format : VK_FORMAT_R32G32B32A32_SFLOAT,
			.offset = 0 },
		{ .location = 1,
			.binding = 1,
			.format = triangle ? VK_FORMAT_R32G32B32A32_SFLOAT : specs[index].format,
			.offset = COLOUR_OFFSET },
	};
	const HarnessPipeline spec = {
		.vertex_spirv = draw == DRAW_ALPHA ? vertex_alpha_vert_spirv : vertex_format_vert_spirv,
		.vertex_bytes = draw == DRAW_ALPHA ? VERTEX_ALPHA_VERT_SPIRV_BYTES : VERTEX_FORMAT_VERT_SPIRV_BYTES,
		.fragment_spirv = triangle_frag_spirv,
		.fragment_bytes = TRIANGLE_FRAG_SPIRV_BYTES,
		.bindings = bindings,
		.binding_count = 2,
		.attributes = attributes,
		.attribute_count = 2,
		.layout = scene->layout,
	};
	return harness_pipeline(harness, &spec, scene->render_pass, &scene->pipelines[index][draw]);
}

static bool build(const Harness *harness, Scene *scene)
{
	const VkPushConstantRange range = { VK_SHADER_STAGE_VERTEX_BIT, 0, 64 };
	const VkPipelineLayoutCreateInfo layout_info = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.pushConstantRangeCount = 1,
		.pPushConstantRanges = &range,
	};
	if (!harness_ok(
			"vkCreatePipelineLayout", vkCreatePipelineLayout(harness->device, &layout_info, NULL, &scene->layout)) ||
		!harness_ok("render pass", harness_render_pass(harness, &scene->render_pass, scene->framebuffers)))
		return false;
	for (uint32_t i = 0; i < SPECS; i++) {
		for (uint32_t draw = 0; draw < DRAWS; draw++) {
			if (draw == DRAW_TRIANGLE && !specs[i].positions)
				continue;
			if (!harness_ok(specs[i].name, make_pipeline(harness, scene, i, draw)))
				return false;
		}
	}
	static uint8_t positions[POSITION_BYTES];
	static uint8_t colours[COLOUR_BYTES];
	for (uint32_t i = 0; i < SPECS; i++)
		fill(&specs[i], i, positions, colours);
	return harness_ok("positions",
			   harness_buffer(harness, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, positions, sizeof positions,
				   &scene->positions, &scene->position_memory)) &&
		harness_ok("colours",
			harness_buffer(harness, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, colours, sizeof colours, &scene->colours,
				&scene->colour_memory));
}

// Places a draw at its pixels, its positions a pixel each or its triangle a unit to 64 pixels, its colours scaled.
static void push(VkCommandBuffer commands, VkPipelineLayout layout, uint32_t index, uint32_t draw)
{
	const float half_width = HARNESS_WIDTH / 2.0f;
	const float half_height = HARNESS_HEIGHT / 2.0f;
	const bool triangle = draw == DRAW_TRIANGLE;
	const float unit = triangle ? TRIANGLE_SIDE : 1.0f;
	const float left = triangle ? (float)triangle_left(index) : (float)(LEFT + index * CELL_STEP);
	const float top = triangle ? TRIANGLE_TOP : draw == DRAW_ALPHA ? ALPHA_TOP : COLOUR_TOP;
	float scale = 1.0f;
	float bias = 0.0f;
	if (!triangle)
		colour_scale(&specs[index], &scale, &bias);
	const float values[16] = {
		unit / half_width,
		unit / half_height,
		1.0f,
		1.0f,
		left / half_width - 1.0f,
		top / half_height - 1.0f,
		0.0f,
		0.0f,
		scale,
		scale,
		scale,
		scale,
		bias,
		bias,
		bias,
		bias,
	};
	vkCmdPushConstants(commands, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof values, values);
}

static VkResult record(const Harness *harness, Scene *scene)
{
	VkResult result = harness_command_buffers(harness, scene->commands);
	for (uint32_t i = 0; i < harness->image_count && result == VK_SUCCESS; i++) {
		const VkCommandBuffer commands = scene->commands[i];
		const VkRenderPassBeginInfo pass = {
			.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
			.renderPass = scene->render_pass,
			.framebuffer = scene->framebuffers[i],
			.renderArea = { { 0, 0 }, { HARNESS_WIDTH, HARNESS_HEIGHT } },
			.clearValueCount = 1,
			.pClearValues = &clear,
		};
		vkCmdBeginRenderPass(commands, &pass, VK_SUBPASS_CONTENTS_INLINE);
		for (uint32_t index = 0; index < SPECS; index++) {
			for (uint32_t draw = 0; draw < DRAWS; draw++) {
				if (draw == DRAW_TRIANGLE && !specs[index].positions)
					continue;
				const VkBuffer buffers[2] = { scene->positions, scene->colours };
				const VkDeviceSize offsets[2] = { (index * DRAWS + draw) * DRAW_VERTICES * POSITION_STRIDE,
					(index * DRAWS + draw) * DRAW_VERTICES * COLOUR_STRIDE };
				vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, scene->pipelines[index][draw]);
				vkCmdBindVertexBuffers(commands, 0, 2, buffers, offsets);
				push(commands, scene->layout, index, draw);
				vkCmdDraw(commands, draw == DRAW_TRIANGLE ? TRIANGLE_VERTICES : CELL_VERTICES, 1, 0, 0);
			}
		}
		vkCmdEndRenderPass(commands);
		result = vkEndCommandBuffer(commands);
	}
	return result;
}

static void check_drawn(const Scene *scene, uint32_t frames)
{
	check("vertex_formats: 30 frames drawn and presented", frames == FRAMES);
	char name[160];
	bool placed = frames == FRAMES;
	for (uint32_t i = 0; i < SPECS; i++) {
		(void)snprintf(name, sizeof name,
			"vertex_formats: %s colours drawn, each square's red, green, blue and alpha as stored", specs[i].name);
		check(name, frames == FRAMES && !scene->missed[i][0]);
		placed = placed && !scene->missed[i][1];
	}
	check(
		"vertex_formats: positions of bytes, shorts and halves place their triangles, x, y, z and w in place", placed);
}

static void check_properties(const Harness *harness)
{
	bool reported = true;
	for (uint32_t i = 0; i < SPECS; i++) {
		VkFormatProperties properties;
		vkGetPhysicalDeviceFormatProperties(harness->physical_device, specs[i].format, &properties);
		reported = reported && properties.bufferFeatures == VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT;
	}
	const VkFormat refused[] = { VK_FORMAT_R8G8B8A8_UINT, VK_FORMAT_R16G16_SINT, VK_FORMAT_R8G8_UNORM,
		VK_FORMAT_R16G16B16_SFLOAT };
	for (uint32_t i = 0; i < sizeof refused / sizeof refused[0]; i++) {
		VkFormatProperties properties;
		vkGetPhysicalDeviceFormatProperties(harness->physical_device, refused[i], &properties);
		reported = reported && !properties.bufferFeatures;
	}
	check("vertex_formats: each is a vertex buffer format, and the integer formats and those under 4 bytes aren't",
		reported);
}

static void destroy(const Harness *harness, Scene *scene)
{
	const VkDevice device = harness->device;
	vkFreeCommandBuffers(device, harness->pool, harness->image_count, scene->commands);
	vkDestroyBuffer(device, scene->positions, NULL);
	vkFreeMemory(device, scene->position_memory, NULL);
	vkDestroyBuffer(device, scene->colours, NULL);
	vkFreeMemory(device, scene->colour_memory, NULL);
	for (uint32_t i = 0; i < SPECS; i++) {
		for (uint32_t draw = 0; draw < DRAWS; draw++)
			vkDestroyPipeline(device, scene->pipelines[i][draw], NULL);
	}
	vkDestroyPipelineLayout(device, scene->layout, NULL);
	harness_destroy_render_pass(harness, scene->render_pass, scene->framebuffers);
}

void test_vertex_formats(const Harness *harness)
{
	static Scene scene;
	scene = (Scene){ .render_pass = VK_NULL_HANDLE };
	make_probes(&scene);
	const bool built = build(harness, &scene);
	check("vertex_formats: a pipeline for each format's colours, its alpha and its positions made", built);
	const bool recorded = built && harness->swapchain && harness_ok("recording", record(harness, &scene));
	uint32_t misses = 0;
	const uint32_t frames =
		recorded ? harness_frames(harness, scene.commands, FRAMES, probes_hold, &scene, &misses) : 0;
	xsdk_log_write(
		"vertex_formats: %u probes a frame, %u frames held a wrong one", (unsigned)scene.probe_count, (unsigned)misses);
	check_drawn(&scene, frames);
	check_properties(harness);
	destroy(harness, &scene);
}
