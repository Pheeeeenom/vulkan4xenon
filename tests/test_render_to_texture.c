#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "harness.h"
#include "shaders.h"
#include "tests.h"
#include "tiled_index.h"
#include "xsdk/log.h"

enum {
	FRAMES = 60,
	TOLERANCE = 2,
	// The scene, R8G8B8A8 with a D24S8 attachment; 200 wide, so its rows are 224 texels apart, whole tiles of 32.
	SCENE_WIDTH = 200,
	SCENE_HEIGHT = 120,
	SCENE_PITCH = 224,
	// Its copy, B8G8R8A8.
	COPY_WIDTH = 96,
	COPY_HEIGHT = 64,
	// A third image, B8G8R8A8, drawn while the swapchain image waits.
	TILE_WIDTH = 64,
	TILE_HEIGHT = 32,
	VERTEX_FLOATS = 6,
	STRIDE_BYTES = VERTEX_FLOATS * 4,
	UV_OFFSET_BYTES = 16,
	QUAD_VERTICES = 6,
	FENCE_TIMEOUT_NS = 1000000000,
};

typedef enum {
	IMAGE_SCENE,
	IMAGE_COPY,
	IMAGE_TILE,
	IMAGES,
} ImageKind;

typedef enum {
	PASS_SCENE_CLEAR,
	PASS_SCENE_LOAD,
	// The scene's colour loaded, without depth.
	PASS_SCENE_COLOR,
	PASS_COPY,
	PASS_TILE,
	PASS_SCREEN_CLEAR,
	PASS_SCREEN_LOAD,
	PASSES,
} PassKind;

typedef enum {
	// push.vert and push.frag, depth tested less-than and written, for the scene with depth.
	PIPELINE_DEPTH,
	// The same without depth, for the scene alone.
	PIPELINE_SCENE_FLAT,
	// The same for the B8G8R8A8 targets.
	PIPELINE_FLAT,
	// texture.vert and texture.frag, drawing an image over the viewport, for the B8G8R8A8 targets.
	PIPELINE_TEXTURED,
	PIPELINES,
} PipelineKind;

typedef struct {
	VkFormat format;
	VkAttachmentLoadOp load;
	bool swapchain;
	bool depth;
	VkAttachmentLoadOp depth_load;
} PassSpec;

static const PassSpec pass_specs[PASSES] = {
	[PASS_SCENE_CLEAR] = { VK_FORMAT_R8G8B8A8_UNORM, VK_ATTACHMENT_LOAD_OP_CLEAR, false, true,
		VK_ATTACHMENT_LOAD_OP_CLEAR },
	[PASS_SCENE_LOAD] = { VK_FORMAT_R8G8B8A8_UNORM, VK_ATTACHMENT_LOAD_OP_LOAD, false, true,
		VK_ATTACHMENT_LOAD_OP_LOAD },
	[PASS_SCENE_COLOR] = { VK_FORMAT_R8G8B8A8_UNORM, VK_ATTACHMENT_LOAD_OP_LOAD, false, false,
		VK_ATTACHMENT_LOAD_OP_DONT_CARE },
	[PASS_COPY] = { VK_FORMAT_B8G8R8A8_UNORM, VK_ATTACHMENT_LOAD_OP_DONT_CARE, false, false,
		VK_ATTACHMENT_LOAD_OP_DONT_CARE },
	[PASS_TILE] = { VK_FORMAT_B8G8R8A8_UNORM, VK_ATTACHMENT_LOAD_OP_CLEAR, false, false,
		VK_ATTACHMENT_LOAD_OP_DONT_CARE },
	[PASS_SCREEN_CLEAR] = { VK_FORMAT_B8G8R8A8_UNORM, VK_ATTACHMENT_LOAD_OP_CLEAR, true, false,
		VK_ATTACHMENT_LOAD_OP_DONT_CARE },
	[PASS_SCREEN_LOAD] = { VK_FORMAT_B8G8R8A8_UNORM, VK_ATTACHMENT_LOAD_OP_LOAD, true, false,
		VK_ATTACHMENT_LOAD_OP_DONT_CARE },
};

static const VkFormat image_formats[IMAGES] = { VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM,
	VK_FORMAT_B8G8R8A8_UNORM };
static const uint32_t image_widths[IMAGES] = { SCENE_WIDTH, COPY_WIDTH, TILE_WIDTH };
static const uint32_t image_heights[IMAGES] = { SCENE_HEIGHT, COPY_HEIGHT, TILE_HEIGHT };
// Rows a whole number of 32-texel tiles apart, as the layer lays images out (docs/ROADMAP.md, milestone 2).
static const uint32_t image_pitches[IMAGES] = { SCENE_PITCH, COPY_WIDTH, TILE_WIDTH };

#define RGBA(r, g, b) { (r) / 255.0f, (g) / 255.0f, (b) / 255.0f, 1.0f }

// The scene clears to teal and depth 1; red and blue differ in every colour, so a swap of them shows.
static const VkClearValue scene_clears[2] = {
	{ .color = { .float32 = RGBA(0, 96, 128) } },
	{ .depthStencil = { 1.0f, 0 } },
};
static const VkClearValue screen_clear = { .color = { .float32 = RGBA(24, 24, 24) } };
static const VkClearValue tile_clear = { .color = { .float32 = RGBA(240, 128, 0) } };

static const Rgb teal = { 0, 96, 128 };
static const Rgb green = { 40, 200, 40 };
static const Rgb red = { 220, 40, 40 };
static const Rgb blue = { 40, 40, 220 };
static const Rgb yellow = { 230, 210, 30 };
static const Rgb grey = { 24, 24, 24 };
static const Rgb orange = { 240, 128, 0 };
static const Rgb magenta = { 200, 40, 200 };
static const Rgb white = { 250, 250, 250 };

// Pass 1: the near green first, so the depth test keeps the far red out of it.
static const HarnessQuad scene_quads[] = {
	{ 20, 20, 100, 60, 0.3f, RGBA(40, 200, 40), false },
	{ 80, 50, 120, 60, 0.6f, RGBA(220, 40, 40), false },
};
// Pass 3: between them in depth, so it covers the red and the clear colour but not the green.
static const HarnessQuad load_quad = { 10, 60, 180, 40, 0.45f, RGBA(40, 40, 220), false };
// Pass 4.
static const HarnessQuad kept_quad = { 150, 5, 40, 30, 0.0f, RGBA(230, 210, 30), false };
// Pass 6, in the third image's top right quarter.
static const HarnessQuad tile_quad = { 32, 0, 32, 16, 0.0f, RGBA(200, 40, 200), false };
// Pass 8, on screen.
static const HarnessQuad screen_quad = { 40, 320, 100, 60, 0.0f, RGBA(250, 250, 250), false };

// Where passes 5 and 7 draw the images on screen: the scene twice its size, its copy three times, the third four.
static const VkRect2D shown[IMAGES] = {
	{ { 40, 40 }, { 2 * SCENE_WIDTH, 2 * SCENE_HEIGHT } },
	{ { 480, 40 }, { 3 * COPY_WIDTH, 3 * COPY_HEIGHT } },
	{ { 480, 300 }, { 4 * TILE_WIDTH, 4 * TILE_HEIGHT } },
};

typedef enum {
	GROUP_SCENE,
	GROUP_COPY,
	GROUP_LOAD,
	GROUP_KEPT,
	GROUP_SCREEN_LOAD,
	GROUP_SCREEN_KEPT,
	GROUPS,
} GroupKind;

static const char *const group_names[GROUPS] = {
	"the scene, depth tested at 200x120, to its last column and row",
	"its copy in B8G8R8A8, made before passes 3 and 4",
	"the scene loaded after another pass: colour drawn back, depth kept in eDRAM",
	"the scene loaded straight after",
	"the swapchain image loaded after another pass, the third image over it",
	"the swapchain image loaded straight after",
};

// A texel an image holds after passes 1 to 4.
typedef struct {
	const char *name;
	GroupKind group;
	ImageKind image;
	uint32_t x;
	uint32_t y;
	Rgb want;
} TexelProbe;

// The copy samples the scene at the texel each of its texels' centres lands in: about x * 200 / 96, y * 120 / 64.
static const TexelProbe texel_probes[] = {
	{ "the scene's clear colour", GROUP_SCENE, IMAGE_SCENE, 10, 10, teal },
	{ "green", GROUP_SCENE, IMAGE_SCENE, 50, 40, green },
	{ "the near green over the far red", GROUP_SCENE, IMAGE_SCENE, 100, 65, green },
	{ "red", GROUP_SCENE, IMAGE_SCENE, 150, 105, red },
	{ "the scene's last column", GROUP_SCENE, IMAGE_SCENE, 199, 55, red },
	{ "the scene's last row", GROUP_SCENE, IMAGE_SCENE, 5, 119, teal },
	{ "the copy's green", GROUP_COPY, IMAGE_COPY, 24, 21, green },
	{ "the copy's red, without pass 3's blue", GROUP_COPY, IMAGE_COPY, 76, 48, red },
	{ "the copy's clear colour", GROUP_COPY, IMAGE_COPY, 4, 5, teal },
	{ "the copy, without pass 4's yellow", GROUP_COPY, IMAGE_COPY, 81, 10, teal },
	{ "pass 3's blue over the far red", GROUP_LOAD, IMAGE_SCENE, 160, 90, blue },
	{ "pass 3's blue over the clear colour", GROUP_LOAD, IMAGE_SCENE, 15, 90, blue },
	{ "the near green over pass 3's blue, by pass 1's depth", GROUP_LOAD, IMAGE_SCENE, 50, 70, green },
	{ "pass 4's yellow", GROUP_KEPT, IMAGE_SCENE, 170, 20, yellow },
};

// What only the screen shows: passes 5 to 8.
static const HarnessProbe screen_probes[] = {
	{ "the third image's orange", 521, 397, orange, 0 },
	{ "the third image's magenta", 673, 333, magenta, 0 },
	{ "the screen's clear colour beside the scene", 460, 100, grey, 0 },
	{ "the screen's clear colour", 20, 20, grey, 0 },
	{ "pass 8's white", 90, 350, white, 0 },
};

enum {
	TEXEL_PROBES = sizeof texel_probes / sizeof texel_probes[0],
	SCREEN_PROBES = sizeof screen_probes / sizeof screen_probes[0],
};

static const GroupKind screen_groups[SCREEN_PROBES] = { GROUP_SCREEN_LOAD, GROUP_SCREEN_LOAD, GROUP_SCREEN_LOAD,
	GROUP_SCREEN_LOAD, GROUP_SCREEN_KEPT };

typedef struct {
	VkImage images[IMAGES];
	VkDeviceMemory image_memory[IMAGES];
	VkImageView views[IMAGES];
	VkImage depth;
	VkDeviceMemory depth_memory;
	VkImageView depth_view;
	VkRenderPass passes[PASSES];
	VkFramebuffer scene_depth;
	VkFramebuffer scene;
	VkFramebuffer copy;
	VkFramebuffer tile;
	VkFramebuffer screens[HARNESS_MAX_IMAGES];
	VkPipelineLayout push_layout;
	VkDescriptorSetLayout set_layout;
	VkPipelineLayout texture_layout;
	VkPipeline pipelines[PIPELINES];
	VkSampler sampler;
	VkDescriptorPool pool;
	VkDescriptorSet sets[IMAGES];
	VkBuffer unit_quad;
	VkDeviceMemory unit_quad_memory;
	VkBuffer image_quad;
	VkDeviceMemory image_quad_memory;
	VkCommandBuffer commands[HARNESS_MAX_IMAGES];
	HarnessProbe grouped[GROUPS][TEXEL_PROBES + SCREEN_PROBES];
	HarnessProbes groups[GROUPS];
	uint32_t misses[GROUPS];
} Scene;

// Clip space, then texture coordinates, over the whole viewport.
static const float image_quad[QUAD_VERTICES][VERTEX_FLOATS] = {
	{ -1.0f, -1.0f, 0.0f, 1.0f, 0.0f, 0.0f },
	{ 1.0f, -1.0f, 0.0f, 1.0f, 1.0f, 0.0f },
	{ -1.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f },
	{ 1.0f, -1.0f, 0.0f, 1.0f, 1.0f, 0.0f },
	{ 1.0f, 1.0f, 0.0f, 1.0f, 1.0f, 1.0f },
	{ -1.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f },
};

static const VkVertexInputBindingDescription image_binding = { 0, STRIDE_BYTES, VK_VERTEX_INPUT_RATE_VERTEX };
static const VkVertexInputAttributeDescription image_attributes[] = {
	{ .location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = 0 },
	{ .location = 1, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = UV_OFFSET_BYTES },
};

// Each group's screen probes: a texel probe lands at its texel's centre where pass 5 drew the image.
static void make_groups(Scene *scene)
{
	uint32_t counts[GROUPS] = { 0 };
	for (uint32_t i = 0; i < TEXEL_PROBES; i++) {
		const TexelProbe *const probe = &texel_probes[i];
		const VkRect2D *const area = &shown[probe->image];
		const uint32_t scale = area->extent.width / image_widths[probe->image];
		scene->grouped[probe->group][counts[probe->group]++] =
			(HarnessProbe){ probe->name, (uint32_t)area->offset.x + probe->x * scale + scale / 2,
				(uint32_t)area->offset.y + probe->y * scale + scale / 2, probe->want, 0 };
	}
	for (uint32_t i = 0; i < SCREEN_PROBES; i++)
		scene->grouped[screen_groups[i]][counts[screen_groups[i]]++] = screen_probes[i];
	for (uint32_t g = 0; g < GROUPS; g++)
		scene->groups[g] = (HarnessProbes){ group_names[g], scene->grouped[g], counts[g], TOLERANCE, 0 };
}

// A HarnessCheck over every group, counting each group's misses.
static bool groups_hold(uint32_t address, const char *what, void *context)
{
	Scene *const scene = context;
	bool held = true;
	for (uint32_t g = 0; g < GROUPS; g++) {
		const bool group = harness_probes_hold(address, what, &scene->groups[g]);
		scene->misses[g] += !group;
		held = held && group;
	}
	return held;
}

// Sampling an image waits for the pass before that wrote it.
static const VkSubpassDependency dependencies[2] = {
	{ VK_SUBPASS_EXTERNAL, 0, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
		VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
			VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
		0 },
	{ 0, VK_SUBPASS_EXTERNAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
		VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, 0 },
};

static VkResult create_render_pass(const Harness *harness, const PassSpec *spec, VkRenderPass *render_pass)
{
	const VkImageLayout after =
		spec->swapchain ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	const bool loads_depth = spec->depth_load == VK_ATTACHMENT_LOAD_OP_LOAD;
	const VkAttachmentDescription attachments[2] = {
		{ .format = spec->format,
			.samples = VK_SAMPLE_COUNT_1_BIT,
			.loadOp = spec->load,
			.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
			.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
			.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
			.initialLayout = spec->load == VK_ATTACHMENT_LOAD_OP_LOAD ? after : VK_IMAGE_LAYOUT_UNDEFINED,
			.finalLayout = after },
		{ .format = VK_FORMAT_D24_UNORM_S8_UINT,
			.samples = VK_SAMPLE_COUNT_1_BIT,
			.loadOp = spec->depth_load,
			.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
			.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
			.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
			.initialLayout = loads_depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
			.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL },
	};
	const VkAttachmentReference color = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
	const VkAttachmentReference depth = { 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
	const VkSubpassDescription subpass = {
		.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
		.colorAttachmentCount = 1,
		.pColorAttachments = &color,
		.pDepthStencilAttachment = spec->depth ? &depth : NULL,
	};
	const VkRenderPassCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
		.attachmentCount = spec->depth ? 2 : 1,
		.pAttachments = attachments,
		.subpassCount = 1,
		.pSubpasses = &subpass,
		.dependencyCount = 2,
		.pDependencies = dependencies,
	};
	return vkCreateRenderPass(harness->device, &info, NULL, render_pass);
}

static VkResult create_framebuffer(const Harness *harness, VkRenderPass render_pass, const VkImageView *views,
	uint32_t view_count, uint32_t width, uint32_t height, VkFramebuffer *framebuffer)
{
	const VkFramebufferCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
		.renderPass = render_pass,
		.attachmentCount = view_count,
		.pAttachments = views,
		.width = width,
		.height = height,
		.layers = 1,
	};
	return vkCreateFramebuffer(harness->device, &info, NULL, framebuffer);
}

static VkResult create_targets(const Harness *harness, Scene *scene)
{
	const VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	VkResult result = VK_SUCCESS;
	for (uint32_t i = 0; i < IMAGES && result == VK_SUCCESS; i++)
		result = harness_image(harness, image_formats[i], image_widths[i], image_heights[i], usage, &scene->images[i],
			&scene->image_memory[i], &scene->views[i]);
	if (result == VK_SUCCESS)
		result = harness_image(harness, VK_FORMAT_D24_UNORM_S8_UINT, SCENE_WIDTH, SCENE_HEIGHT,
			VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, &scene->depth, &scene->depth_memory, &scene->depth_view);
	for (uint32_t i = 0; i < PASSES && result == VK_SUCCESS; i++)
		result = create_render_pass(harness, &pass_specs[i], &scene->passes[i]);
	if (result != VK_SUCCESS)
		return result;
	const VkImageView scene_views[2] = { scene->views[IMAGE_SCENE], scene->depth_view };
	result = create_framebuffer(
		harness, scene->passes[PASS_SCENE_CLEAR], scene_views, 2, SCENE_WIDTH, SCENE_HEIGHT, &scene->scene_depth);
	if (result == VK_SUCCESS)
		result = create_framebuffer(
			harness, scene->passes[PASS_SCENE_COLOR], scene_views, 1, SCENE_WIDTH, SCENE_HEIGHT, &scene->scene);
	if (result == VK_SUCCESS)
		result = create_framebuffer(
			harness, scene->passes[PASS_COPY], &scene->views[IMAGE_COPY], 1, COPY_WIDTH, COPY_HEIGHT, &scene->copy);
	if (result == VK_SUCCESS)
		result = create_framebuffer(
			harness, scene->passes[PASS_TILE], &scene->views[IMAGE_TILE], 1, TILE_WIDTH, TILE_HEIGHT, &scene->tile);
	for (uint32_t i = 0; i < harness->image_count && result == VK_SUCCESS; i++)
		result = create_framebuffer(harness, scene->passes[PASS_SCREEN_CLEAR], &harness->views[i], 1, HARNESS_WIDTH,
			HARNESS_HEIGHT, &scene->screens[i]);
	return result;
}

static VkResult create_descriptors(const Harness *harness, Scene *scene)
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
	// Nearest texels, so each lands whole.
	const VkSamplerCreateInfo sampler_info = {
		.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
		.magFilter = VK_FILTER_NEAREST,
		.minFilter = VK_FILTER_NEAREST,
		.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
		.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
	};
	if (result == VK_SUCCESS)
		result = vkCreateSampler(harness->device, &sampler_info, NULL, &scene->sampler);
	const VkDescriptorPoolSize size = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, IMAGES };
	const VkDescriptorPoolCreateInfo pool_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.maxSets = IMAGES,
		.poolSizeCount = 1,
		.pPoolSizes = &size,
	};
	if (result == VK_SUCCESS)
		result = vkCreateDescriptorPool(harness->device, &pool_info, NULL, &scene->pool);
	const VkDescriptorSetLayout layouts[IMAGES] = { scene->set_layout, scene->set_layout, scene->set_layout };
	const VkDescriptorSetAllocateInfo allocate_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool = scene->pool,
		.descriptorSetCount = IMAGES,
		.pSetLayouts = layouts,
	};
	if (result == VK_SUCCESS)
		result = vkAllocateDescriptorSets(harness->device, &allocate_info, scene->sets);
	for (uint32_t i = 0; i < IMAGES && result == VK_SUCCESS; i++) {
		const VkDescriptorImageInfo image = { scene->sampler, scene->views[i],
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
		const VkWriteDescriptorSet write = {
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstSet = scene->sets[i],
			.descriptorCount = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			.pImageInfo = &image,
		};
		vkUpdateDescriptorSets(harness->device, 1, &write, 0, NULL);
	}
	return result;
}

// Every pipeline takes its viewport and scissor from the commands, as the targets differ in size.
static VkResult create_pipeline(const Harness *harness, Scene *scene, PipelineKind kind)
{
	const VkPipelineDepthStencilStateCreateInfo depth = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
		.depthTestEnable = VK_TRUE,
		.depthWriteEnable = VK_TRUE,
		.depthCompareOp = VK_COMPARE_OP_LESS,
	};
	const bool textured = kind == PIPELINE_TEXTURED;
	const HarnessPipeline spec = {
		.vertex_spirv = textured ? texture_vert_spirv : push_vert_spirv,
		.vertex_bytes = textured ? TEXTURE_VERT_SPIRV_BYTES : PUSH_VERT_SPIRV_BYTES,
		.fragment_spirv = textured ? texture_frag_spirv : push_frag_spirv,
		.fragment_bytes = textured ? TEXTURE_FRAG_SPIRV_BYTES : PUSH_FRAG_SPIRV_BYTES,
		.bindings = textured ? &image_binding : &harness_quad_binding,
		.binding_count = 1,
		.attributes = textured ? image_attributes : &harness_quad_attribute,
		.attribute_count = textured ? 2 : 1,
		.layout = textured ? scene->texture_layout : scene->push_layout,
		.depth_stencil = kind == PIPELINE_DEPTH ? &depth : NULL,
		.dynamic_viewport = true,
	};
	const PassKind pass = kind == PIPELINE_DEPTH ? PASS_SCENE_CLEAR
		: kind == PIPELINE_SCENE_FLAT            ? PASS_SCENE_COLOR
												 : PASS_COPY;
	return harness_pipeline(harness, &spec, scene->passes[pass], &scene->pipelines[kind]);
}

static bool build(const Harness *harness, Scene *scene)
{
	bool ok = harness_ok("targets", create_targets(harness, scene));
	ok = ok && harness_ok("push layout", harness_push_layout(harness, &scene->push_layout));
	ok = ok && harness_ok("descriptors", create_descriptors(harness, scene));
	for (uint32_t i = 0; i < PIPELINES && ok; i++)
		ok = harness_ok("pipeline", create_pipeline(harness, scene, (PipelineKind)i));
	ok = ok && harness_ok("unit quad", harness_quad_buffer(harness, &scene->unit_quad, &scene->unit_quad_memory));
	return ok &&
		harness_ok("image quad",
			harness_buffer(harness, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, image_quad, sizeof image_quad,
				&scene->image_quad, &scene->image_quad_memory));
}

static void set_area(VkCommandBuffer commands, const VkRect2D *area)
{
	const VkViewport viewport = { (float)area->offset.x, (float)area->offset.y, (float)area->extent.width,
		(float)area->extent.height, 0.0f, 1.0f };
	vkCmdSetViewport(commands, 0, 1, &viewport);
	vkCmdSetScissor(commands, 0, 1, area);
}

static void begin(VkCommandBuffer commands, VkRenderPass render_pass, VkFramebuffer framebuffer, uint32_t width,
	uint32_t height, const VkClearValue *clears, uint32_t clear_count)
{
	const VkRect2D whole = { { 0, 0 }, { width, height } };
	const VkRenderPassBeginInfo info = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		.renderPass = render_pass,
		.framebuffer = framebuffer,
		.renderArea = whole,
		.clearValueCount = clear_count,
		.pClearValues = clears,
	};
	vkCmdBeginRenderPass(commands, &info, VK_SUBPASS_CONTENTS_INLINE);
	set_area(commands, &whole);
}

// An image drawn over an area of the target, each texel by its nearest.
static void draw_image(VkCommandBuffer commands, const Scene *scene, ImageKind image, const VkRect2D *area)
{
	const VkDeviceSize offset = 0;
	set_area(commands, area);
	vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, scene->pipelines[PIPELINE_TEXTURED]);
	vkCmdBindDescriptorSets(
		commands, VK_PIPELINE_BIND_POINT_GRAPHICS, scene->texture_layout, 0, 1, &scene->sets[image], 0, NULL);
	vkCmdBindVertexBuffers(commands, 0, 1, &scene->image_quad, &offset);
	vkCmdDraw(commands, QUAD_VERTICES, 1, 0, 0);
}

static void draw_quad(VkCommandBuffer commands, const Scene *scene, PipelineKind pipeline, const HarnessQuad *quad,
	uint32_t width, uint32_t height)
{
	const VkDeviceSize offset = 0;
	vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, scene->pipelines[pipeline]);
	vkCmdBindVertexBuffers(commands, 0, 1, &scene->unit_quad, &offset);
	harness_push_quad_in(commands, scene->push_layout, quad, width, height);
}

// Passes 1 to count of the four off screen.
static void record_offscreen(VkCommandBuffer commands, const Scene *scene, uint32_t count)
{
	// 1. The scene, depth tested.
	begin(commands, scene->passes[PASS_SCENE_CLEAR], scene->scene_depth, SCENE_WIDTH, SCENE_HEIGHT, scene_clears, 2);
	for (uint32_t i = 0; i < sizeof scene_quads / sizeof scene_quads[0]; i++)
		draw_quad(commands, scene, PIPELINE_DEPTH, &scene_quads[i], SCENE_WIDTH, SCENE_HEIGHT);
	vkCmdEndRenderPass(commands);
	if (count == 1)
		return;
	// 2. Copied into a smaller image, as encounter's Aurora copies its framebuffer.
	const VkRect2D whole_copy = { { 0, 0 }, { COPY_WIDTH, COPY_HEIGHT } };
	begin(commands, scene->passes[PASS_COPY], scene->copy, COPY_WIDTH, COPY_HEIGHT, NULL, 0);
	draw_image(commands, scene, IMAGE_SCENE, &whole_copy);
	vkCmdEndRenderPass(commands);
	// 3. Loaded after the copy took eDRAM: colour drawn back in, depth still there.
	begin(commands, scene->passes[PASS_SCENE_LOAD], scene->scene_depth, SCENE_WIDTH, SCENE_HEIGHT, NULL, 0);
	draw_quad(commands, scene, PIPELINE_DEPTH, &load_quad, SCENE_WIDTH, SCENE_HEIGHT);
	vkCmdEndRenderPass(commands);
	// 4. Loaded straight after, without depth.
	begin(commands, scene->passes[PASS_SCENE_COLOR], scene->scene, SCENE_WIDTH, SCENE_HEIGHT, NULL, 0);
	draw_quad(commands, scene, PIPELINE_SCENE_FLAT, &kept_quad, SCENE_WIDTH, SCENE_HEIGHT);
	vkCmdEndRenderPass(commands);
}

// Passes 5 to 8, onto swapchain image index.
static void record_screen(VkCommandBuffer commands, const Scene *scene, uint32_t index)
{
	// 5. Both images on screen.
	const VkFramebuffer screen = scene->screens[index];
	begin(commands, scene->passes[PASS_SCREEN_CLEAR], screen, HARNESS_WIDTH, HARNESS_HEIGHT, &screen_clear, 1);
	draw_image(commands, scene, IMAGE_SCENE, &shown[IMAGE_SCENE]);
	draw_image(commands, scene, IMAGE_COPY, &shown[IMAGE_COPY]);
	vkCmdEndRenderPass(commands);
	// 6. A third image drawn while the screen waits.
	begin(commands, scene->passes[PASS_TILE], scene->tile, TILE_WIDTH, TILE_HEIGHT, &tile_clear, 1);
	draw_quad(commands, scene, PIPELINE_FLAT, &tile_quad, TILE_WIDTH, TILE_HEIGHT);
	vkCmdEndRenderPass(commands);
	// 7. The screen loaded after it, so drawn back in, with the third image over it.
	begin(commands, scene->passes[PASS_SCREEN_LOAD], screen, HARNESS_WIDTH, HARNESS_HEIGHT, NULL, 0);
	draw_image(commands, scene, IMAGE_TILE, &shown[IMAGE_TILE]);
	vkCmdEndRenderPass(commands);
	// 8. Loaded straight after, with a white quad.
	begin(commands, scene->passes[PASS_SCREEN_LOAD], screen, HARNESS_WIDTH, HARNESS_HEIGHT, NULL, 0);
	draw_quad(commands, scene, PIPELINE_FLAT, &screen_quad, HARNESS_WIDTH, HARNESS_HEIGHT);
	vkCmdEndRenderPass(commands);
}

static VkResult record(const Harness *harness, Scene *scene)
{
	VkResult result = harness_command_buffers(harness, scene->commands);
	for (uint32_t i = 0; i < harness->image_count && result == VK_SUCCESS; i++) {
		record_offscreen(scene->commands[i], scene, 4);
		record_screen(scene->commands[i], scene, i);
		result = vkEndCommandBuffer(scene->commands[i]);
	}
	return result;
}

// A texel of an image, read where the layer's tiled layout puts it; R8G8B8A8 bytes are red first, B8G8R8A8 blue.
static Rgb image_texel(const uint8_t *texels, ImageKind image, uint32_t x, uint32_t y)
{
	const uint8_t *const bytes = texels + tiled_index(x, y, image_pitches[image]) * 4;
	if (image_formats[image] == VK_FORMAT_R8G8B8A8_UNORM)
		return (Rgb){ bytes[0], bytes[1], bytes[2] };
	return (Rgb){ bytes[2], bytes[1], bytes[0] };
}

// Records the first count passes in a command buffer of their own, and waits for them.
static bool run_offscreen(const Harness *harness, const Scene *scene, uint32_t count)
{
	const VkCommandBufferAllocateInfo allocate_info = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = harness->pool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1,
	};
	VkCommandBuffer commands = VK_NULL_HANDLE;
	if (!harness_ok("offscreen command buffer", vkAllocateCommandBuffers(harness->device, &allocate_info, &commands)))
		return false;
	const VkCommandBufferBeginInfo begin_info = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	(void)vkBeginCommandBuffer(commands, &begin_info);
	record_offscreen(commands, scene, count);
	bool ok = harness_ok("offscreen recording", vkEndCommandBuffer(commands));
	const VkSubmitInfo submit = {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &commands
	};
	ok = ok && harness_ok("offscreen submit", vkQueueSubmit(harness->queue, 1, &submit, harness->fence));
	ok = ok &&
		harness_ok("offscreen wait", vkWaitForFences(harness->device, 1, &harness->fence, VK_TRUE, FENCE_TIMEOUT_NS));
	(void)vkResetFences(harness->device, 1, &harness->fence);
	vkFreeCommandBuffers(harness->device, harness->pool, 1, &commands);
	return ok;
}

// Without a display, as in Xenia, the passes off screen still run; their images are read back for the texel probes:
// the scene's alone after pass 1, all of them after pass 4.
static uint32_t offscreen_misses(const Harness *harness, const Scene *scene, uint32_t count)
{
	bool ok = run_offscreen(harness, scene, count);
	void *mapped[IMAGES] = { NULL };
	for (uint32_t i = 0; i < IMAGES && ok; i++)
		ok = harness_ok(
			"vkMapMemory", vkMapMemory(harness->device, scene->image_memory[i], 0, VK_WHOLE_SIZE, 0, &mapped[i]));
	uint32_t misses = ok ? 0 : TEXEL_PROBES;
	for (uint32_t i = 0; i < TEXEL_PROBES && ok; i++) {
		const TexelProbe *const probe = &texel_probes[i];
		if (count == 1 && probe->group != GROUP_SCENE)
			continue;
		const Rgb texel = image_texel(mapped[probe->image], probe->image, probe->x, probe->y);
		if (harness_near(texel, probe->want, TOLERANCE))
			continue;
		misses++;
		xsdk_log_write("render to texture: after pass %u, %s at (%u, %u) is %u %u %u, not %u %u %u", (unsigned)count,
			probe->name, (unsigned)probe->x, (unsigned)probe->y, (unsigned)texel.red, (unsigned)texel.green,
			(unsigned)texel.blue, (unsigned)probe->want.red, (unsigned)probe->want.green, (unsigned)probe->want.blue);
	}
	for (uint32_t i = 0; i < IMAGES; i++) {
		if (mapped[i])
			vkUnmapMemory(harness->device, scene->image_memory[i]);
	}
	return misses;
}

static void destroy(const Harness *harness, Scene *scene)
{
	const VkDevice device = harness->device;
	vkFreeCommandBuffers(device, harness->pool, harness->image_count, scene->commands);
	vkDestroyBuffer(device, scene->image_quad, NULL);
	vkFreeMemory(device, scene->image_quad_memory, NULL);
	vkDestroyBuffer(device, scene->unit_quad, NULL);
	vkFreeMemory(device, scene->unit_quad_memory, NULL);
	for (uint32_t i = 0; i < PIPELINES; i++)
		vkDestroyPipeline(device, scene->pipelines[i], NULL);
	vkDestroyDescriptorPool(device, scene->pool, NULL);
	vkDestroySampler(device, scene->sampler, NULL);
	vkDestroyPipelineLayout(device, scene->texture_layout, NULL);
	vkDestroyDescriptorSetLayout(device, scene->set_layout, NULL);
	vkDestroyPipelineLayout(device, scene->push_layout, NULL);
	for (uint32_t i = 0; i < harness->image_count; i++)
		vkDestroyFramebuffer(device, scene->screens[i], NULL);
	vkDestroyFramebuffer(device, scene->tile, NULL);
	vkDestroyFramebuffer(device, scene->copy, NULL);
	vkDestroyFramebuffer(device, scene->scene, NULL);
	vkDestroyFramebuffer(device, scene->scene_depth, NULL);
	for (uint32_t i = 0; i < PASSES; i++)
		vkDestroyRenderPass(device, scene->passes[i], NULL);
	vkDestroyImageView(device, scene->depth_view, NULL);
	vkDestroyImage(device, scene->depth, NULL);
	vkFreeMemory(device, scene->depth_memory, NULL);
	for (uint32_t i = 0; i < IMAGES; i++) {
		vkDestroyImageView(device, scene->views[i], NULL);
		vkDestroyImage(device, scene->images[i], NULL);
		vkFreeMemory(device, scene->image_memory[i], NULL);
	}
}

static void check_frames(const Harness *harness, Scene *scene, bool built)
{
	const bool recorded = built && harness->swapchain && harness_ok("recording", record(harness, scene));
	uint32_t misses = 0;
	const uint32_t frames =
		recorded ? harness_frames(harness, scene->commands, FRAMES, groups_hold, scene, &misses) : 0;
	check("render to texture: 60 frames of eight render passes drawn and presented", frames == FRAMES);
	for (uint32_t g = 0; g < GROUPS; g++) {
		char name[128];
		(void)snprintf(name, sizeof name, "render to texture: every frame: %s", group_names[g]);
		check(name, frames == FRAMES && scene->misses[g] == 0);
	}
	check("render to texture: the image on screen shows them too", harness_screen(harness, groups_hold, scene));
}

void test_render_to_texture(const Harness *harness)
{
	static Scene scene;
	memset(&scene, 0, sizeof scene);
	make_groups(&scene);
	const bool built = build(harness, &scene);
	check("render to texture: three images, seven render passes and four pipelines made", built);
	check("render to texture: pass 1 alone leaves the scene's texels, read back",
		built && offscreen_misses(harness, &scene, 1) == 0);
	check("render to texture: passes 1 to 4 leave every texel probed in the scene and its copy, read back",
		built && offscreen_misses(harness, &scene, 4) == 0);
	check_frames(harness, &scene, built);
	destroy(harness, &scene);
}
