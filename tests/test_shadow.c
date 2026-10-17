#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "harness.h"
#include "shaders.h"
#include "tests.h"
#include "xsdk/log.h"

// A D24S8 image drawn once at depth 0.25 on its left half and 0.75 on its right, then sampled every frame: compared
// against a reference of 0.5 through comparison samplers, less, greater, never and always, each half on its own side;
// and filtered linearly, the format reporting it, at four times its size across the edge between the depths
// (milestone 23).

enum {
	FRAMES = 30,
	FENCE_TIMEOUT_NS = 1000000000,
	DEPTH_WIDTH = 64,
	DEPTH_HEIGHT = 32,
	OPS = 4,
	// The comparison cells, each the depth image at twice its size, then the filtered one at four times.
	CELL_WIDTH = 128,
	CELL_HEIGHT = 64,
	CELL_STEP = 160,
	CELL_LEFT = 40,
	CELL_TOP = 100,
	FILTERED_SCALE = 4,
	FILTERED_TOP = 240,
	SHADOW_FLOATS = 8,
	SHADOW_STRIDE = SHADOW_FLOATS * 4,
	TEXTURE_FLOATS = 6,
	TEXTURE_STRIDE = TEXTURE_FLOATS * 4,
	COORD_OFFSET_BYTES = 16,
	QUAD_VERTICES = 6,
	SETS = OPS + 1,
	COMPARE_PROBES = OPS * 2 + 1,
	FILTERED_PROBES = 4,
	TOLERANCE = 1,
	// A linear filter weighs texels in fixed point and rounds on the way out.
	FILTERED_TOLERANCE = 3,
};

#define REFERENCE  0.5f
#define NEAR_DEPTH 0.25f
#define FAR_DEPTH  0.75f

static const VkCompareOp ops[OPS] = { VK_COMPARE_OP_LESS, VK_COMPARE_OP_GREATER, VK_COMPARE_OP_NEVER,
	VK_COMPARE_OP_ALWAYS };
static const char *const op_names[OPS] = { "less", "greater", "never", "always" };

static const VkVertexInputBindingDescription shadow_binding = { 0, SHADOW_STRIDE, VK_VERTEX_INPUT_RATE_VERTEX };
static const VkVertexInputAttributeDescription shadow_attributes[] = {
	{ .location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = 0 },
	{ .location = 1, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = COORD_OFFSET_BYTES },
};
static const VkVertexInputBindingDescription texture_binding = { 0, TEXTURE_STRIDE, VK_VERTEX_INPUT_RATE_VERTEX };
static const VkVertexInputAttributeDescription texture_attributes[] = {
	{ .location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = 0 },
	{ .location = 1, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = COORD_OFFSET_BYTES },
};

static const VkClearValue screen_clear = { .color = { .float32 = { 24.0f / 255, 24.0f / 255, 24.0f / 255, 1.0f } } };
static const VkClearValue depth_clears[2] = {
	{ .color = { .float32 = { 0.0f, 0.0f, 0.0f, 1.0f } } },
	{ .depthStencil = { NEAR_DEPTH, 0 } },
};
// The depth image's right half, drawn over the clear.
static const HarnessQuad far_half = { DEPTH_WIDTH / 2, 0, DEPTH_WIDTH / 2, DEPTH_HEIGHT, FAR_DEPTH,
	{ 1.0f, 1.0f, 1.0f, 1.0f }, false };

typedef struct {
	VkImage color;
	VkDeviceMemory color_memory;
	VkImageView color_view;
	VkImage depth;
	VkDeviceMemory depth_memory;
	VkImageView depth_view;
	VkImageView sampled_view;
	VkRenderPass depth_pass;
	VkFramebuffer depth_target;
	VkPipelineLayout push_layout;
	VkPipeline depth_pipeline;
	VkBuffer unit_quad;
	VkDeviceMemory unit_quad_memory;
	VkRenderPass screen_pass;
	VkFramebuffer screens[HARNESS_MAX_IMAGES];
	VkDescriptorSetLayout set_layout;
	VkPipelineLayout layout;
	VkPipeline shadow_pipeline;
	VkPipeline texture_pipeline;
	VkSampler samplers[SETS];
	VkDescriptorPool pool;
	// A set for each compare op, then the filtered one.
	VkDescriptorSet sets[SETS];
	VkBuffer shadow_quads;
	VkDeviceMemory shadow_memory;
	VkBuffer filtered_quad;
	VkDeviceMemory filtered_memory;
	VkCommandBuffer commands[HARNESS_MAX_IMAGES];
	HarnessProbe compare_probes[COMPARE_PROBES];
	HarnessProbe filtered_probes[FILTERED_PROBES];
	HarnessProbes compared;
	HarnessProbes filtered;
} Scene;

// Vulkan's result, white for 1: the reference against a depth, VkCompareOp's bits 0 to 2 passing it below, equal to and
// above the depth.
static Rgb compared_rgb(VkCompareOp op, float depth)
{
	const uint32_t bits = (uint32_t)op;
	const bool passes =
		(bits & 1 && REFERENCE < depth) || (bits & 2 && REFERENCE == depth) || (bits & 4 && REFERENCE > depth);
	return passes ? (Rgb){ 255, 255, 255 } : (Rgb){ 0, 0, 0 };
}

static uint32_t to_byte(float value)
{
	return (uint32_t)(value * 255.0f + 0.5f);
}

// Each comparison cell at its near and far halves' middles; the filtered cell at both ends and either side of the edge,
// where the filter weighs 3/8 and 5/8 of the far texel.
static void make_probes(Scene *scene)
{
	for (uint32_t op = 0; op < OPS; op++) {
		const uint32_t x = CELL_LEFT + op * CELL_STEP;
		const uint32_t y = CELL_TOP + CELL_HEIGHT / 2;
		scene->compare_probes[2 * op] =
			(HarnessProbe){ op_names[op], x + CELL_WIDTH / 4, y, compared_rgb(ops[op], NEAR_DEPTH), 0 };
		scene->compare_probes[2 * op + 1] =
			(HarnessProbe){ op_names[op], x + 3 * CELL_WIDTH / 4, y, compared_rgb(ops[op], FAR_DEPTH), 0 };
	}
	scene->compare_probes[2 * OPS] = (HarnessProbe){ "the clear colour", 20, 20, { 24, 24, 24 }, 0 };
	const uint32_t edge = CELL_LEFT + FILTERED_SCALE * DEPTH_WIDTH / 2;
	const uint32_t y = FILTERED_TOP + FILTERED_SCALE * DEPTH_HEIGHT / 2;
	const float span = FAR_DEPTH - NEAR_DEPTH;
	scene->filtered_probes[0] = (HarnessProbe){ "the near half", CELL_LEFT + 16, y, { to_byte(NEAR_DEPTH), 0, 0 }, 0 };
	scene->filtered_probes[1] = (HarnessProbe){ "the far half", edge + 96, y, { to_byte(FAR_DEPTH), 0, 0 }, 0 };
	scene->filtered_probes[2] =
		(HarnessProbe){ "left of the edge", edge - 1, y, { to_byte(NEAR_DEPTH + span * 3 / 8), 0, 0 }, 0 };
	scene->filtered_probes[3] =
		(HarnessProbe){ "right of the edge", edge, y, { to_byte(NEAR_DEPTH + span * 5 / 8), 0, 0 }, 0 };
	scene->compared = (HarnessProbes){ "shadow", scene->compare_probes, COMPARE_PROBES, TOLERANCE, 0 };
	scene->filtered =
		(HarnessProbes){ "filtered depth", scene->filtered_probes, FILTERED_PROBES, FILTERED_TOLERANCE, 0 };
}

static float clip_x(float x)
{
	return x / (HARNESS_WIDTH / 2) - 1.0f;
}

static float clip_y(float y)
{
	return y / (HARNESS_HEIGHT / 2) - 1.0f;
}

static const float corners[QUAD_VERTICES][2] = { { 0, 0 }, { 1, 0 }, { 0, 1 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };

// Each comparison cell's quad, its coordinate the depth image's u and v and the reference.
static void fill_shadow_quads(float vertices[OPS * QUAD_VERTICES][SHADOW_FLOATS])
{
	for (uint32_t op = 0; op < OPS; op++) {
		for (uint32_t i = 0; i < QUAD_VERTICES; i++) {
			const float s = corners[i][0];
			const float t = corners[i][1];
			const float vertex[SHADOW_FLOATS] = { clip_x((float)(CELL_LEFT + op * CELL_STEP) + s * CELL_WIDTH),
				clip_y((float)CELL_TOP + t * CELL_HEIGHT), 0.0f, 1.0f, s, t, REFERENCE, 0.0f };
			memcpy(vertices[op * QUAD_VERTICES + i], vertex, sizeof vertex);
		}
	}
}

static void fill_filtered_quad(float vertices[QUAD_VERTICES][TEXTURE_FLOATS])
{
	for (uint32_t i = 0; i < QUAD_VERTICES; i++) {
		const float s = corners[i][0];
		const float t = corners[i][1];
		const float vertex[TEXTURE_FLOATS] = { clip_x((float)CELL_LEFT + s * FILTERED_SCALE * DEPTH_WIDTH),
			clip_y((float)FILTERED_TOP + t * FILTERED_SCALE * DEPTH_HEIGHT), 0.0f, 1.0f, s, t };
		memcpy(vertices[i], vertex, sizeof vertex);
	}
}

// Colour and depth cleared, depth stored and left to be read, so the pass's end resolves it into its texture.
static VkResult make_depth_pass(const Harness *harness, Scene *scene)
{
	const VkAttachmentDescription attachments[2] = {
		{ .format = VK_FORMAT_R8G8B8A8_UNORM,
			.samples = VK_SAMPLE_COUNT_1_BIT,
			.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
			.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
			.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
			.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
			.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
			.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL },
		{ .format = VK_FORMAT_D24_UNORM_S8_UINT,
			.samples = VK_SAMPLE_COUNT_1_BIT,
			.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
			.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
			.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
			.stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE,
			.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
			.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL },
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
	};
	VkResult result = vkCreateRenderPass(harness->device, &info, NULL, &scene->depth_pass);
	const VkImageView views[2] = { scene->color_view, scene->depth_view };
	const VkFramebufferCreateInfo framebuffer_info = {
		.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
		.renderPass = scene->depth_pass,
		.attachmentCount = 2,
		.pAttachments = views,
		.width = DEPTH_WIDTH,
		.height = DEPTH_HEIGHT,
		.layers = 1,
	};
	if (result == VK_SUCCESS)
		result = vkCreateFramebuffer(harness->device, &framebuffer_info, NULL, &scene->depth_target);
	return result;
}

static VkResult make_images(const Harness *harness, Scene *scene)
{
	VkResult result = harness_image(harness, VK_FORMAT_R8G8B8A8_UNORM, DEPTH_WIDTH, DEPTH_HEIGHT,
		VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, &scene->color, &scene->color_memory, &scene->color_view);
	if (result == VK_SUCCESS)
		result = harness_image(harness, VK_FORMAT_D24_UNORM_S8_UINT, DEPTH_WIDTH, DEPTH_HEIGHT,
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
	return result;
}

// Depth written wherever the quad lands.
static VkResult make_depth_pipeline(const Harness *harness, Scene *scene)
{
	const VkPipelineDepthStencilStateCreateInfo depth_stencil = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
		.depthTestEnable = VK_TRUE,
		.depthWriteEnable = VK_TRUE,
		.depthCompareOp = VK_COMPARE_OP_ALWAYS,
	};
	// The layout first, as the pipeline's spec takes it.
	const VkResult result = harness_push_layout(harness, &scene->push_layout);
	if (result != VK_SUCCESS)
		return result;
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
	return harness_pipeline(harness, &spec, scene->depth_pass, &scene->depth_pipeline);
}

static VkResult make_screen_pipelines(const Harness *harness, Scene *scene)
{
	const VkDescriptorSetLayoutBinding binding_info = {
		.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		.descriptorCount = 1,
		.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
	};
	const VkDescriptorSetLayoutCreateInfo set_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = 1,
		.pBindings = &binding_info,
	};
	VkResult result = vkCreateDescriptorSetLayout(harness->device, &set_info, NULL, &scene->set_layout);
	const VkPipelineLayoutCreateInfo layout_info = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = 1,
		.pSetLayouts = &scene->set_layout,
	};
	if (result == VK_SUCCESS)
		result = vkCreatePipelineLayout(harness->device, &layout_info, NULL, &scene->layout);
	const HarnessPipeline shadow = {
		.vertex_spirv = coord_vert_spirv,
		.vertex_bytes = COORD_VERT_SPIRV_BYTES,
		.fragment_spirv = shadow_frag_spirv,
		.fragment_bytes = SHADOW_FRAG_SPIRV_BYTES,
		.bindings = &shadow_binding,
		.binding_count = 1,
		.attributes = shadow_attributes,
		.attribute_count = 2,
		.layout = scene->layout,
	};
	const HarnessPipeline texture = {
		.vertex_spirv = texture_vert_spirv,
		.vertex_bytes = TEXTURE_VERT_SPIRV_BYTES,
		.fragment_spirv = texture_frag_spirv,
		.fragment_bytes = TEXTURE_FRAG_SPIRV_BYTES,
		.bindings = &texture_binding,
		.binding_count = 1,
		.attributes = texture_attributes,
		.attribute_count = 2,
		.layout = scene->layout,
	};
	if (result == VK_SUCCESS)
		result = harness_render_pass(harness, &scene->screen_pass, scene->screens);
	if (result == VK_SUCCESS)
		result = harness_pipeline(harness, &shadow, scene->screen_pass, &scene->shadow_pipeline);
	if (result == VK_SUCCESS)
		result = harness_pipeline(harness, &texture, scene->screen_pass, &scene->texture_pipeline);
	return result;
}

// A comparison sampler for each op, nearest, then a linear one that doesn't compare.
static VkResult make_descriptors(const Harness *harness, Scene *scene)
{
	VkResult result = VK_SUCCESS;
	for (uint32_t i = 0; i < SETS && result == VK_SUCCESS; i++) {
		const bool filtered = i == OPS;
		const VkSamplerCreateInfo info = {
			.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
			.magFilter = filtered ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
			.minFilter = filtered ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
			.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
			.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
			.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
			.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
			.compareEnable = !filtered,
			.compareOp = filtered ? VK_COMPARE_OP_NEVER : ops[i],
		};
		result = vkCreateSampler(harness->device, &info, NULL, &scene->samplers[i]);
	}
	const VkDescriptorPoolSize size = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, SETS };
	const VkDescriptorPoolCreateInfo pool_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.maxSets = SETS,
		.poolSizeCount = 1,
		.pPoolSizes = &size,
	};
	if (result == VK_SUCCESS)
		result = vkCreateDescriptorPool(harness->device, &pool_info, NULL, &scene->pool);
	VkDescriptorSetLayout layouts[SETS];
	for (uint32_t i = 0; i < SETS; i++)
		layouts[i] = scene->set_layout;
	const VkDescriptorSetAllocateInfo allocate_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool = scene->pool,
		.descriptorSetCount = SETS,
		.pSetLayouts = layouts,
	};
	if (result == VK_SUCCESS)
		result = vkAllocateDescriptorSets(harness->device, &allocate_info, scene->sets);
	for (uint32_t i = 0; i < SETS && result == VK_SUCCESS; i++) {
		const VkDescriptorImageInfo image = { scene->samplers[i], scene->sampled_view,
			VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL };
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

static bool build(const Harness *harness, Scene *scene)
{
	static float shadow_quads[OPS * QUAD_VERTICES][SHADOW_FLOATS];
	static float filtered_quad[QUAD_VERTICES][TEXTURE_FLOATS];
	fill_shadow_quads(shadow_quads);
	fill_filtered_quad(filtered_quad);
	return harness_ok("images", make_images(harness, scene)) &&
		harness_ok("depth pass", make_depth_pass(harness, scene)) &&
		harness_ok("depth pipeline", make_depth_pipeline(harness, scene)) &&
		harness_ok("screen pipelines", make_screen_pipelines(harness, scene)) &&
		harness_ok("samplers", make_descriptors(harness, scene)) &&
		harness_ok("unit quad", harness_quad_buffer(harness, &scene->unit_quad, &scene->unit_quad_memory)) &&
		harness_ok("comparison quads",
			harness_buffer(harness, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, shadow_quads, sizeof shadow_quads,
				&scene->shadow_quads, &scene->shadow_memory)) &&
		harness_ok("filtered quad",
			harness_buffer(harness, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, filtered_quad, sizeof filtered_quad,
				&scene->filtered_quad, &scene->filtered_memory));
}

// Once: the depth image cleared to the near depth, its right half drawn at the far one, and resolved to be sampled.
static bool draw_depth(const Harness *harness, Scene *scene)
{
	const VkCommandBufferAllocateInfo allocate_info = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = harness->pool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1,
	};
	VkCommandBuffer commands = VK_NULL_HANDLE;
	if (!harness_ok("depth commands", vkAllocateCommandBuffers(harness->device, &allocate_info, &commands)))
		return false;
	const VkCommandBufferBeginInfo begin = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	(void)vkBeginCommandBuffer(commands, &begin);
	const VkRenderPassBeginInfo pass = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		.renderPass = scene->depth_pass,
		.framebuffer = scene->depth_target,
		.renderArea = { { 0, 0 }, { DEPTH_WIDTH, DEPTH_HEIGHT } },
		.clearValueCount = 2,
		.pClearValues = depth_clears,
	};
	const VkViewport viewport = { 0.0f, 0.0f, DEPTH_WIDTH, DEPTH_HEIGHT, 0.0f, 1.0f };
	const VkRect2D scissor = { { 0, 0 }, { DEPTH_WIDTH, DEPTH_HEIGHT } };
	const VkDeviceSize offset = 0;
	vkCmdBeginRenderPass(commands, &pass, VK_SUBPASS_CONTENTS_INLINE);
	vkCmdSetViewport(commands, 0, 1, &viewport);
	vkCmdSetScissor(commands, 0, 1, &scissor);
	vkCmdBindVertexBuffers(commands, 0, 1, &scene->unit_quad, &offset);
	vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, scene->depth_pipeline);
	harness_push_quad_in(commands, scene->push_layout, &far_half, DEPTH_WIDTH, DEPTH_HEIGHT);
	vkCmdEndRenderPass(commands);
	bool ok = harness_ok("depth recording", vkEndCommandBuffer(commands));
	const VkSubmitInfo submit = {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &commands
	};
	ok = ok && harness_ok("depth submit", vkQueueSubmit(harness->queue, 1, &submit, harness->fence));
	ok =
		ok && harness_ok("depth wait", vkWaitForFences(harness->device, 1, &harness->fence, VK_TRUE, FENCE_TIMEOUT_NS));
	(void)vkResetFences(harness->device, 1, &harness->fence);
	vkFreeCommandBuffers(harness->device, harness->pool, 1, &commands);
	return ok;
}

static VkResult record(const Harness *harness, Scene *scene)
{
	VkResult result = harness_command_buffers(harness, scene->commands);
	for (uint32_t i = 0; i < harness->image_count && result == VK_SUCCESS; i++) {
		const VkRenderPassBeginInfo pass = {
			.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
			.renderPass = scene->screen_pass,
			.framebuffer = scene->screens[i],
			.renderArea = { { 0, 0 }, { HARNESS_WIDTH, HARNESS_HEIGHT } },
			.clearValueCount = 1,
			.pClearValues = &screen_clear,
		};
		const VkDeviceSize offset = 0;
		const VkCommandBuffer commands = scene->commands[i];
		vkCmdBeginRenderPass(commands, &pass, VK_SUBPASS_CONTENTS_INLINE);
		vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, scene->shadow_pipeline);
		vkCmdBindVertexBuffers(commands, 0, 1, &scene->shadow_quads, &offset);
		for (uint32_t op = 0; op < OPS; op++) {
			vkCmdBindDescriptorSets(
				commands, VK_PIPELINE_BIND_POINT_GRAPHICS, scene->layout, 0, 1, &scene->sets[op], 0, NULL);
			vkCmdDraw(commands, QUAD_VERTICES, 1, op * QUAD_VERTICES, 0);
		}
		vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, scene->texture_pipeline);
		vkCmdBindVertexBuffers(commands, 0, 1, &scene->filtered_quad, &offset);
		vkCmdBindDescriptorSets(
			commands, VK_PIPELINE_BIND_POINT_GRAPHICS, scene->layout, 0, 1, &scene->sets[OPS], 0, NULL);
		vkCmdDraw(commands, QUAD_VERTICES, 1, 0, 0);
		vkCmdEndRenderPass(commands);
		result = vkEndCommandBuffer(commands);
	}
	return result;
}

static bool reports_filtered_depth(const Harness *harness)
{
	VkFormatProperties properties;
	vkGetPhysicalDeviceFormatProperties(harness->physical_device, VK_FORMAT_D24_UNORM_S8_UINT, &properties);
	return properties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
}

// xost compares after the fetch, so a comparison sampler filtering linearly would blend depths, not results.
static bool refuses_filtered_comparison(const Harness *harness)
{
	const VkSamplerCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
		.magFilter = VK_FILTER_LINEAR,
		.minFilter = VK_FILTER_LINEAR,
		.compareEnable = VK_TRUE,
		.compareOp = VK_COMPARE_OP_LESS,
	};
	VkSampler sampler = VK_NULL_HANDLE;
	return vkCreateSampler(harness->device, &info, NULL, &sampler) == VK_ERROR_FEATURE_NOT_PRESENT;
}

static void destroy(const Harness *harness, Scene *scene)
{
	const VkDevice device = harness->device;
	vkFreeCommandBuffers(device, harness->pool, harness->image_count, scene->commands);
	vkDestroyBuffer(device, scene->filtered_quad, NULL);
	vkFreeMemory(device, scene->filtered_memory, NULL);
	vkDestroyBuffer(device, scene->shadow_quads, NULL);
	vkFreeMemory(device, scene->shadow_memory, NULL);
	vkDestroyBuffer(device, scene->unit_quad, NULL);
	vkFreeMemory(device, scene->unit_quad_memory, NULL);
	vkDestroyDescriptorPool(device, scene->pool, NULL);
	for (uint32_t i = 0; i < SETS; i++)
		vkDestroySampler(device, scene->samplers[i], NULL);
	vkDestroyPipeline(device, scene->shadow_pipeline, NULL);
	vkDestroyPipeline(device, scene->texture_pipeline, NULL);
	vkDestroyPipelineLayout(device, scene->layout, NULL);
	vkDestroyDescriptorSetLayout(device, scene->set_layout, NULL);
	harness_destroy_render_pass(harness, scene->screen_pass, scene->screens);
	vkDestroyPipeline(device, scene->depth_pipeline, NULL);
	vkDestroyPipelineLayout(device, scene->push_layout, NULL);
	vkDestroyFramebuffer(device, scene->depth_target, NULL);
	vkDestroyRenderPass(device, scene->depth_pass, NULL);
	vkDestroyImageView(device, scene->sampled_view, NULL);
	vkDestroyImageView(device, scene->depth_view, NULL);
	vkDestroyImage(device, scene->depth, NULL);
	vkFreeMemory(device, scene->depth_memory, NULL);
	vkDestroyImageView(device, scene->color_view, NULL);
	vkDestroyImage(device, scene->color, NULL);
	vkFreeMemory(device, scene->color_memory, NULL);
}

void test_shadow(const Harness *harness)
{
	Scene scene = { .depth_pass = VK_NULL_HANDLE };
	make_probes(&scene);
	const bool built = build(harness, &scene);
	check("shadow: a D24S8 image to sample, comparison samplers and their pipelines made", built);
	check("shadow: the device reports D24S8 filtered linearly", reports_filtered_depth(harness));
	check("shadow: comparison samplers that filter linearly are refused", refuses_filtered_comparison(harness));
	const bool drawn = built && draw_depth(harness, &scene);
	const bool recorded = drawn && harness->swapchain && harness_ok("recording", record(harness, &scene));
	uint32_t misses = 0;
	const uint32_t frames =
		recorded ? harness_frames(harness, scene.commands, FRAMES, harness_probes_hold, &scene.compared, &misses) : 0;
	check("shadow: 30 frames drawn and presented", frames == FRAMES);
	check("shadow: every frame compares 0.5 with depths of 0.25 and 0.75 by less, greater, never and always",
		frames == FRAMES && misses == 0);
	check("shadow: the image on screen shows them too",
		frames == FRAMES && harness_screen(harness, harness_probes_hold, &scene.compared));
	check("shadow: D24S8 filtered linearly across the edge between depths 0.25 and 0.75",
		frames == FRAMES && harness_screen(harness, harness_probes_hold, &scene.filtered));
	destroy(harness, &scene);
}
