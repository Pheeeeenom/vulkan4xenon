#include <stdbool.h>
#include <stdint.h>

#include "harness.h"
#include "shaders.h"
#include "tests.h"
#include "tiled_index.h"
#include "xsdk/log.h"

// A D24S8 attachment made to be sampled, in each frame as Dusklight's EFB depth (milestone 14): a pass draws depth and
// stencil into it and keeps it as an attachment; a smaller pass draws with depth of its own, as the bloom does; and a
// pass loads it and leaves it to be read, so its memory holds each texel's depth above its stencil. The pass after
// draws it on screen, its depth in red. Then a pass that keeps it as an attachment, which leaves its memory alone.

enum {
	FRAMES = 30,
	TOLERANCE = 2,
	// The depth image and the colour beside it, rows whole tiles of 32 apart.
	TARGET_WIDTH = 256,
	TARGET_HEIGHT = 128,
	// The smaller pass's: its 16 tiles of eDRAM, ending at the top as milestone 4 placed every depth, would cover the
	// depth image's last 16, rows 64 to 127 (xsdk:src/gpu_draw.c, tiles_across).
	OTHER_WIDTH = 160,
	OTHER_HEIGHT = 128,
	// Where the screen's pass draws the depth image, twice its size.
	SHOWN_X = 64,
	SHOWN_Y = 64,
	SHOWN_SCALE = 2,
	VERTEX_FLOATS = 6,
	STRIDE_BYTES = VERTEX_FLOATS * 4,
	UV_OFFSET_BYTES = 16,
	QUAD_VERTICES = 6,
	DEPTH_24_MAX = 0xFFFFFF,
	// D24's rounding, as xsdk's check of the resolve allows (xsdk:tests/test_depth.c).
	DEPTH_24_TOLERANCE = 2,
	WORD_MISSES_LOGGED = 4,
	QUADS = 3,
	POINTS = 6,
	SCREEN_PROBES = POINTS + 1,
};

#define RGBA(r, g, b) { (r) / 255.0f, (g) / 255.0f, (b) / 255.0f, 1.0f }

typedef struct {
	HarnessQuad quad;
	uint32_t stencil;
} DepthQuad;

// Depth clears to 1 and stencil to 0; each quad tests less-than, writes depth, and sets stencil where it passes. The
// far quad comes second, so the near one keeps their overlap; the middle one reaches the last column and row.
static const DepthQuad quads[QUADS] = {
	{ { 16, 16, 96, 48, 0.25f, RGBA(224, 64, 64), false }, 0x5A },
	{ { 64, 40, 112, 60, 0.75f, RGBA(64, 224, 64), false }, 0xA5 },
	{ { 192, 16, 64, 112, 0.5f, RGBA(64, 64, 224), false }, 0x3C },
};

typedef struct {
	const char *name;
	uint32_t x;
	uint32_t y;
	float depth;
	uint32_t stencil;
} DepthPoint;

static const DepthPoint points[POINTS] = {
	{ "the near quad", 40, 30, 0.25f, 0x5A },
	{ "the near quad over the far one", 96, 52, 0.25f, 0x5A },
	{ "the far quad", 150, 90, 0.75f, 0xA5 },
	{ "the middle quad", 216, 40, 0.5f, 0x3C },
	{ "the middle quad's last texel", TARGET_WIDTH - 1, TARGET_HEIGHT - 1, 0.5f, 0x3C },
	{ "the clear", 8, 120, 1.0f, 0 },
};

static const VkClearValue target_clears[2] = {
	{ .color = { .float32 = RGBA(0, 96, 128) } },
	{ .depthStencil = { 1.0f, 0 } },
};
static const VkClearValue screen_clear = { .color = { .float32 = RGBA(24, 24, 24) } };
static const VkClearValue other_clears[2] = {
	{ .color = { .float32 = RGBA(200, 200, 0) } },
	{ .depthStencil = { 0.9f, 0x77 } },
};
// Over all the smaller pass's target, through the first quad's pipeline.
static const DepthQuad other_quad = { { 0, 0, OTHER_WIDTH, OTHER_HEIGHT, 0.05f, RGBA(0, 200, 200), false }, 0x5A };
// The kept pass's quad, over the whole target, nearer than every other.
static const HarnessQuad kept_quad = { 0, 0, TARGET_WIDTH, TARGET_HEIGHT, 0.1f, RGBA(250, 250, 250), false };

// Clip space, then texture coordinates, over where the depth image is shown.
#define SHOWN_LEFT   ((float)SHOWN_X / (HARNESS_WIDTH / 2) - 1.0f)
#define SHOWN_RIGHT  ((float)(SHOWN_X + SHOWN_SCALE * TARGET_WIDTH) / (HARNESS_WIDTH / 2) - 1.0f)
#define SHOWN_TOP    ((float)SHOWN_Y / (HARNESS_HEIGHT / 2) - 1.0f)
#define SHOWN_BOTTOM ((float)(SHOWN_Y + SHOWN_SCALE * TARGET_HEIGHT) / (HARNESS_HEIGHT / 2) - 1.0f)

static const float shown_quad[QUAD_VERTICES][VERTEX_FLOATS] = {
	{ SHOWN_LEFT, SHOWN_TOP, 0.0f, 1.0f, 0.0f, 0.0f },
	{ SHOWN_RIGHT, SHOWN_TOP, 0.0f, 1.0f, 1.0f, 0.0f },
	{ SHOWN_LEFT, SHOWN_BOTTOM, 0.0f, 1.0f, 0.0f, 1.0f },
	{ SHOWN_RIGHT, SHOWN_TOP, 0.0f, 1.0f, 1.0f, 0.0f },
	{ SHOWN_RIGHT, SHOWN_BOTTOM, 0.0f, 1.0f, 1.0f, 1.0f },
	{ SHOWN_LEFT, SHOWN_BOTTOM, 0.0f, 1.0f, 0.0f, 1.0f },
};

static const VkVertexInputBindingDescription shown_binding = { 0, STRIDE_BYTES, VK_VERTEX_INPUT_RATE_VERTEX };
static const VkVertexInputAttributeDescription shown_attributes[] = {
	{ .location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = 0 },
	{ .location = 1, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = UV_OFFSET_BYTES },
};

// The first pass's depth is sampled by the second, and the next frame's first pass waits for that.
static const VkSubpassDependency dependencies[2] = {
	{ VK_SUBPASS_EXTERNAL, 0, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
		VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
		VK_ACCESS_SHADER_READ_BIT,
		VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, 0 },
	{ 0, VK_SUBPASS_EXTERNAL, VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
		VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, 0 },
};

typedef struct {
	VkImage color;
	VkDeviceMemory color_memory;
	VkImageView color_view;
	VkImage depth;
	VkDeviceMemory depth_memory;
	// Depth and stencil, for the framebuffer; depth alone, to sample.
	VkImageView depth_view;
	VkImageView sampled_view;
	VkImage other_color;
	VkDeviceMemory other_color_memory;
	VkImageView other_color_view;
	VkImage other_depth;
	VkDeviceMemory other_depth_memory;
	VkImageView other_depth_view;
	// Cleared and kept as an attachment, or loaded and left to be read; one framebuffer serves both.
	VkRenderPass kept_pass;
	VkRenderPass load_pass;
	VkFramebuffer target;
	VkRenderPass other_pass;
	VkFramebuffer other;
	VkRenderPass screen_pass;
	VkFramebuffer screens[HARNESS_MAX_IMAGES];
	VkPipelineLayout push_layout;
	VkPipeline quad_pipelines[QUADS];
	VkDescriptorSetLayout set_layout;
	VkPipelineLayout texture_layout;
	VkPipeline textured;
	VkSampler sampler;
	VkDescriptorPool pool;
	VkDescriptorSet set;
	VkBuffer unit_quad;
	VkDeviceMemory unit_quad_memory;
	VkBuffer shown;
	VkDeviceMemory shown_memory;
	VkCommandBuffer commands[HARNESS_MAX_IMAGES];
	volatile const uint32_t *words;
	bool words_logged;
	uint32_t word_misses_logged;
	uint32_t word_misses;
	uint32_t screen_misses;
	HarnessProbe probes[SCREEN_PROBES];
	HarnessProbes screen_check;
} Scene;

static uint32_t depth_24(float depth)
{
	return (uint32_t)(depth * (double)DEPTH_24_MAX + 0.5);
}

static uint32_t difference(uint32_t a, uint32_t b)
{
	return a > b ? a - b : b - a;
}

// Each point lands at its texel's centre where the second pass draws the image; the clear colour beside it.
static void make_probes(Scene *scene)
{
	for (uint32_t i = 0; i < POINTS; i++) {
		const DepthPoint *const p = &points[i];
		scene->probes[i] = (HarnessProbe){ p->name, SHOWN_X + p->x * SHOWN_SCALE + SHOWN_SCALE / 2,
			SHOWN_Y + p->y * SHOWN_SCALE + SHOWN_SCALE / 2, { (uint32_t)(p->depth * 255.0f + 0.5f), 0, 0 }, 0 };
	}
	scene->probes[POINTS] = (HarnessProbe){ "the screen's clear colour", 20, 20, { 24, 24, 24 }, 0 };
	scene->screen_check = (HarnessProbes){ "sampled depth", scene->probes, SCREEN_PROBES, TOLERANCE, 0 };
}

// The depth image's words as the CPU reads them, every point's logged once and then any miss.
static bool frame_holds_words(Scene *scene, const char *what)
{
	bool words = true;
	for (uint32_t i = 0; i < POINTS; i++) {
		const DepthPoint *const p = &points[i];
		const uint32_t word = scene->words[tiled_index(p->x, p->y, TARGET_WIDTH)];
		const uint32_t depth = depth_24(p->depth);
		const bool right = difference(word >> 8, depth) <= DEPTH_24_TOLERANCE && (word & 0xFF) == p->stencil;
		if (!scene->words_logged || (!right && scene->word_misses_logged < WORD_MISSES_LOGGED)) {
			scene->word_misses_logged += !right;
			xsdk_log_write("sampled depth: %s, %s at (%u, %u): %08X, for depth %06X above stencil %02X", what, p->name,
				(unsigned)p->x, (unsigned)p->y, (unsigned)word, (unsigned)depth, (unsigned)p->stencil);
		}
		words = words && right;
	}
	scene->words_logged = true;
	scene->word_misses += !words;
	return words;
}

// A HarnessCheck: the depth image's words, then the screen.
static bool frame_holds(uint32_t address, const char *what, void *context)
{
	Scene *const scene = context;
	const bool words = frame_holds_words(scene, what);
	const bool shown = harness_probes_hold(address, what, &scene->screen_check);
	scene->screen_misses += !shown;
	return words && shown;
}

// Colour and depth both cleared, or both loaded as the pass before left them; depth left as depth_after says.
static VkResult create_pass(
	const Harness *harness, VkAttachmentLoadOp load, VkImageLayout depth_after, VkRenderPass *render_pass)
{
	const bool loads = load == VK_ATTACHMENT_LOAD_OP_LOAD;
	const VkAttachmentDescription attachments[2] = {
		{ .format = VK_FORMAT_B8G8R8A8_UNORM,
			.samples = VK_SAMPLE_COUNT_1_BIT,
			.loadOp = load,
			.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
			.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
			.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
			.initialLayout = loads ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
			.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
		{ .format = VK_FORMAT_D24_UNORM_S8_UINT,
			.samples = VK_SAMPLE_COUNT_1_BIT,
			.loadOp = load,
			.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
			.stencilLoadOp = load,
			.stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE,
			.initialLayout = loads ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
			.finalLayout = depth_after },
	};
	const VkAttachmentReference color = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
	const VkAttachmentReference depth = { 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
	const VkSubpassDescription subpass = {
		.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
		.colorAttachmentCount = 1,
		.pColorAttachments = &color,
		.pDepthStencilAttachment = &depth,
	};
	const VkRenderPassCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
		.attachmentCount = 2,
		.pAttachments = attachments,
		.subpassCount = 1,
		.pSubpasses = &subpass,
		.dependencyCount = 2,
		.pDependencies = dependencies,
	};
	return vkCreateRenderPass(harness->device, &info, NULL, render_pass);
}

static VkResult create_framebuffer(const Harness *harness, VkRenderPass render_pass, VkImageView color,
	VkImageView depth, uint32_t width, uint32_t height, VkFramebuffer *framebuffer)
{
	const VkImageView views[2] = { color, depth };
	const VkFramebufferCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
		.renderPass = render_pass,
		.attachmentCount = 2,
		.pAttachments = views,
		.width = width,
		.height = height,
		.layers = 1,
	};
	return vkCreateFramebuffer(harness->device, &info, NULL, framebuffer);
}

static VkResult create_passes(const Harness *harness, Scene *scene)
{
	VkResult result = create_pass(
		harness, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, &scene->kept_pass);
	if (result == VK_SUCCESS)
		result = create_pass(
			harness, VK_ATTACHMENT_LOAD_OP_LOAD, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, &scene->load_pass);
	if (result == VK_SUCCESS)
		result = create_pass(
			harness, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, &scene->other_pass);
	if (result == VK_SUCCESS)
		result = create_framebuffer(harness, scene->kept_pass, scene->color_view, scene->depth_view, TARGET_WIDTH,
			TARGET_HEIGHT, &scene->target);
	if (result == VK_SUCCESS)
		result = create_framebuffer(harness, scene->other_pass, scene->other_color_view, scene->other_depth_view,
			OTHER_WIDTH, OTHER_HEIGHT, &scene->other);
	return result;
}

// The device reports D24S8 as sampled, and an image of it both sampled and an attachment.
static bool reports_sampled_depth(const Harness *harness)
{
	VkFormatProperties properties;
	vkGetPhysicalDeviceFormatProperties(harness->physical_device, VK_FORMAT_D24_UNORM_S8_UINT, &properties);
	VkImageFormatProperties image_properties;
	const VkResult result = vkGetPhysicalDeviceImageFormatProperties(harness->physical_device,
		VK_FORMAT_D24_UNORM_S8_UINT, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
		VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 0, &image_properties);
	return (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) && result == VK_SUCCESS;
}

static VkResult create_images(const Harness *harness, Scene *scene)
{
	VkResult result = harness_image(harness, VK_FORMAT_B8G8R8A8_UNORM, TARGET_WIDTH, TARGET_HEIGHT,
		VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, &scene->color, &scene->color_memory,
		&scene->color_view);
	if (result == VK_SUCCESS)
		result = harness_image(harness, VK_FORMAT_D24_UNORM_S8_UINT, TARGET_WIDTH, TARGET_HEIGHT,
			VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, &scene->depth,
			&scene->depth_memory, &scene->depth_view);
	const VkImageViewCreateInfo view_info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.image = scene->depth,
		.viewType = VK_IMAGE_VIEW_TYPE_2D,
		.format = VK_FORMAT_D24_UNORM_S8_UINT,
		.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 },
	};
	if (result == VK_SUCCESS)
		result = vkCreateImageView(harness->device, &view_info, NULL, &scene->sampled_view);
	if (result == VK_SUCCESS)
		result = harness_image(harness, VK_FORMAT_B8G8R8A8_UNORM, OTHER_WIDTH, OTHER_HEIGHT,
			VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, &scene->other_color,
			&scene->other_color_memory, &scene->other_color_view);
	if (result == VK_SUCCESS)
		result = harness_image(harness, VK_FORMAT_D24_UNORM_S8_UINT, OTHER_WIDTH, OTHER_HEIGHT,
			VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, &scene->other_depth, &scene->other_depth_memory,
			&scene->other_depth_view);
	return result;
}

static VkResult create_quad_pipeline(const Harness *harness, Scene *scene, uint32_t q)
{
	const VkStencilOpState stencil = {
		.failOp = VK_STENCIL_OP_KEEP,
		.passOp = VK_STENCIL_OP_REPLACE,
		.depthFailOp = VK_STENCIL_OP_KEEP,
		.compareOp = VK_COMPARE_OP_ALWAYS,
		.compareMask = 0xFF,
		.writeMask = 0xFF,
		.reference = quads[q].stencil,
	};
	const VkPipelineDepthStencilStateCreateInfo depth_stencil = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
		.depthTestEnable = VK_TRUE,
		.depthWriteEnable = VK_TRUE,
		.depthCompareOp = VK_COMPARE_OP_LESS,
		.stencilTestEnable = VK_TRUE,
		.front = stencil,
		.back = stencil,
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
		.layout = scene->push_layout,
		.depth_stencil = &depth_stencil,
		.dynamic_viewport = true,
	};
	return harness_pipeline(harness, &spec, scene->kept_pass, &scene->quad_pipelines[q]);
}

static VkResult create_textured(const Harness *harness, Scene *scene)
{
	const VkDescriptorSetLayoutBinding binding = {
		.binding = 0,
		.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		.descriptorCount = 1,
		.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
	};
	const VkDescriptorSetLayoutCreateInfo set_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = 1,
		.pBindings = &binding,
	};
	VkResult result = vkCreateDescriptorSetLayout(harness->device, &set_info, NULL, &scene->set_layout);
	const VkPipelineLayoutCreateInfo layout_info = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = 1,
		.pSetLayouts = &scene->set_layout,
	};
	if (result == VK_SUCCESS)
		result = vkCreatePipelineLayout(harness->device, &layout_info, NULL, &scene->texture_layout);
	const HarnessPipeline spec = {
		.vertex_spirv = texture_vert_spirv,
		.vertex_bytes = TEXTURE_VERT_SPIRV_BYTES,
		.fragment_spirv = texture_frag_spirv,
		.fragment_bytes = TEXTURE_FRAG_SPIRV_BYTES,
		.bindings = &shown_binding,
		.binding_count = 1,
		.attributes = shown_attributes,
		.attribute_count = 2,
		.layout = scene->texture_layout,
	};
	if (result == VK_SUCCESS)
		result = harness_pipeline(harness, &spec, scene->screen_pass, &scene->textured);
	return result;
}

// Nearest texels, as D24S8 is sampled.
static VkResult create_descriptor(const Harness *harness, Scene *scene)
{
	const VkSamplerCreateInfo sampler_info = {
		.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
		.magFilter = VK_FILTER_NEAREST,
		.minFilter = VK_FILTER_NEAREST,
		.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
		.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
	};
	VkResult result = vkCreateSampler(harness->device, &sampler_info, NULL, &scene->sampler);
	const VkDescriptorPoolSize size = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 };
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
	const VkDescriptorImageInfo image = { scene->sampler, scene->sampled_view,
		VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL };
	const VkWriteDescriptorSet write = {
		.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		.dstSet = scene->set,
		.dstBinding = 0,
		.descriptorCount = 1,
		.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		.pImageInfo = &image,
	};
	vkUpdateDescriptorSets(harness->device, 1, &write, 0, NULL);
	return VK_SUCCESS;
}

static bool build(const Harness *harness, Scene *scene)
{
	bool ok = harness_ok("images", create_images(harness, scene));
	ok = ok && harness_ok("passes", create_passes(harness, scene));
	ok = ok && harness_ok("screen pass", harness_render_pass(harness, &scene->screen_pass, scene->screens));
	ok = ok && harness_ok("push layout", harness_push_layout(harness, &scene->push_layout));
	for (uint32_t q = 0; q < QUADS && ok; q++)
		ok = harness_ok("quad pipeline", create_quad_pipeline(harness, scene, q));
	ok = ok && harness_ok("textured pipeline", create_textured(harness, scene));
	ok = ok && harness_ok("descriptor", create_descriptor(harness, scene));
	ok = ok && harness_ok("unit quad", harness_quad_buffer(harness, &scene->unit_quad, &scene->unit_quad_memory));
	return ok &&
		harness_ok("shown quad",
			harness_buffer(harness, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, shown_quad, sizeof shown_quad, &scene->shown,
				&scene->shown_memory));
}

// A pass over the depth image's target, or the smaller one's, drawing quads through their pipelines in order.
typedef struct {
	VkRenderPass render_pass;
	VkFramebuffer framebuffer;
	uint32_t width;
	uint32_t height;
	const VkClearValue *clears;
	const DepthQuad *quads;
	uint32_t count;
} PassRecord;

static void record_pass(const Scene *scene, VkCommandBuffer commands, const PassRecord *record)
{
	const VkRenderPassBeginInfo pass = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		.renderPass = record->render_pass,
		.framebuffer = record->framebuffer,
		.renderArea = { { 0, 0 }, { record->width, record->height } },
		.clearValueCount = 2,
		.pClearValues = record->clears,
	};
	const VkViewport viewport = { 0.0f, 0.0f, (float)record->width, (float)record->height, 0.0f, 1.0f };
	const VkRect2D scissor = { { 0, 0 }, { record->width, record->height } };
	const VkDeviceSize offset = 0;
	vkCmdBeginRenderPass(commands, &pass, VK_SUBPASS_CONTENTS_INLINE);
	vkCmdSetViewport(commands, 0, 1, &viewport);
	vkCmdSetScissor(commands, 0, 1, &scissor);
	vkCmdBindVertexBuffers(commands, 0, 1, &scene->unit_quad, &offset);
	for (uint32_t q = 0; q < record->count; q++) {
		vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, scene->quad_pipelines[q]);
		harness_push_quad_in(commands, scene->push_layout, &record->quads[q].quad, record->width, record->height);
	}
	vkCmdEndRenderPass(commands);
}

static VkResult record(const Harness *harness, Scene *scene)
{
	VkResult result = harness_command_buffers(harness, scene->commands);
	for (uint32_t i = 0; i < harness->image_count && result == VK_SUCCESS; i++) {
		const VkCommandBuffer commands = scene->commands[i];
		const PassRecord passes[3] = {
			{ scene->kept_pass, scene->target, TARGET_WIDTH, TARGET_HEIGHT, target_clears, quads, QUADS },
			{ scene->other_pass, scene->other, OTHER_WIDTH, OTHER_HEIGHT, other_clears, &other_quad, 1 },
			{ scene->load_pass, scene->target, TARGET_WIDTH, TARGET_HEIGHT, target_clears, NULL, 0 },
		};
		for (uint32_t p = 0; p < 3; p++)
			record_pass(scene, commands, &passes[p]);
		const VkRenderPassBeginInfo pass = {
			.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
			.renderPass = scene->screen_pass,
			.framebuffer = scene->screens[i],
			.renderArea = { { 0, 0 }, { HARNESS_WIDTH, HARNESS_HEIGHT } },
			.clearValueCount = 1,
			.pClearValues = &screen_clear,
		};
		const VkDeviceSize offset = 0;
		vkCmdBeginRenderPass(commands, &pass, VK_SUBPASS_CONTENTS_INLINE);
		vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, scene->textured);
		vkCmdBindDescriptorSets(
			commands, VK_PIPELINE_BIND_POINT_GRAPHICS, scene->texture_layout, 0, 1, &scene->set, 0, NULL);
		vkCmdBindVertexBuffers(commands, 0, 1, &scene->shown, &offset);
		vkCmdDraw(commands, QUAD_VERTICES, 1, 0, 0);
		vkCmdEndRenderPass(commands);
		result = vkEndCommandBuffer(commands);
	}
	return result;
}

// One submission of the kept pass, its quad drawn by the first quad's pipeline; then whether the memory still holds
// what the frames left in it.
static bool keeps_memory(const Harness *harness, Scene *scene)
{
	VkCommandBuffer commands;
	const VkCommandBufferAllocateInfo allocate_info = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = harness->pool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1,
	};
	if (!harness_ok("kept pass commands", vkAllocateCommandBuffers(harness->device, &allocate_info, &commands)))
		return false;
	const VkCommandBufferBeginInfo begin = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	bool ok = harness_ok("vkBeginCommandBuffer", vkBeginCommandBuffer(commands, &begin));
	const DepthQuad kept = { kept_quad, quads[0].stencil };
	const PassRecord record = { scene->kept_pass, scene->target, TARGET_WIDTH, TARGET_HEIGHT, target_clears, &kept, 1 };
	if (ok)
		record_pass(scene, commands, &record);
	ok = ok && harness_ok("vkEndCommandBuffer", vkEndCommandBuffer(commands));
	const VkSubmitInfo submit = {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.commandBufferCount = 1,
		.pCommandBuffers = &commands,
	};
	ok = ok && harness_ok("vkQueueSubmit", vkQueueSubmit(harness->queue, 1, &submit, harness->fence)) &&
		harness_ok("vkWaitForFences", vkWaitForFences(harness->device, 1, &harness->fence, VK_TRUE, UINT64_MAX));
	(void)vkResetFences(harness->device, 1, &harness->fence);
	vkFreeCommandBuffers(harness->device, harness->pool, 1, &commands);
	if (!ok)
		return false;
	const uint32_t misses = scene->word_misses;
	scene->words_logged = false;
	(void)frame_holds_words(scene, "after the kept pass");
	return scene->word_misses == misses;
}

static void destroy(const Harness *harness, Scene *scene)
{
	const VkDevice device = harness->device;
	vkFreeCommandBuffers(device, harness->pool, harness->image_count, scene->commands);
	vkDestroyBuffer(device, scene->shown, NULL);
	vkFreeMemory(device, scene->shown_memory, NULL);
	vkDestroyBuffer(device, scene->unit_quad, NULL);
	vkFreeMemory(device, scene->unit_quad_memory, NULL);
	vkDestroyDescriptorPool(device, scene->pool, NULL);
	vkDestroySampler(device, scene->sampler, NULL);
	vkDestroyPipeline(device, scene->textured, NULL);
	vkDestroyPipelineLayout(device, scene->texture_layout, NULL);
	vkDestroyDescriptorSetLayout(device, scene->set_layout, NULL);
	for (uint32_t q = 0; q < QUADS; q++)
		vkDestroyPipeline(device, scene->quad_pipelines[q], NULL);
	vkDestroyPipelineLayout(device, scene->push_layout, NULL);
	harness_destroy_render_pass(harness, scene->screen_pass, scene->screens);
	vkDestroyFramebuffer(device, scene->other, NULL);
	vkDestroyFramebuffer(device, scene->target, NULL);
	vkDestroyRenderPass(device, scene->other_pass, NULL);
	vkDestroyRenderPass(device, scene->load_pass, NULL);
	vkDestroyRenderPass(device, scene->kept_pass, NULL);
	vkDestroyImageView(device, scene->sampled_view, NULL);
	vkDestroyImageView(device, scene->depth_view, NULL);
	vkDestroyImage(device, scene->depth, NULL);
	vkFreeMemory(device, scene->depth_memory, NULL);
	vkDestroyImageView(device, scene->color_view, NULL);
	vkDestroyImage(device, scene->color, NULL);
	vkFreeMemory(device, scene->color_memory, NULL);
	vkDestroyImageView(device, scene->other_depth_view, NULL);
	vkDestroyImage(device, scene->other_depth, NULL);
	vkFreeMemory(device, scene->other_depth_memory, NULL);
	vkDestroyImageView(device, scene->other_color_view, NULL);
	vkDestroyImage(device, scene->other_color, NULL);
	vkFreeMemory(device, scene->other_color_memory, NULL);
}

void test_sampled_depth(const Harness *harness)
{
	Scene scene = { .kept_pass = VK_NULL_HANDLE };
	make_probes(&scene);
	check("sampled depth: D24S8 reported sampled, an image of it both sampled and an attachment",
		reports_sampled_depth(harness));
	const bool built = build(harness, &scene);
	check("sampled depth: a sampled D24S8 attachment, a smaller one, the passes that keep, load and sample them made",
		built);
	void *mapped = NULL;
	const bool ready = built &&
		harness_ok("vkMapMemory", vkMapMemory(harness->device, scene.depth_memory, 0, VK_WHOLE_SIZE, 0, &mapped));
	scene.words = mapped;
	const bool recorded = ready && harness->swapchain && harness_ok("recording", record(harness, &scene));
	uint32_t misses = 0;
	const uint32_t frames =
		recorded ? harness_frames(harness, scene.commands, FRAMES, frame_holds, &scene, &misses) : 0;
	check("sampled depth: 30 frames drawn and presented", frames == FRAMES);
	check(
		"sampled depth: every frame leaves each texel's depth above its stencil in the image's memory, to its last "
		"texel, a smaller pass with depth of its own between the pass that drew it and the one that loads it",
		frames == FRAMES && scene.word_misses == 0);
	check("sampled depth: every frame's fetch of it returns its depth in red, green and blue 0",
		frames == FRAMES && scene.screen_misses == 0);
	check("sampled depth: the image on screen shows it too",
		harness_screen(harness, harness_probes_hold, &scene.screen_check));
	check("sampled depth: a pass that stores it but keeps it as an attachment leaves its memory as it was",
		frames == FRAMES && keeps_memory(harness, &scene));
	destroy(harness, &scene);
}
