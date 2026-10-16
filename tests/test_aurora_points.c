#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "aurora_points.h"
#include "harness.h"
#include "tests.h"
#include "xsdk/log.h"

// Aurora's GLSL for GX configs (milestone 18), each drawn as a GX point, its quad's six corners from one vertex, so
// every interpolant holds that vertex's value across it, and checked at the point's middle against the colour
// tools/gen_aurora_points.py worked out from Aurora's reference of its WGSL. A Z texture's point writes depth, always;
// over its middle a quad a little below that depth tests greater-than and one a little above less-than, so either
// shows only if the point wrote a depth outside the two.

enum {
	CASES = sizeof aurora_points / sizeof aurora_points[0],
	FRAMES = 3,
	FLOAT_BYTES = 4,
	VEC4_BYTES = 16,
	PUSH_BYTES = AURORA_PUSH_VEC4S * VEC4_BYTES,
	TEXEL_BYTES = 4,
	IMAGE_BYTES = AURORA_GRADIENT * AURORA_GRADIENT * TEXEL_BYTES,
	FENCE_TIMEOUT_NS = 1000000000,
	// Each channel within 2 of the reference: the sampler's weights and the TEV's sums each round once or twice.
	TOLERANCE = 2,
	PROBES = 2,
	SET_COUNT = 2,
	DEPTH_QUAD_WIDTH = 8,
};

// D24's steps the depth may be off by: the program's floats hold the depth's sum to a few (Aurora's
// lib/vulkan4xenon/glsl.cpp, z_texture), and the depth and the quads' each round once.
static const float depth_margin = 32.0f / 0xFFFFFF;

// A dark grey: 24 in each channel, and depth 0.
static const VkClearValue clears[2] = { { .color = { .float32 = { 24.0f / 255, 24.0f / 255, 24.0f / 255, 1.0f } } },
	{ .depthStencil = { 0.0f, 0 } } };
static const Rgb clear_rgb = { 24, 24, 24 };

// Map 0's texels, too big for a stack: gen_aurora_points.py's wave in red and green.
static uint8_t gradient[IMAGE_BYTES];

typedef struct {
	VkImage depth;
	VkDeviceMemory depth_memory;
	VkImageView depth_view;
	VkRenderPass render_pass;
	VkFramebuffer framebuffers[HARNESS_MAX_IMAGES];
	// The quads over a Z texture's point: below its depth testing greater-than, above it less-than.
	VkPipelineLayout quad_layout;
	VkPipeline below;
	VkPipeline above;
	VkBuffer quad;
	VkDeviceMemory quad_memory;
	VkDescriptorSetLayout set_layouts[SET_COUNT];
	VkPipelineLayout layout;
	VkImage image;
	VkDeviceMemory image_memory;
	VkImageView view;
	VkSampler sampler;
	VkBuffer staging;
	VkDeviceMemory staging_memory;
	// A block of zeros for each piece a case's programs don't read.
	VkBuffer unread;
	VkDeviceMemory unread_memory;
	VkDescriptorPool pool;
} Shared;

typedef struct {
	VkPipeline pipeline;
	VkBuffer blocks[AURORA_BLOCKS];
	VkDeviceMemory block_memory[AURORA_BLOCKS];
	VkBuffer vertices;
	VkDeviceMemory vertex_memory;
	VkDescriptorSet sets[SET_COUNT];
	VkCommandBuffer commands[HARNESS_MAX_IMAGES];
	HarnessProbe probes[PROBES];
	HarnessProbes check;
} Case;

static uint8_t wave(uint32_t x)
{
	const int32_t value = 2 * (int32_t)x - (AURORA_GRADIENT - 1);
	return (uint8_t)(value < 0 ? -value : value);
}

static void fill_gradient(void)
{
	for (uint32_t y = 0; y < AURORA_GRADIENT; y++) {
		for (uint32_t x = 0; x < AURORA_GRADIENT; x++) {
			uint8_t *const texel = gradient + (y * AURORA_GRADIENT + x) * TEXEL_BYTES;
			texel[0] = wave(x), texel[1] = wave(y), texel[2] = AURORA_GRADIENT_BLUE, texel[3] = 0xFF;
		}
	}
}

// Set 0, the uniform pieces at their bindings; set 1, map 0; the vertex stage's push constants.
static VkResult create_layout(const Harness *harness, Shared *shared)
{
	VkDescriptorSetLayoutBinding blocks[AURORA_BLOCKS];
	for (uint32_t i = 0; i < AURORA_BLOCKS; i++)
		blocks[i] = (VkDescriptorSetLayoutBinding){ .binding = i,
			.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
			.descriptorCount = 1,
			.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT };
	const VkDescriptorSetLayoutBinding map = { .binding = 0,
		.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		.descriptorCount = 1,
		.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT };
	const VkDescriptorSetLayoutCreateInfo infos[SET_COUNT] = {
		{ .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
			.bindingCount = AURORA_BLOCKS,
			.pBindings = blocks },
		{ .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &map },
	};
	VkResult result = VK_SUCCESS;
	for (uint32_t i = 0; i < SET_COUNT && result == VK_SUCCESS; i++)
		result = vkCreateDescriptorSetLayout(harness->device, &infos[i], NULL, &shared->set_layouts[i]);
	const VkPushConstantRange push = { VK_SHADER_STAGE_VERTEX_BIT, 0, PUSH_BYTES };
	const VkPipelineLayoutCreateInfo layout_info = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = SET_COUNT,
		.pSetLayouts = shared->set_layouts,
		.pushConstantRangeCount = 1,
		.pPushConstantRanges = &push,
	};
	if (result == VK_SUCCESS)
		result = vkCreatePipelineLayout(harness->device, &layout_info, NULL, &shared->layout);
	const VkDescriptorPoolSize sizes[] = {
		{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, CASES * AURORA_BLOCKS },
		{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, CASES },
	};
	const VkDescriptorPoolCreateInfo pool_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.maxSets = CASES * SET_COUNT,
		.poolSizeCount = 2,
		.pPoolSizes = sizes,
	};
	if (result == VK_SUCCESS)
		result = vkCreateDescriptorPool(harness->device, &pool_info, NULL, &shared->pool);
	return result;
}

// Map 0: the gradient, sampled bilinear and repeating, its texel centres at (i + 0.5) / AURORA_GRADIENT.
static VkResult create_map(const Harness *harness, Shared *shared)
{
	VkResult result = harness_image(harness, VK_FORMAT_R8G8B8A8_UNORM, AURORA_GRADIENT, AURORA_GRADIENT,
		VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, &shared->image, &shared->image_memory,
		&shared->view);
	const VkSamplerCreateInfo sampler_info = {
		.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
		.magFilter = VK_FILTER_LINEAR,
		.minFilter = VK_FILTER_LINEAR,
		.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
		.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
		.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
		.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
	};
	if (result == VK_SUCCESS)
		result = vkCreateSampler(harness->device, &sampler_info, NULL, &shared->sampler);
	fill_gradient();
	if (result == VK_SUCCESS)
		result = harness_buffer(harness, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, gradient, IMAGE_BYTES, &shared->staging,
			&shared->staging_memory);
	static const float zeros[4] = { 0 };
	if (result == VK_SUCCESS)
		result = harness_buffer(
			harness, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, zeros, sizeof zeros, &shared->unread, &shared->unread_memory);
	return result;
}

static void transition(VkCommandBuffer commands, VkImage image, VkImageLayout from, VkImageLayout to)
{
	const VkImageMemoryBarrier barrier = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		.srcAccessMask = from == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_TRANSFER_WRITE_BIT,
		.dstAccessMask =
			to == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL ? VK_ACCESS_TRANSFER_WRITE_BIT : VK_ACCESS_SHADER_READ_BIT,
		.oldLayout = from,
		.newLayout = to,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = image,
		.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
	};
	vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0,
		NULL, 1, &barrier);
}

static bool upload(const Harness *harness, const Shared *shared)
{
	const VkCommandBufferAllocateInfo allocate_info = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = harness->pool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1,
	};
	VkCommandBuffer commands = VK_NULL_HANDLE;
	if (!harness_ok("upload command buffer", vkAllocateCommandBuffers(harness->device, &allocate_info, &commands)))
		return false;
	const VkCommandBufferBeginInfo begin = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	(void)vkBeginCommandBuffer(commands, &begin);
	const VkBufferImageCopy region = {
		.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
		.imageExtent = { AURORA_GRADIENT, AURORA_GRADIENT, 1 },
	};
	transition(commands, shared->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	vkCmdCopyBufferToImage(commands, shared->staging, shared->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
	transition(commands, shared->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	bool ok = harness_ok("upload recording", vkEndCommandBuffer(commands));
	const VkSubmitInfo submit = {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &commands
	};
	ok = ok && harness_ok("upload submit", vkQueueSubmit(harness->queue, 1, &submit, harness->fence));
	ok = ok &&
		harness_ok("upload wait", vkWaitForFences(harness->device, 1, &harness->fence, VK_TRUE, FENCE_TIMEOUT_NS));
	(void)vkResetFences(harness->device, 1, &harness->fence);
	vkFreeCommandBuffers(harness->device, harness->pool, 1, &commands);
	return ok;
}

static VkResult create_depth_quad(
	const Harness *harness, const Shared *shared, VkCompareOp compare, VkPipeline *pipeline)
{
	const VkPipelineDepthStencilStateCreateInfo depth_stencil = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
		.depthTestEnable = VK_TRUE,
		.depthCompareOp = compare,
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
		.layout = shared->quad_layout,
		.depth_stencil = &depth_stencil,
	};
	return harness_pipeline(harness, &spec, shared->render_pass, pipeline);
}

// The depth attachment, its pass, and the quads over a Z texture's point.
static VkResult create_depth(const Harness *harness, Shared *shared)
{
	VkResult result = harness_depth_image(harness, &shared->depth, &shared->depth_memory, &shared->depth_view);
	if (result == VK_SUCCESS)
		result = harness_depth_render_pass(harness, shared->depth_view, &shared->render_pass, shared->framebuffers);
	if (result == VK_SUCCESS)
		result = harness_push_layout(harness, &shared->quad_layout);
	if (result == VK_SUCCESS)
		result = create_depth_quad(harness, shared, VK_COMPARE_OP_GREATER, &shared->below);
	if (result == VK_SUCCESS)
		result = create_depth_quad(harness, shared, VK_COMPARE_OP_LESS, &shared->above);
	if (result == VK_SUCCESS)
		result = harness_quad_buffer(harness, &shared->quad, &shared->quad_memory);
	return result;
}

// The point's six corners, each the one vertex with its corner: right when odd, lower from 2, as Aurora's
// write_corners orders them; the attributes packed in their order.
static VkResult create_vertices(const Harness *harness, const AuroraPoint *point, Case *c,
	VkVertexInputAttributeDescription *attributes, uint32_t *stride)
{
	static const uint32_t order[AURORA_CORNERS] = { 0, 1, 3, 3, 2, 0 };
	float data[AURORA_CORNERS][AURORA_MAX_ATTRIBUTES * 3];
	uint32_t offset = 0;
	for (uint32_t a = 0; a < point->attribute_count; a++) {
		const AuroraAttribute *const attribute = &point->attributes[a];
		static const VkFormat formats[] = { VK_FORMAT_UNDEFINED, VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32G32_SFLOAT,
			VK_FORMAT_R32G32B32_SFLOAT };
		attributes[a] = (VkVertexInputAttributeDescription){ attribute->location, 0, formats[attribute->components],
			offset * FLOAT_BYTES };
		for (uint32_t k = 0; k < AURORA_CORNERS; k++) {
			for (uint32_t i = 0; i < attribute->components; i++)
				data[k][offset + i] = attribute->value[i];
			if (attribute->corner) {
				data[k][offset] = (order[k] & 1) != 0 ? 1.0f : -1.0f;
				data[k][offset + 1] = order[k] >= 2 ? 1.0f : -1.0f;
			}
		}
		offset += attribute->components;
	}
	*stride = offset * FLOAT_BYTES;
	// Packed tight: each corner's floats after the last's.
	float packed[AURORA_CORNERS * AURORA_MAX_ATTRIBUTES * 3];
	for (uint32_t k = 0; k < AURORA_CORNERS; k++)
		memcpy(packed + k * offset, data[k], *stride);
	return harness_buffer(
		harness, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, packed, AURORA_CORNERS * *stride, &c->vertices, &c->vertex_memory);
}

static VkResult create_case(const Harness *harness, const Shared *shared, const AuroraPoint *point, Case *c)
{
	VkResult result = VK_SUCCESS;
	for (uint32_t b = 0; b < AURORA_BLOCKS && result == VK_SUCCESS; b++) {
		if (point->blocks[b])
			result = harness_buffer(harness, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, point->blocks[b],
				point->block_vec4s[b] * VEC4_BYTES, &c->blocks[b], &c->block_memory[b]);
	}
	VkVertexInputAttributeDescription attributes[AURORA_MAX_ATTRIBUTES];
	uint32_t stride = 0;
	if (result == VK_SUCCESS)
		result = create_vertices(harness, point, c, attributes, &stride);
	const VkVertexInputBindingDescription binding = { 0, stride, VK_VERTEX_INPUT_RATE_VERTEX };
	const VkPipelineDepthStencilStateCreateInfo writes_depth = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
		.depthTestEnable = VK_TRUE,
		.depthWriteEnable = VK_TRUE,
		.depthCompareOp = VK_COMPARE_OP_ALWAYS,
	};
	const HarnessPipeline spec = {
		.vertex_spirv = point->vertex_spirv,
		.vertex_bytes = point->vertex_bytes,
		.fragment_spirv = point->fragment_spirv,
		.fragment_bytes = point->fragment_bytes,
		.bindings = &binding,
		.binding_count = 1,
		.attributes = attributes,
		.attribute_count = point->attribute_count,
		.layout = shared->layout,
		.depth_stencil = point->depth >= 0.0f ? &writes_depth : NULL,
	};
	if (result == VK_SUCCESS)
		result = harness_pipeline(harness, &spec, shared->render_pass, &c->pipeline);
	const VkDescriptorSetAllocateInfo allocate_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool = shared->pool,
		.descriptorSetCount = SET_COUNT,
		.pSetLayouts = shared->set_layouts,
	};
	if (result == VK_SUCCESS)
		result = vkAllocateDescriptorSets(harness->device, &allocate_info, c->sets);
	if (result != VK_SUCCESS)
		return result;
	VkDescriptorBufferInfo buffers[AURORA_BLOCKS];
	VkWriteDescriptorSet writes[AURORA_BLOCKS + 1];
	for (uint32_t b = 0; b < AURORA_BLOCKS; b++) {
		buffers[b] = point->blocks[b] ? (VkDescriptorBufferInfo){ c->blocks[b], 0, point->block_vec4s[b] * VEC4_BYTES }
									  : (VkDescriptorBufferInfo){ shared->unread, 0, VEC4_BYTES };
		writes[b] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstSet = c->sets[0],
			.dstBinding = b,
			.descriptorCount = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
			.pBufferInfo = &buffers[b] };
	}
	const VkDescriptorImageInfo image = { shared->sampler, shared->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
	writes[AURORA_BLOCKS] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		.dstSet = c->sets[1],
		.dstBinding = 0,
		.descriptorCount = 1,
		.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		.pImageInfo = &image };
	vkUpdateDescriptorSets(harness->device, AURORA_BLOCKS + 1, writes, 0, NULL);
	return VK_SUCCESS;
}

static void record_depth_quads(const Shared *shared, const AuroraPoint *point, VkCommandBuffer commands)
{
	const float x = (float)point->x - DEPTH_QUAD_WIDTH / 2;
	const float y = (float)point->y - DEPTH_QUAD_WIDTH / 2;
	const HarnessQuad below = { x, y, DEPTH_QUAD_WIDTH, DEPTH_QUAD_WIDTH, point->depth - depth_margin,
		{ 1.0f, 0.0f, 1.0f, 1.0f }, false };
	const HarnessQuad above = { x, y, DEPTH_QUAD_WIDTH, DEPTH_QUAD_WIDTH, point->depth + depth_margin,
		{ 0.0f, 1.0f, 1.0f, 1.0f }, false };
	const VkDeviceSize offset = 0;
	vkCmdBindVertexBuffers(commands, 0, 1, &shared->quad, &offset);
	vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, shared->below);
	harness_push_quad(commands, shared->quad_layout, &below);
	vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, shared->above);
	harness_push_quad(commands, shared->quad_layout, &above);
}

static VkResult record(const Harness *harness, const Shared *shared, const AuroraPoint *point, Case *c)
{
	VkResult result = harness_command_buffers(harness, c->commands);
	for (uint32_t i = 0; i < harness->image_count && result == VK_SUCCESS; i++) {
		const VkRenderPassBeginInfo pass = {
			.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
			.renderPass = shared->render_pass,
			.framebuffer = shared->framebuffers[i],
			.renderArea = { { 0, 0 }, { HARNESS_WIDTH, HARNESS_HEIGHT } },
			.clearValueCount = 2,
			.pClearValues = clears,
		};
		const VkDeviceSize offset = 0;
		const VkCommandBuffer commands = c->commands[i];
		vkCmdBeginRenderPass(commands, &pass, VK_SUBPASS_CONTENTS_INLINE);
		vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, c->pipeline);
		vkCmdBindDescriptorSets(
			commands, VK_PIPELINE_BIND_POINT_GRAPHICS, shared->layout, 0, SET_COUNT, c->sets, 0, NULL);
		vkCmdPushConstants(commands, shared->layout, VK_SHADER_STAGE_VERTEX_BIT, 0, PUSH_BYTES, point->push);
		vkCmdBindVertexBuffers(commands, 0, 1, &c->vertices, &offset);
		vkCmdDraw(commands, AURORA_CORNERS, 1, 0, 0);
		if (point->depth >= 0.0f)
			record_depth_quads(shared, point, commands);
		vkCmdEndRenderPass(commands);
		result = vkEndCommandBuffer(commands);
	}
	return result;
}

static void destroy_case(const Harness *harness, Case *c)
{
	const VkDevice device = harness->device;
	vkFreeCommandBuffers(device, harness->pool, harness->image_count, c->commands);
	for (uint32_t b = 0; b < AURORA_BLOCKS; b++) {
		vkDestroyBuffer(device, c->blocks[b], NULL);
		vkFreeMemory(device, c->block_memory[b], NULL);
	}
	vkDestroyBuffer(device, c->vertices, NULL);
	vkFreeMemory(device, c->vertex_memory, NULL);
	vkDestroyPipeline(device, c->pipeline, NULL);
}

static void destroy(const Harness *harness, Shared *shared)
{
	const VkDevice device = harness->device;
	vkDestroyDescriptorPool(device, shared->pool, NULL);
	vkDestroySampler(device, shared->sampler, NULL);
	vkDestroyImageView(device, shared->view, NULL);
	vkDestroyImage(device, shared->image, NULL);
	vkFreeMemory(device, shared->image_memory, NULL);
	vkDestroyBuffer(device, shared->staging, NULL);
	vkFreeMemory(device, shared->staging_memory, NULL);
	vkDestroyBuffer(device, shared->unread, NULL);
	vkFreeMemory(device, shared->unread_memory, NULL);
	vkDestroyPipelineLayout(device, shared->layout, NULL);
	for (uint32_t i = 0; i < SET_COUNT; i++)
		vkDestroyDescriptorSetLayout(device, shared->set_layouts[i], NULL);
	vkDestroyBuffer(device, shared->quad, NULL);
	vkFreeMemory(device, shared->quad_memory, NULL);
	vkDestroyPipeline(device, shared->above, NULL);
	vkDestroyPipeline(device, shared->below, NULL);
	vkDestroyPipelineLayout(device, shared->quad_layout, NULL);
	harness_destroy_render_pass(harness, shared->render_pass, shared->framebuffers);
	vkDestroyImageView(device, shared->depth_view, NULL);
	vkDestroyImage(device, shared->depth, NULL);
	vkFreeMemory(device, shared->depth_memory, NULL);
}

void test_aurora_points(const Harness *harness)
{
	Shared shared = { .render_pass = VK_NULL_HANDLE };
	bool ok = harness_ok("depth and its pass", create_depth(harness, &shared));
	ok = ok && harness_ok("layouts", create_layout(harness, &shared));
	ok = ok && harness_ok("map 0", create_map(harness, &shared));
	ok = ok && upload(harness, &shared);
	check("aurora points: the depth, layouts, map 0's gradient and its sampler made", ok);
	for (uint32_t p = 0; ok && harness->swapchain && p < CASES; p++) {
		const AuroraPoint *const point = &aurora_points[p];
		Case c = { .pipeline = VK_NULL_HANDLE };
		c.probes[0] = (HarnessProbe){ "the point's middle", point->x, point->y, point->want, 0 };
		c.probes[1] = (HarnessProbe){ "past the point", point->x + AURORA_POINT_WIDTH, point->y, clear_rgb, 0 };
		c.check = (HarnessProbes){ point->name, c.probes, PROBES, TOLERANCE, 0 };
		const bool built = harness_ok(point->name, create_case(harness, &shared, point, &c)) &&
			harness_ok("recording", record(harness, &shared, point, &c));
		uint32_t misses = 0;
		const uint32_t frames =
			built ? harness_frames(harness, c.commands, FRAMES, harness_probes_hold, &c.check, &misses) : 0;
		xsdk_log_write("aurora points: %s at (%u, %u), wants %u %u %u", point->name, (unsigned)point->x,
			(unsigned)point->y, (unsigned)point->want.red, (unsigned)point->want.green, (unsigned)point->want.blue);
		char name[96];
		strcpy(name, "aurora points: ");
		strcat(name, point->name);
		strcat(name, ": the point's middle holds the WGSL's colour");
		if (point->depth >= 0.0f)
			strcat(name, " and depth");
		check(name, built && frames == FRAMES && misses == 0);
		destroy_case(harness, &c);
	}
	destroy(harness, &shared);
}
