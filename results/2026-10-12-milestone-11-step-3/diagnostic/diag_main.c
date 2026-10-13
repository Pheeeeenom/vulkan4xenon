#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "diag_shaders.h"
#include "harness.h"
#include "xsdk/log.h"

// Milestone 11, step 3's diagnostic title: why band.frag read index 0 at every column on the console. Each scene
// separates one thing: a pixel shader's a0 read with its index from an interpolant (D1, and D1b with the execs'
// addressing mode a0), and gl_FragCoord written out as a colour, with no a0 (D2 as band.frag has it, D2b with no
// interpolator in, D3 with the vertex shader exporting the interpolator the pixel parameters land in, D4 with
// SQ_CONTEXT_MISC.SC_OUTPUT_SCREEN_XY set).

enum {
	IMAGES = 2,
	FRAMES = 10,
	BANDS = 10,
	BAND_PIXELS = 128,
	QUAD_VERTICES = 6,
	TOLERANCE = 2,
};

static int passed_count;
static int failed_count;

void check(const char *name, bool passed)
{
	xsdk_log_write("%s: %s", passed ? "PASS" : "FAIL", name);
	if (passed)
		passed_count++;
	else
		failed_count++;
}

typedef struct {
	float position[4];
	float color[4];
	float band;
} Vertex;

static const float unit_quad[QUAD_VERTICES][2] = { { 0, 0 }, { 1, 0 }, { 0, 1 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };

static const VkClearValue clear = { .color = { .float32 = { 0.0f, 0.0f, 0.0f, 1.0f } } };

static Rgb band_color(uint32_t band)
{
	return (Rgb){ 20 + band * 23, 230 - band * 21, band % 2 ? 200 : 40 };
}

typedef struct {
	const char *name;
	const uint32_t *vertex_spirv;
	uint32_t vertex_bytes;
	const uint32_t *fragment_spirv;
	uint32_t fragment_bytes;
	bool bands;          // ten quads, one a band, each naming its band; else the whole screen in white
	float blue;          // the coordinate scenes' blue
} DiagScene;

typedef struct {
	const DiagScene *scene;
	uint32_t misses;
	bool log;
} Context;

static const VkVertexInputBindingDescription binding = { 0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX };
// The band scenes' vertex shader reads the position and band; the others the position and colour.
static const VkVertexInputAttributeDescription band_attributes[] = {
	{ .location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = 0 },
	{ .location = 2, .binding = 0, .format = VK_FORMAT_R32_SFLOAT, .offset = offsetof(Vertex, band) },
};
static const VkVertexInputAttributeDescription colour_attributes[] = {
	{ .location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = 0 },
	{ .location = 1, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = offsetof(Vertex, color) },
};

static const uint32_t sample_x[] = { 0, 1, 64, 127, 128, 129, 640, 1151, 1152, 1279 };
static const uint32_t sample_y[] = { 0, 1, 100, 360, 719 };

// Logs what the image holds at each sample, and counts the samples off what the scene should draw.
static bool image_holds(uint32_t address, const char *what, void *context)
{
	Context *const c = context;
	const DiagScene *const s = c->scene;
	uint32_t misses = 0;
	for (uint32_t j = 0; j < sizeof sample_y / sizeof sample_y[0]; j++) {
		for (uint32_t i = 0; i < sizeof sample_x / sizeof sample_x[0]; i++) {
			const uint32_t x = sample_x[i];
			const uint32_t y = sample_y[j];
			const Rgb pixel = harness_pixel(address, x, y);
			Rgb want;
			if (s->bands) {
				want = band_color(x / BAND_PIXELS);
			} else {
				want = (Rgb){ (uint32_t)((x + 0.5f) / 1280.0f * 255.0f + 0.5f),
					(uint32_t)((y + 0.5f) / 720.0f * 255.0f + 0.5f), (uint32_t)(s->blue * 255.0f + 0.5f) };
			}
			const bool near = harness_near(pixel, want, TOLERANCE);
			misses += !near;
			if (c->log && (j == 2 || j == 3 || !near))
				xsdk_log_write("%s: %s: %u, %u held %02X %02X %02X, want %02X %02X %02X%s", s->name, what,
					(unsigned)x, (unsigned)y, (unsigned)pixel.red, (unsigned)pixel.green, (unsigned)pixel.blue,
					(unsigned)want.red, (unsigned)want.green, (unsigned)want.blue, near ? "" : " (off)");
		}
	}
	c->misses = misses;
	return misses == 0;
}

static void run_scene(const Harness *harness, const DiagScene *s)
{
	VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
	VkPipelineLayout layout = VK_NULL_HANDLE;
	VkDescriptorPool pool = VK_NULL_HANDLE;
	VkDescriptorSet set = VK_NULL_HANDLE;
	VkBuffer uniforms = VK_NULL_HANDLE;
	VkDeviceMemory uniform_memory = VK_NULL_HANDLE;
	float block[BANDS][4];
	for (uint32_t b = 0; b < BANDS; b++) {
		const Rgb color = band_color(b);
		block[b][0] = color.red / 255.0f;
		block[b][1] = color.green / 255.0f;
		block[b][2] = color.blue / 255.0f;
		block[b][3] = 1.0f;
	}
	const VkDescriptorSetLayoutBinding uniform_binding = { 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
		VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
	const VkDescriptorSetLayoutCreateInfo set_info = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = 1,
		.pBindings = &uniform_binding };
	bool ok = harness_ok("set layout", vkCreateDescriptorSetLayout(harness->device, &set_info, NULL, &set_layout));
	const VkPipelineLayoutCreateInfo layout_info = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = 1,
		.pSetLayouts = &set_layout };
	ok = ok && harness_ok("pipeline layout", vkCreatePipelineLayout(harness->device, &layout_info, NULL, &layout));
	const VkDescriptorPoolSize size = { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1 };
	const VkDescriptorPoolCreateInfo pool_info = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.maxSets = 1,
		.poolSizeCount = 1,
		.pPoolSizes = &size };
	ok = ok && harness_ok("pool", vkCreateDescriptorPool(harness->device, &pool_info, NULL, &pool));
	const VkDescriptorSetAllocateInfo allocate_info = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool = pool,
		.descriptorSetCount = 1,
		.pSetLayouts = &set_layout };
	ok = ok && harness_ok("set", vkAllocateDescriptorSets(harness->device, &allocate_info, &set));
	ok = ok &&
		harness_ok("uniforms", harness_buffer(harness, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, block, sizeof block,
				&uniforms, &uniform_memory));
	if (ok) {
		const VkDescriptorBufferInfo buffer = { uniforms, 0, sizeof block };
		const VkWriteDescriptorSet write = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstSet = set,
			.descriptorCount = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
			.pBufferInfo = &buffer };
		vkUpdateDescriptorSets(harness->device, 1, &write, 0, NULL);
	}

	Vertex vertices[BANDS * QUAD_VERTICES];
	const uint32_t quads = s->bands ? BANDS : 1;
	for (uint32_t q = 0; q < quads; q++) {
		for (uint32_t v = 0; v < QUAD_VERTICES; v++) {
			const float x = s->bands ? (q + unit_quad[v][0]) * (2.0f / BANDS) - 1.0f : unit_quad[v][0] * 2.0f - 1.0f;
			vertices[q * QUAD_VERTICES + v] = (Vertex){ { x, unit_quad[v][1] * 2.0f - 1.0f, 0.5f, 1.0f },
				{ 1.0f, 1.0f, 1.0f, 1.0f }, (float)q };
		}
	}
	VkRenderPass render_pass = VK_NULL_HANDLE;
	VkFramebuffer framebuffers[HARNESS_MAX_IMAGES];
	VkPipeline pipeline = VK_NULL_HANDLE;
	VkBuffer vertex_buffer = VK_NULL_HANDLE;
	VkDeviceMemory vertex_memory = VK_NULL_HANDLE;
	const HarnessPipeline spec = {
		.vertex_spirv = s->vertex_spirv,
		.vertex_bytes = s->vertex_bytes,
		.fragment_spirv = s->fragment_spirv,
		.fragment_bytes = s->fragment_bytes,
		.bindings = &binding,
		.binding_count = 1,
		.attributes = s->bands ? band_attributes : colour_attributes,
		.attribute_count = 2,
		.layout = layout,
	};
	ok = ok && harness_ok("render pass", harness_render_pass(harness, &render_pass, framebuffers));
	ok = ok && harness_ok("pipeline", harness_pipeline(harness, &spec, render_pass, &pipeline));
	ok = ok &&
		harness_ok("vertices", harness_buffer(harness, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, vertices,
				quads * QUAD_VERTICES * sizeof(Vertex), &vertex_buffer, &vertex_memory));
	VkCommandBuffer commands[HARNESS_MAX_IMAGES];
	ok = ok && harness_ok("command buffers", harness_command_buffers(harness, commands));
	for (uint32_t i = 0; ok && i < harness->image_count; i++) {
		const VkRenderPassBeginInfo pass = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
			.renderPass = render_pass,
			.framebuffer = framebuffers[i],
			.renderArea = { { 0, 0 }, { HARNESS_WIDTH, HARNESS_HEIGHT } },
			.clearValueCount = 1,
			.pClearValues = &clear };
		const VkDeviceSize offset = 0;
		vkCmdBeginRenderPass(commands[i], &pass, VK_SUBPASS_CONTENTS_INLINE);
		vkCmdBindPipeline(commands[i], VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
		vkCmdBindVertexBuffers(commands[i], 0, 1, &vertex_buffer, &offset);
		vkCmdBindDescriptorSets(commands[i], VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, NULL);
		vkCmdDraw(commands[i], quads * QUAD_VERTICES, 1, 0, 0);
		vkCmdEndRenderPass(commands[i]);
		ok = harness_ok("end", vkEndCommandBuffer(commands[i]));
	}
	Context context = { s, 0, false };
	uint32_t misses = 0;
	const uint32_t frames = ok ? harness_frames(harness, commands, FRAMES, image_holds, &context, &misses) : 0;
	context.log = true;
	const bool shown = ok && harness_screen(harness, image_holds, &context);
	xsdk_log_write("%s: %u frames, %u missed; on screen %u samples off", s->name, (unsigned)frames, (unsigned)misses,
		(unsigned)context.misses);
	check(s->name, ok && frames == FRAMES && misses == 0 && shown);
	if (ok)
		vkFreeCommandBuffers(harness->device, harness->pool, harness->image_count, commands);
	vkDestroyBuffer(harness->device, vertex_buffer, NULL);
	vkFreeMemory(harness->device, vertex_memory, NULL);
	vkDestroyPipeline(harness->device, pipeline, NULL);
	if (render_pass)
		harness_destroy_render_pass(harness, render_pass, framebuffers);
	vkDestroyDescriptorPool(harness->device, pool, NULL);
	vkDestroyBuffer(harness->device, uniforms, NULL);
	vkFreeMemory(harness->device, uniform_memory, NULL);
	vkDestroyPipelineLayout(harness->device, layout, NULL);
	vkDestroyDescriptorSetLayout(harness->device, set_layout, NULL);
}

static const DiagScene scenes[] = {
	{ "D1 a0 in a pixel shader, index from an interpolant", diag_index_vert_spirv, DIAG_INDEX_VERT_SPIRV_BYTES,
		diag_index_frag_spirv, DIAG_INDEX_FRAG_SPIRV_BYTES, true, 0 },
	{ "D1b the same, execs' addressing mode a0", diag_index_vert_spirv, DIAG_INDEX_VERT_SPIRV_BYTES,
		diag_index_cfa0_frag_spirv, DIAG_INDEX_CFA0_FRAG_SPIRV_BYTES, true, 0 },
	{ "D2 gl_FragCoord as colour, parameters in r1", diag_triangle_vert_spirv, DIAG_TRIANGLE_VERT_SPIRV_BYTES,
		diag_coord_frag_spirv, DIAG_COORD_FRAG_SPIRV_BYTES, false, 0.5f },
	{ "D2b gl_FragCoord as colour, no interpolator in, parameters in r0", diag_triangle_vert_spirv,
		DIAG_TRIANGLE_VERT_SPIRV_BYTES, diag_coord_noin_frag_spirv, DIAG_COORD_NOIN_FRAG_SPIRV_BYTES, false, 0.125f },
	{ "D3 gl_FragCoord as colour, vertex shader exporting interpolator 1", diag_two_out_vert_spirv,
		DIAG_TWO_OUT_VERT_SPIRV_BYTES, diag_coord_extra_frag_spirv, DIAG_COORD_EXTRA_FRAG_SPIRV_BYTES, false, 0.75f },
	{ "D4 gl_FragCoord as colour, SC_OUTPUT_SCREEN_XY set", diag_triangle_vert_spirv, DIAG_TRIANGLE_VERT_SPIRV_BYTES,
		diag_coord_scxy_frag_spirv, DIAG_COORD_SCXY_FRAG_SPIRV_BYTES, false, 0.25f },
};

int main(void)
{
	xsdk_log_write("vulkan4xenon diagnostic title: milestone 11, step 3");
	Harness harness = { .instance = VK_NULL_HANDLE };
	const bool made = harness_create_device(&harness) == VK_SUCCESS &&
		harness_create_swapchain(&harness, IMAGES) == VK_SUCCESS;
	check("setup: device and swapchain made", made);
	for (uint32_t i = 0; made && i < sizeof scenes / sizeof scenes[0]; i++)
		run_scene(&harness, &scenes[i]);
	harness_destroy(&harness);
	xsdk_log_write("tests: %d passed, %d failed", passed_count, failed_count);
	return failed_count;
}
