#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "harness.h"
#include "shaders.h"
#include "tests.h"
#include "xsdk/log.h"

enum {
	FRAMES = 30,
	IMAGES = 4,
	TEXEL_BYTES = 4,
	MAX_WIDTH = 256,
	STAGING_BYTES = (16 + 64 + 204 + 256) * TEXEL_BYTES,
	VERTEX_FLOATS = 6,
	STRIDE_BYTES = VERTEX_FLOATS * 4,
	UV_OFFSET_BYTES = 16,
	QUAD_VERTICES = 6,
	// Each strip starts at x 64, 4 pixels a texel, 40 pixels high, a strip every 120 pixels from y 80.
	LEFT_X = 64,
	TEXEL_PIXELS = 4,
	STRIP_HEIGHT = 40,
	TOP_Y = 80,
	STRIP_STEP_Y = 120,
	TOLERANCE = 1,
};

// One texel high, as palettes are: 204 is Dusklight's minimap's (docs/ROADMAP.md, milestone 14).
static const uint32_t widths[IMAGES] = { 16, 64, 204, 256 };
static const char *const names[IMAGES] = { "16x1", "64x1", "204x1", "256x1" };

static const VkVertexInputBindingDescription binding = { 0, STRIDE_BYTES, VK_VERTEX_INPUT_RATE_VERTEX };
static const VkVertexInputAttributeDescription attributes[] = {
	{ .location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = 0 },
	{ .location = 1, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = UV_OFFSET_BYTES },
};
static const VkClearValue clear = { .color = { .float32 = { 24.0f / 255, 24.0f / 255, 24.0f / 255, 1.0f } } };

typedef struct {
	VkDescriptorSetLayout set_layout;
	VkPipelineLayout layout;
	VkRenderPass render_pass;
	VkFramebuffer framebuffers[HARNESS_MAX_IMAGES];
	VkPipeline pipeline;
	VkImage images[IMAGES];
	VkDeviceMemory image_memory[IMAGES];
	VkImageView views[IMAGES];
	VkSampler sampler;
	VkDescriptorPool pool;
	VkDescriptorSet sets[IMAGES];
	VkBuffer vertices;
	VkDeviceMemory vertex_memory;
	VkBuffer staging;
	VkDeviceMemory staging_memory;
	VkCommandBuffer commands[HARNESS_MAX_IMAGES];
	HarnessProbe probes[IMAGES][MAX_WIDTH];
	HarnessProbes checks[IMAGES];
} Scene;

// Each texel of an image a colour of its own: red counts up, green down, blue in steps of 7.
static Rgb texel(uint32_t image, uint32_t x)
{
	return (Rgb){ x & 0xFF, (255 - x + image * 16) & 0xFF, (x * 7 + image * 64) & 0xFF };
}

static uint32_t staging_offset(uint32_t image)
{
	uint32_t offset = 0;
	for (uint32_t i = 0; i < image; i++)
		offset += widths[i] * TEXEL_BYTES;
	return offset;
}

static void fill_staging(uint8_t *bytes)
{
	for (uint32_t i = 0; i < IMAGES; i++) {
		uint8_t *const row = bytes + staging_offset(i);
		for (uint32_t x = 0; x < widths[i]; x++) {
			const Rgb rgb = texel(i, x);
			row[x * TEXEL_BYTES] = (uint8_t)rgb.red;
			row[x * TEXEL_BYTES + 1] = (uint8_t)rgb.green;
			row[x * TEXEL_BYTES + 2] = (uint8_t)rgb.blue;
			row[x * TEXEL_BYTES + 3] = 0xFF;
		}
	}
}

// A strip's two triangles in clip space, with the image's texture coordinates.
static void strip(uint32_t image, float vertices[QUAD_VERTICES][VERTEX_FLOATS])
{
	const float left = (float)LEFT_X / (HARNESS_WIDTH / 2) - 1.0f;
	const float right = (float)(LEFT_X + widths[image] * TEXEL_PIXELS) / (HARNESS_WIDTH / 2) - 1.0f;
	const float top = (float)(TOP_Y + image * STRIP_STEP_Y) / (HARNESS_HEIGHT / 2) - 1.0f;
	const float bottom = (float)(TOP_Y + image * STRIP_STEP_Y + STRIP_HEIGHT) / (HARNESS_HEIGHT / 2) - 1.0f;
	const float corners[QUAD_VERTICES][VERTEX_FLOATS] = {
		{ left, top, 0.0f, 1.0f, 0.0f, 0.0f },
		{ right, top, 0.0f, 1.0f, 1.0f, 0.0f },
		{ left, bottom, 0.0f, 1.0f, 0.0f, 1.0f },
		{ right, top, 0.0f, 1.0f, 1.0f, 0.0f },
		{ right, bottom, 0.0f, 1.0f, 1.0f, 1.0f },
		{ left, bottom, 0.0f, 1.0f, 0.0f, 1.0f },
	};
	memcpy(vertices, corners, sizeof corners);
}

static void make_probes(Scene *scene)
{
	for (uint32_t i = 0; i < IMAGES; i++) {
		for (uint32_t x = 0; x < widths[i]; x++) {
			scene->probes[i][x] = (HarnessProbe){ names[i], LEFT_X + x * TEXEL_PIXELS + TEXEL_PIXELS / 2,
				TOP_Y + i * STRIP_STEP_Y + STRIP_HEIGHT / 2, texel(i, x), 0 };
		}
		scene->checks[i] = (HarnessProbes){ "thin textures", scene->probes[i], widths[i], TOLERANCE, 0 };
	}
}

static VkResult create_image(const Harness *harness, Scene *scene, uint32_t i)
{
	return harness_image(harness, VK_FORMAT_R8G8B8A8_UNORM, widths[i], 1,
		VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, &scene->images[i], &scene->image_memory[i],
		&scene->views[i]);
}

static VkResult create_descriptors(const Harness *harness, Scene *scene)
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
	const VkDescriptorPoolSize size = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, IMAGES };
	const VkDescriptorPoolCreateInfo pool_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.maxSets = IMAGES,
		.poolSizeCount = 1,
		.pPoolSizes = &size,
	};
	if (result == VK_SUCCESS)
		result = vkCreateDescriptorPool(harness->device, &pool_info, NULL, &scene->pool);
	const VkDescriptorSetLayout layouts[IMAGES] = { scene->set_layout, scene->set_layout, scene->set_layout,
		scene->set_layout };
	const VkDescriptorSetAllocateInfo allocate_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool = scene->pool,
		.descriptorSetCount = IMAGES,
		.pSetLayouts = layouts,
	};
	if (result == VK_SUCCESS)
		result = vkAllocateDescriptorSets(harness->device, &allocate_info, scene->sets);
	if (result != VK_SUCCESS)
		return result;
	for (uint32_t i = 0; i < IMAGES; i++) {
		const VkDescriptorImageInfo image = { scene->sampler, scene->views[i],
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
		const VkWriteDescriptorSet write = {
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstSet = scene->sets[i],
			.dstBinding = 0,
			.descriptorCount = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			.pImageInfo = &image,
		};
		vkUpdateDescriptorSets(harness->device, 1, &write, 0, NULL);
	}
	return VK_SUCCESS;
}

static VkResult create_pipeline(const Harness *harness, Scene *scene)
{
	const VkDescriptorSetLayoutBinding binding_info = {
		.binding = 0,
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
	const HarnessPipeline spec = {
		.vertex_spirv = texture_vert_spirv,
		.vertex_bytes = TEXTURE_VERT_SPIRV_BYTES,
		.fragment_spirv = texture_frag_spirv,
		.fragment_bytes = TEXTURE_FRAG_SPIRV_BYTES,
		.bindings = &binding,
		.binding_count = 1,
		.attributes = attributes,
		.attribute_count = 2,
		.layout = scene->layout,
	};
	if (result == VK_SUCCESS)
		result = harness_render_pass(harness, &scene->render_pass, scene->framebuffers);
	if (result == VK_SUCCESS)
		result = harness_pipeline(harness, &spec, scene->render_pass, &scene->pipeline);
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
	vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL,
		0, NULL, 1, &barrier);
}

// One submission copies every image's texels in.
static bool upload(const Harness *harness, const Scene *scene)
{
	VkCommandBuffer commands;
	const VkCommandBufferAllocateInfo allocate_info = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = harness->pool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1,
	};
	if (!harness_ok("upload commands", vkAllocateCommandBuffers(harness->device, &allocate_info, &commands)))
		return false;
	const VkCommandBufferBeginInfo begin = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	bool ok = harness_ok("vkBeginCommandBuffer", vkBeginCommandBuffer(commands, &begin));
	for (uint32_t i = 0; ok && i < IMAGES; i++) {
		transition(commands, scene->images[i], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		const VkBufferImageCopy region = {
			.bufferOffset = staging_offset(i),
			.bufferRowLength = widths[i],
			.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
			.imageExtent = { widths[i], 1, 1 },
		};
		vkCmdCopyBufferToImage(
			commands, scene->staging, scene->images[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
		transition(
			commands, scene->images[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	}
	ok = ok && harness_ok("vkEndCommandBuffer", vkEndCommandBuffer(commands));
	const VkSubmitInfo submit = {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.commandBufferCount = 1,
		.pCommandBuffers = &commands,
	};
	ok = ok && harness_ok("vkResetFences", vkResetFences(harness->device, 1, &harness->fence)) &&
		harness_ok("vkQueueSubmit", vkQueueSubmit(harness->queue, 1, &submit, harness->fence)) &&
		harness_ok("vkWaitForFences", vkWaitForFences(harness->device, 1, &harness->fence, VK_TRUE, UINT64_MAX));
	vkFreeCommandBuffers(harness->device, harness->pool, 1, &commands);
	return ok;
}

static bool build(const Harness *harness, Scene *scene)
{
	static uint8_t staging[STAGING_BYTES];
	static float vertices[IMAGES][QUAD_VERTICES][VERTEX_FLOATS];
	fill_staging(staging);
	for (uint32_t i = 0; i < IMAGES; i++)
		strip(i, vertices[i]);
	bool ok = harness_ok("pipeline", create_pipeline(harness, scene));
	for (uint32_t i = 0; i < IMAGES; i++)
		ok = ok && harness_ok(names[i], create_image(harness, scene, i));
	ok = ok && harness_ok("descriptors", create_descriptors(harness, scene));
	ok = ok &&
		harness_ok("vertex buffer",
			harness_buffer(harness, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, vertices, sizeof vertices, &scene->vertices,
				&scene->vertex_memory));
	return ok &&
		harness_ok("staging buffer",
			harness_buffer(harness, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, staging, STAGING_BYTES, &scene->staging,
				&scene->staging_memory));
}

static VkResult record(const Harness *harness, Scene *scene)
{
	VkResult result = harness_command_buffers(harness, scene->commands);
	for (uint32_t frame = 0; result == VK_SUCCESS && frame < harness->image_count; frame++) {
		const VkCommandBuffer commands = scene->commands[frame];
		const VkRenderPassBeginInfo begin = {
			.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
			.renderPass = scene->render_pass,
			.framebuffer = scene->framebuffers[frame],
			.renderArea = { { 0, 0 }, { HARNESS_WIDTH, HARNESS_HEIGHT } },
			.clearValueCount = 1,
			.pClearValues = &clear,
		};
		vkCmdBeginRenderPass(commands, &begin, VK_SUBPASS_CONTENTS_INLINE);
		vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, scene->pipeline);
		const VkDeviceSize offset = 0;
		vkCmdBindVertexBuffers(commands, 0, 1, &scene->vertices, &offset);
		for (uint32_t i = 0; i < IMAGES; i++) {
			vkCmdBindDescriptorSets(
				commands, VK_PIPELINE_BIND_POINT_GRAPHICS, scene->layout, 0, 1, &scene->sets[i], 0, NULL);
			vkCmdDraw(commands, QUAD_VERTICES, 1, i * QUAD_VERTICES, 0);
		}
		vkCmdEndRenderPass(commands);
		result = vkEndCommandBuffer(commands);
	}
	return result;
}

static void destroy(const Harness *harness, Scene *scene)
{
	vkDestroyPipeline(harness->device, scene->pipeline, NULL);
	harness_destroy_render_pass(harness, scene->render_pass, scene->framebuffers);
	vkDestroyPipelineLayout(harness->device, scene->layout, NULL);
	vkDestroyDescriptorPool(harness->device, scene->pool, NULL);
	vkDestroyDescriptorSetLayout(harness->device, scene->set_layout, NULL);
	vkDestroySampler(harness->device, scene->sampler, NULL);
	for (uint32_t i = 0; i < IMAGES; i++) {
		vkDestroyImageView(harness->device, scene->views[i], NULL);
		vkDestroyImage(harness->device, scene->images[i], NULL);
		vkFreeMemory(harness->device, scene->image_memory[i], NULL);
	}
	vkDestroyBuffer(harness->device, scene->vertices, NULL);
	vkFreeMemory(harness->device, scene->vertex_memory, NULL);
	vkDestroyBuffer(harness->device, scene->staging, NULL);
	vkFreeMemory(harness->device, scene->staging_memory, NULL);
}

// One image's texels on screen, how many are wrong logged, and the first few of them.
static bool shape_holds(uint32_t address, const char *what, void *context)
{
	HarnessProbes *const checks = context;
	uint32_t wrong = 0;
	for (uint32_t i = 0; i < checks->count; i++) {
		const HarnessProbe *const probe = &checks->probes[i];
		wrong += !harness_near(harness_pixel(address, probe->x, probe->y), probe->want, checks->tolerance);
	}
	xsdk_log_write("thin textures: %s: %u of %u texels wrong on screen", checks->probes[0].name, (unsigned)wrong,
		(unsigned)checks->count);
	return harness_probes_hold(address, what, context) && wrong == 0;
}

static bool all_hold(uint32_t address, const char *what, void *context)
{
	Scene *const scene = context;
	bool held = true;
	for (uint32_t i = 0; i < IMAGES; i++)
		held = harness_probes_hold(address, what, &scene->checks[i]) && held;
	return held;
}

void test_thin_textures(const Harness *harness)
{
	static Scene scene;
	memset(&scene, 0, sizeof scene);
	make_probes(&scene);
	const bool built = build(harness, &scene) && upload(harness, &scene) && harness->swapchain &&
		harness_ok("recording", record(harness, &scene));
	check("thin textures: four images one texel high made and copied in", built);
	uint32_t misses = 0;
	const uint32_t frames = built ? harness_frames(harness, scene.commands, FRAMES, all_hold, &scene, &misses) : 0;
	check("thin textures: 30 frames drawn", frames == FRAMES);
	for (uint32_t i = 0; i < IMAGES; i++) {
		scene.checks[i].logged = 0;
		const bool held = frames == FRAMES && harness_screen(harness, shape_holds, &scene.checks[i]);
		char name[96];
		(void)snprintf(name, sizeof name, "thin textures: every texel of the %s image shows on screen", names[i]);
		check(name, held);
	}
	destroy(harness, &scene);
}
