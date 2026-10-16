#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "harness.h"
#include "shaders.h"
#include "tests.h"
#include "xsdk/log.h"

// A cube image of two levels and a 3D image, copied in from a buffer face by face and slice by slice, each face and
// level and each slice its own colour, its quadrants apart, and sampled through cube and 3D views. Then a face and two
// slices copied back out, and a face copied into another cube and two of that cube's faces cleared (milestone 23).

enum {
	FRAMES = 30,
	FENCE_TIMEOUT_NS = 1000000000,
	CUBE_SIDE = 16,
	CUBE_LEVELS = 2,
	FACES = 6,
	SLICES = 8,
	TEXEL_BYTES = 4,
	// The staging buffer: the cube's level 0, its level 1, then the 3D image's slices.
	CUBE_FACE_BYTES = CUBE_SIDE * CUBE_SIDE * TEXEL_BYTES,
	CUBE_LEVEL_1_AT = FACES * CUBE_FACE_BYTES,
	SMALL_FACE_BYTES = CUBE_FACE_BYTES / 4,
	VOLUME_AT = CUBE_LEVEL_1_AT + FACES * SMALL_FACE_BYTES,
	SLICE_BYTES = CUBE_SIDE * CUBE_SIDE * TEXEL_BYTES,
	STAGING_BYTES = VOLUME_AT + SLICES * SLICE_BYTES,
	// The slices copied in two halves, the second from imageOffset.z.
	HALF_SLICES = SLICES / 2,
	// The read-back: level 1's face 3, slices 5 and 6, then the other cube's faces 0 and 1 and face 4.
	READ_FACE = 3,
	READ_SLICE = 5,
	READ_SLICES = 2,
	READ_SLICES_AT = SMALL_FACE_BYTES,
	READ_CLEARED_AT = READ_SLICES_AT + READ_SLICES * SLICE_BYTES,
	CLEARED_FACES = 2,
	READ_COPIED_AT = READ_CLEARED_AT + CLEARED_FACES * CUBE_FACE_BYTES,
	READBACK_BYTES = READ_COPIED_AT + CUBE_FACE_BYTES,
	COPIED_FROM_FACE = 1,
	COPIED_TO_FACE = 4,
	// The cached memory type, which transfer buffers alone take (docs/SUPPORT.md, Memory and buffers).
	CACHED_TYPE = 1,
	VERTEX_FLOATS = 8,
	STRIDE_BYTES = VERTEX_FLOATS * 4,
	COORD_OFFSET_BYTES = 16,
	QUAD_VERTICES = 6,
	// Each cube face at level 0, then at level 1, then each slice, a quad 96 pixels on a side each.
	QUADS = FACES * CUBE_LEVELS + SLICES,
	QUAD_SIZE = 96,
	QUAD_STEP = 112,
	LEFT_X = 40,
	TOP_Y = 60,
	ROW_STEP = 120,
	PROBES = QUADS * 4 + 1,
	TOLERANCE = 1,
	SETS = 3,
};

static const VkVertexInputBindingDescription binding = { 0, STRIDE_BYTES, VK_VERTEX_INPUT_RATE_VERTEX };
static const VkVertexInputAttributeDescription attributes[] = {
	{ .location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = 0 },
	{ .location = 1, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = COORD_OFFSET_BYTES },
};

static const VkClearValue clear = { .color = { .float32 = { 24.0f / 255, 24.0f / 255, 24.0f / 255, 1.0f } } };
static const Rgb clear_rgb = { 24, 24, 24 };
static const VkClearColorValue face_clear = { .float32 = { 1.0f, 0.5f, 0.0f, 1.0f } };
static const uint8_t face_clear_bytes[TEXEL_BYTES] = { 255, 128, 0, 255 };

typedef struct {
	VkRenderPass render_pass;
	VkFramebuffer framebuffers[HARNESS_MAX_IMAGES];
	VkDescriptorSetLayout set_layout;
	VkPipelineLayout layout;
	VkPipeline cube_pipeline;
	VkPipeline volume_pipeline;
	VkBuffer vertices;
	VkDeviceMemory vertex_memory;
	VkBuffer staging;
	VkDeviceMemory staging_memory;
	VkBuffer readback;
	VkDeviceMemory readback_memory;
	const uint8_t *read;
	VkImage cube;
	VkDeviceMemory cube_memory;
	VkImageView cube_view;
	VkImage volume;
	VkDeviceMemory volume_memory;
	VkImageView volume_view;
	VkImage other;
	VkDeviceMemory other_memory;
	// Level 0 alone and level 1 alone, by their LOD clamps.
	VkSampler level_samplers[CUBE_LEVELS];
	VkDescriptorPool pool;
	// The cube through each sampler, then the 3D image.
	VkDescriptorSet sets[SETS];
	VkCommandBuffer commands[HARNESS_MAX_IMAGES];
	HarnessProbe probes[PROBES];
	HarnessProbes check;
} Scene;

static uint32_t quadrant(uint32_t x, uint32_t y, uint32_t side)
{
	return (y >= side / 2) * 2 + (x >= side / 2);
}

static Rgb face_rgb(uint32_t face, uint32_t level, uint32_t corner)
{
	return (Rgb){ 40 + 30 * face, level ? 200 : 60, 40 + 50 * corner };
}

static Rgb slice_rgb(uint32_t slice, uint32_t corner)
{
	return (Rgb){ 40 + 25 * slice, 130, 60 + 50 * corner };
}

static void put_texel(uint8_t *bytes, Rgb rgb)
{
	bytes[0] = (uint8_t)rgb.red, bytes[1] = (uint8_t)rgb.green, bytes[2] = (uint8_t)rgb.blue, bytes[3] = 0xFF;
}

// A cube face's texels at a level, as the staging buffer and a read-back hold them.
static void face_texels(uint8_t *bytes, uint32_t face, uint32_t level)
{
	const uint32_t side = CUBE_SIDE >> level;
	for (uint32_t y = 0; y < side; y++)
		for (uint32_t x = 0; x < side; x++)
			put_texel(bytes + (y * side + x) * TEXEL_BYTES, face_rgb(face, level, quadrant(x, y, side)));
}

static void slice_texels(uint8_t *bytes, uint32_t slice)
{
	for (uint32_t y = 0; y < CUBE_SIDE; y++)
		for (uint32_t x = 0; x < CUBE_SIDE; x++)
			put_texel(bytes + (y * CUBE_SIDE + x) * TEXEL_BYTES, slice_rgb(slice, quadrant(x, y, CUBE_SIDE)));
}

static void fill_staging(uint8_t *bytes)
{
	for (uint32_t face = 0; face < FACES; face++) {
		face_texels(bytes + face * CUBE_FACE_BYTES, face, 0);
		face_texels(bytes + CUBE_LEVEL_1_AT + face * SMALL_FACE_BYTES, face, 1);
	}
	for (uint32_t slice = 0; slice < SLICES; slice++)
		slice_texels(bytes + VOLUME_AT + slice * SLICE_BYTES, slice);
}

static uint32_t quad_x(uint32_t quad)
{
	return LEFT_X + (quad < FACES * CUBE_LEVELS ? quad % FACES : quad - FACES * CUBE_LEVELS) * QUAD_STEP;
}

static uint32_t quad_y(uint32_t quad)
{
	return TOP_Y + (quad < FACES ? 0 : quad < FACES * CUBE_LEVELS ? 1 : 2) * ROW_STEP;
}

// A probe at each quadrant's centre of each quad, and the clear colour.
static void make_probes(Scene *scene)
{
	uint32_t count = 0;
	for (uint32_t quad = 0; quad < QUADS; quad++) {
		for (uint32_t corner = 0; corner < 4; corner++) {
			const bool cube = quad < FACES * CUBE_LEVELS;
			const Rgb want =
				cube ? face_rgb(quad % FACES, quad / FACES, corner) : slice_rgb(quad - FACES * CUBE_LEVELS, corner);
			scene->probes[count++] = (HarnessProbe){ cube ? "a cube face" : "a 3D slice",
				quad_x(quad) + (corner & 1 ? 3 : 1) * QUAD_SIZE / 4,
				quad_y(quad) + (corner & 2 ? 3 : 1) * QUAD_SIZE / 4, want, 0 };
		}
	}
	scene->probes[count++] = (HarnessProbe){ "the clear colour", 20, 20, clear_rgb, 0 };
	scene->check = (HarnessProbes){ "cube and 3D", scene->probes, count, TOLERANCE, 0 };
}

// A quad's corner s across and t down, each 0 or 1: a direction toward a face's sc and tc, as Vulkan's cube map face
// selection takes them back (Vulkan 1.0, "Cube map face selection"), or a 3D slice's u, v and w.
static void corner_coord(uint32_t quad, float s, float t, float *out)
{
	const float sc = 2.0f * s - 1.0f;
	const float tc = 2.0f * t - 1.0f;
	const float directions[FACES][4] = {
		{ 1.0f, -tc, -sc, 0.0f },
		{ -1.0f, -tc, sc, 0.0f },
		{ sc, 1.0f, tc, 0.0f },
		{ sc, -1.0f, -tc, 0.0f },
		{ sc, -tc, 1.0f, 0.0f },
		{ -sc, -tc, -1.0f, 0.0f },
	};
	const uint32_t slice = quad - FACES * CUBE_LEVELS;
	const float volume[4] = { s, t, ((float)slice + 0.5f) / SLICES, 0.0f };
	memcpy(out, quad < FACES * CUBE_LEVELS ? directions[quad % FACES] : volume, sizeof volume);
}

static void fill_vertices(float vertices[QUADS * QUAD_VERTICES][VERTEX_FLOATS])
{
	static const float corners[QUAD_VERTICES][2] = { { 0, 0 }, { 1, 0 }, { 0, 1 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
	for (uint32_t quad = 0; quad < QUADS; quad++) {
		for (uint32_t i = 0; i < QUAD_VERTICES; i++) {
			float *const vertex = vertices[quad * QUAD_VERTICES + i];
			const float s = corners[i][0];
			const float t = corners[i][1];
			vertex[0] = ((float)quad_x(quad) + s * QUAD_SIZE) / (HARNESS_WIDTH / 2) - 1.0f;
			vertex[1] = ((float)quad_y(quad) + t * QUAD_SIZE) / (HARNESS_HEIGHT / 2) - 1.0f;
			vertex[2] = 0.0f;
			vertex[3] = 1.0f;
			corner_coord(quad, s, t, &vertex[4]);
		}
	}
}

static VkResult bind_memory(const Harness *harness, VkImage image, VkDeviceMemory *memory)
{
	VkMemoryRequirements requirements;
	vkGetImageMemoryRequirements(harness->device, image, &requirements);
	const VkMemoryAllocateInfo allocate_info = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = requirements.size,
	};
	VkResult result = vkAllocateMemory(harness->device, &allocate_info, NULL, memory);
	if (result == VK_SUCCESS)
		result = vkBindImageMemory(harness->device, image, *memory, 0);
	return result;
}

// A cube of levels mip levels, or a 3D image of the slices; and a view of all of it, unless view is NULL.
static VkResult make_image(const Harness *harness, bool cube, uint32_t levels, VkImageUsageFlags usage, VkImage *image,
	VkDeviceMemory *memory, VkImageView *view)
{
	const VkImageCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0,
		.imageType = cube ? VK_IMAGE_TYPE_2D : VK_IMAGE_TYPE_3D,
		.format = VK_FORMAT_R8G8B8A8_UNORM,
		.extent = { CUBE_SIDE, CUBE_SIDE, cube ? 1 : SLICES },
		.mipLevels = levels,
		.arrayLayers = cube ? FACES : 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = usage,
	};
	VkResult result = vkCreateImage(harness->device, &info, NULL, image);
	if (result == VK_SUCCESS)
		result = bind_memory(harness, *image, memory);
	const VkImageViewCreateInfo view_info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.image = *image,
		.viewType = cube ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_3D,
		.format = VK_FORMAT_R8G8B8A8_UNORM,
		.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS },
	};
	if (result == VK_SUCCESS && view)
		result = vkCreateImageView(harness->device, &view_info, NULL, view);
	return result;
}

// A buffer only copied into, in cached memory, mapped.
static VkResult make_readback(const Harness *harness, Scene *scene)
{
	const VkBufferCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = READBACK_BYTES, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT
	};
	VkResult result = vkCreateBuffer(harness->device, &info, NULL, &scene->readback);
	if (result != VK_SUCCESS)
		return result;
	VkMemoryRequirements requirements;
	vkGetBufferMemoryRequirements(harness->device, scene->readback, &requirements);
	const VkMemoryAllocateInfo allocate = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = requirements.size,
		.memoryTypeIndex = CACHED_TYPE,
	};
	void *mapped = NULL;
	result = vkAllocateMemory(harness->device, &allocate, NULL, &scene->readback_memory);
	if (result == VK_SUCCESS)
		result = vkBindBufferMemory(harness->device, scene->readback, scene->readback_memory, 0);
	if (result == VK_SUCCESS)
		result = vkMapMemory(harness->device, scene->readback_memory, 0, VK_WHOLE_SIZE, 0, &mapped);
	scene->read = mapped;
	return result;
}

static VkResult make_samplers(const Harness *harness, Scene *scene)
{
	VkResult result = VK_SUCCESS;
	for (uint32_t level = 0; level < CUBE_LEVELS && result == VK_SUCCESS; level++) {
		const VkSamplerCreateInfo info = {
			.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
			.magFilter = VK_FILTER_NEAREST,
			.minFilter = VK_FILTER_NEAREST,
			.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
			.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
			.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
			.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
			.minLod = (float)level,
			.maxLod = (float)level,
		};
		result = vkCreateSampler(harness->device, &info, NULL, &scene->level_samplers[level]);
	}
	return result;
}

static VkResult make_descriptors(const Harness *harness, Scene *scene)
{
	const VkDescriptorPoolSize size = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, SETS };
	const VkDescriptorPoolCreateInfo pool_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.maxSets = SETS,
		.poolSizeCount = 1,
		.pPoolSizes = &size,
	};
	VkResult result = vkCreateDescriptorPool(harness->device, &pool_info, NULL, &scene->pool);
	const VkDescriptorSetLayout layouts[SETS] = { scene->set_layout, scene->set_layout, scene->set_layout };
	const VkDescriptorSetAllocateInfo allocate_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool = scene->pool,
		.descriptorSetCount = SETS,
		.pSetLayouts = layouts,
	};
	if (result == VK_SUCCESS)
		result = vkAllocateDescriptorSets(harness->device, &allocate_info, scene->sets);
	if (result != VK_SUCCESS)
		return result;
	const VkDescriptorImageInfo images[SETS] = {
		{ scene->level_samplers[0], scene->cube_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
		{ scene->level_samplers[1], scene->cube_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
		{ scene->level_samplers[0], scene->volume_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
	};
	for (uint32_t i = 0; i < SETS; i++) {
		const VkWriteDescriptorSet write = {
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstSet = scene->sets[i],
			.descriptorCount = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			.pImageInfo = &images[i],
		};
		vkUpdateDescriptorSets(harness->device, 1, &write, 0, NULL);
	}
	return VK_SUCCESS;
}

static VkResult make_pipelines(const Harness *harness, Scene *scene)
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
	HarnessPipeline spec = {
		.vertex_spirv = coord_vert_spirv,
		.vertex_bytes = COORD_VERT_SPIRV_BYTES,
		.fragment_spirv = cube_frag_spirv,
		.fragment_bytes = CUBE_FRAG_SPIRV_BYTES,
		.bindings = &binding,
		.binding_count = 1,
		.attributes = attributes,
		.attribute_count = 2,
		.layout = scene->layout,
	};
	if (result == VK_SUCCESS)
		result = harness_render_pass(harness, &scene->render_pass, scene->framebuffers);
	if (result == VK_SUCCESS)
		result = harness_pipeline(harness, &spec, scene->render_pass, &scene->cube_pipeline);
	spec.fragment_spirv = volume_frag_spirv;
	spec.fragment_bytes = VOLUME_FRAG_SPIRV_BYTES;
	if (result == VK_SUCCESS)
		result = harness_pipeline(harness, &spec, scene->render_pass, &scene->volume_pipeline);
	return result;
}

static bool build(const Harness *harness, Scene *scene)
{
	static uint8_t staging[STAGING_BYTES];
	static float vertices[QUADS * QUAD_VERTICES][VERTEX_FLOATS];
	fill_staging(staging);
	fill_vertices(vertices);
	const VkImageUsageFlags usage =
		VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	const VkImageUsageFlags copy_usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	return harness_ok("pipelines", make_pipelines(harness, scene)) &&
		harness_ok("cube",
			make_image(harness, true, CUBE_LEVELS, usage, &scene->cube, &scene->cube_memory, &scene->cube_view)) &&
		harness_ok("3D image",
			make_image(harness, false, 1, usage, &scene->volume, &scene->volume_memory, &scene->volume_view)) &&
		harness_ok("other cube", make_image(harness, true, 1, copy_usage, &scene->other, &scene->other_memory, NULL)) &&
		harness_ok("samplers", make_samplers(harness, scene)) &&
		harness_ok("descriptors", make_descriptors(harness, scene)) &&
		harness_ok("vertex buffer",
			harness_buffer(harness, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, vertices, sizeof vertices, &scene->vertices,
				&scene->vertex_memory)) &&
		harness_ok("staging buffer",
			harness_buffer(harness, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, staging, STAGING_BYTES, &scene->staging,
				&scene->staging_memory)) &&
		harness_ok("read-back buffer", make_readback(harness, scene));
}

static VkBufferImageCopy region(
	uint32_t at, uint32_t level, uint32_t first_layer, uint32_t layers, int32_t z, uint32_t depth)
{
	const uint32_t side = CUBE_SIDE >> level;
	return (VkBufferImageCopy){
		.bufferOffset = at,
		.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, level, first_layer, layers },
		.imageOffset = { 0, 0, z },
		.imageExtent = { side, side, depth },
	};
}

static bool submit_once(const Harness *harness, void (*record)(const Scene *, VkCommandBuffer), const Scene *scene)
{
	const VkCommandBufferAllocateInfo allocate_info = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = harness->pool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1,
	};
	VkCommandBuffer commands = VK_NULL_HANDLE;
	if (!harness_ok("command buffer", vkAllocateCommandBuffers(harness->device, &allocate_info, &commands)))
		return false;
	const VkCommandBufferBeginInfo begin = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	(void)vkBeginCommandBuffer(commands, &begin);
	record(scene, commands);
	bool ok = harness_ok("recording", vkEndCommandBuffer(commands));
	const VkSubmitInfo submit = {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &commands
	};
	ok = ok && harness_ok("submit", vkQueueSubmit(harness->queue, 1, &submit, harness->fence));
	ok = ok && harness_ok("wait", vkWaitForFences(harness->device, 1, &harness->fence, VK_TRUE, FENCE_TIMEOUT_NS));
	(void)vkResetFences(harness->device, 1, &harness->fence);
	vkFreeCommandBuffers(harness->device, harness->pool, 1, &commands);
	return ok;
}

// The cube's six faces at each level in one region each, and the slices in two halves, the second at z 4.
static void record_upload(const Scene *scene, VkCommandBuffer commands)
{
	const VkBufferImageCopy cube_regions[CUBE_LEVELS] = { region(0, 0, 0, FACES, 0, 1),
		region(CUBE_LEVEL_1_AT, 1, 0, FACES, 0, 1) };
	const VkBufferImageCopy volume_regions[2] = { region(VOLUME_AT, 0, 0, 1, 0, HALF_SLICES),
		region(VOLUME_AT + HALF_SLICES * SLICE_BYTES, 0, 0, 1, HALF_SLICES, HALF_SLICES) };
	vkCmdCopyBufferToImage(
		commands, scene->staging, scene->cube, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, CUBE_LEVELS, cube_regions);
	vkCmdCopyBufferToImage(
		commands, scene->staging, scene->volume, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 2, volume_regions);
}

// Level 1's face 3 and slices 5 and 6 out; face 1 into the other cube's face 4, its faces 0 and 1 cleared, and out.
static void record_readback(const Scene *scene, VkCommandBuffer commands)
{
	const VkBufferImageCopy out[] = { region(0, 1, READ_FACE, 1, 0, 1),
		region(READ_SLICES_AT, 0, 0, 1, READ_SLICE, READ_SLICES) };
	vkCmdCopyImageToBuffer(commands, scene->cube, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, scene->readback, 1, &out[0]);
	vkCmdCopyImageToBuffer(commands, scene->volume, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, scene->readback, 1, &out[1]);
	const VkImageCopy copy = {
		.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, COPIED_FROM_FACE, 1 },
		.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, COPIED_TO_FACE, 1 },
		.extent = { CUBE_SIDE, CUBE_SIDE, 1 },
	};
	vkCmdCopyImage(commands, scene->cube, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, scene->other,
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
	const VkImageSubresourceRange cleared = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, CLEARED_FACES };
	vkCmdClearColorImage(commands, scene->other, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &face_clear, 1, &cleared);
	const VkBufferImageCopy other_out[] = { region(READ_CLEARED_AT, 0, 0, CLEARED_FACES, 0, 1),
		region(READ_COPIED_AT, 0, COPIED_TO_FACE, 1, 0, 1) };
	vkCmdCopyImageToBuffer(commands, scene->other, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, scene->readback, 2, other_out);
}

static bool read_back_out(const Scene *scene)
{
	static uint8_t want[READ_CLEARED_AT];
	face_texels(want, READ_FACE, 1);
	for (uint32_t i = 0; i < READ_SLICES; i++)
		slice_texels(want + READ_SLICES_AT + i * SLICE_BYTES, READ_SLICE + i);
	const bool same = memcmp(scene->read, want, sizeof want) == 0;
	if (!same)
		xsdk_log_write("cube and 3D: read back %02X %02X %02X at the face, %02X %02X %02X at the slices",
			scene->read[0], scene->read[1], scene->read[2], scene->read[READ_SLICES_AT],
			scene->read[READ_SLICES_AT + 1], scene->read[READ_SLICES_AT + 2]);
	return same;
}

static bool read_back_other(const Scene *scene)
{
	static uint8_t want[READBACK_BYTES - READ_CLEARED_AT];
	for (uint32_t i = 0; i < CLEARED_FACES * CUBE_SIDE * CUBE_SIDE; i++)
		memcpy(want + i * TEXEL_BYTES, face_clear_bytes, TEXEL_BYTES);
	face_texels(want + READ_COPIED_AT - READ_CLEARED_AT, COPIED_FROM_FACE, 0);
	const bool same = memcmp(scene->read + READ_CLEARED_AT, want, sizeof want) == 0;
	if (!same)
		xsdk_log_write("cube and 3D: the other cube read back %02X %02X %02X cleared, %02X %02X %02X copied",
			scene->read[READ_CLEARED_AT], scene->read[READ_CLEARED_AT + 1], scene->read[READ_CLEARED_AT + 2],
			scene->read[READ_COPIED_AT], scene->read[READ_COPIED_AT + 1], scene->read[READ_COPIED_AT + 2]);
	return same;
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
		vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, scene->cube_pipeline);
		for (uint32_t level = 0; level < CUBE_LEVELS; level++) {
			vkCmdBindDescriptorSets(
				commands, VK_PIPELINE_BIND_POINT_GRAPHICS, scene->layout, 0, 1, &scene->sets[level], 0, NULL);
			vkCmdDraw(commands, FACES * QUAD_VERTICES, 1, level * FACES * QUAD_VERTICES, 0);
		}
		vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, scene->volume_pipeline);
		vkCmdBindDescriptorSets(
			commands, VK_PIPELINE_BIND_POINT_GRAPHICS, scene->layout, 0, 1, &scene->sets[2], 0, NULL);
		vkCmdDraw(commands, SLICES * QUAD_VERTICES, 1, FACES * CUBE_LEVELS * QUAD_VERTICES, 0);
		vkCmdEndRenderPass(commands);
		result = vkEndCommandBuffer(commands);
	}
	return result;
}

// What the device reports: cubes of six layers with their mips, 3D images of one level, and the limits for both.
static bool reports_cube_and_3d(const Harness *harness)
{
	VkPhysicalDeviceProperties properties;
	vkGetPhysicalDeviceProperties(harness->physical_device, &properties);
	const VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	VkImageFormatProperties cube;
	VkImageFormatProperties volume;
	const bool found =
		vkGetPhysicalDeviceImageFormatProperties(harness->physical_device, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_2D,
			VK_IMAGE_TILING_OPTIMAL, usage, VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT, &cube) == VK_SUCCESS &&
		vkGetPhysicalDeviceImageFormatProperties(harness->physical_device, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_3D,
			VK_IMAGE_TILING_OPTIMAL, usage, 0, &volume) == VK_SUCCESS;
	return found && cube.maxArrayLayers == FACES && cube.maxMipLevels > 1 && volume.maxMipLevels == 1 &&
		volume.maxExtent.depth == properties.limits.maxImageDimension3D &&
		properties.limits.maxImageArrayLayers == FACES &&
		properties.limits.maxImageDimensionCube >= cube.maxExtent.width;
}

// An array of 2D images, an array of cubes, a cube drawn into and a 2D view of one face.
static bool refuses_arrays(const Harness *harness, const Scene *scene)
{
	VkImageCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = VK_FORMAT_R8G8B8A8_UNORM,
		.extent = { CUBE_SIDE, CUBE_SIDE, 1 },
		.mipLevels = 1,
		.arrayLayers = 2,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = VK_IMAGE_USAGE_SAMPLED_BIT,
	};
	VkImage image = VK_NULL_HANDLE;
	const bool array = vkCreateImage(harness->device, &info, NULL, &image) == VK_ERROR_FEATURE_NOT_PRESENT;
	info.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
	info.arrayLayers = 2 * FACES;
	const bool cubes = vkCreateImage(harness->device, &info, NULL, &image) == VK_ERROR_FEATURE_NOT_PRESENT;
	info.arrayLayers = FACES;
	info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	const bool drawn = vkCreateImage(harness->device, &info, NULL, &image) == VK_ERROR_FEATURE_NOT_PRESENT;
	const VkImageViewCreateInfo view_info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.image = scene->cube,
		.viewType = VK_IMAGE_VIEW_TYPE_2D,
		.format = VK_FORMAT_R8G8B8A8_UNORM,
		.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 2, 1 },
	};
	VkImageView view = VK_NULL_HANDLE;
	const bool face = vkCreateImageView(harness->device, &view_info, NULL, &view) == VK_ERROR_FEATURE_NOT_PRESENT;
	return array && cubes && drawn && face;
}

static void destroy(const Harness *harness, Scene *scene)
{
	const VkDevice device = harness->device;
	vkFreeCommandBuffers(device, harness->pool, harness->image_count, scene->commands);
	vkDestroyDescriptorPool(device, scene->pool, NULL);
	for (uint32_t i = 0; i < CUBE_LEVELS; i++)
		vkDestroySampler(device, scene->level_samplers[i], NULL);
	vkDestroyImageView(device, scene->cube_view, NULL);
	vkDestroyImage(device, scene->cube, NULL);
	vkFreeMemory(device, scene->cube_memory, NULL);
	vkDestroyImageView(device, scene->volume_view, NULL);
	vkDestroyImage(device, scene->volume, NULL);
	vkFreeMemory(device, scene->volume_memory, NULL);
	vkDestroyImage(device, scene->other, NULL);
	vkFreeMemory(device, scene->other_memory, NULL);
	vkDestroyBuffer(device, scene->readback, NULL);
	vkFreeMemory(device, scene->readback_memory, NULL);
	vkDestroyBuffer(device, scene->staging, NULL);
	vkFreeMemory(device, scene->staging_memory, NULL);
	vkDestroyBuffer(device, scene->vertices, NULL);
	vkFreeMemory(device, scene->vertex_memory, NULL);
	vkDestroyPipeline(device, scene->cube_pipeline, NULL);
	vkDestroyPipeline(device, scene->volume_pipeline, NULL);
	vkDestroyPipelineLayout(device, scene->layout, NULL);
	vkDestroyDescriptorSetLayout(device, scene->set_layout, NULL);
	harness_destroy_render_pass(harness, scene->render_pass, scene->framebuffers);
}

void test_cube_3d(const Harness *harness)
{
	Scene scene = { .render_pass = VK_NULL_HANDLE };
	make_probes(&scene);
	const bool built = build(harness, &scene);
	check("cube and 3D: a cube of two levels, a 3D image and another cube made, with cube and 3D views", built);
	check("cube and 3D: the device reports cubes of six layers with mips, and 3D images of one level",
		reports_cube_and_3d(harness));
	check("cube and 3D: arrays of 2D images and of cubes, cubes drawn into and a 2D view of a face are refused",
		built && refuses_arrays(harness, &scene));
	const bool uploaded = built && submit_once(harness, record_upload, &scene);
	check("cube and 3D: a buffer copied into the cube's six faces at each level, and the 3D image's slices in halves",
		uploaded);
	const bool recorded = uploaded && harness->swapchain && harness_ok("recording", record(harness, &scene));
	uint32_t misses = 0;
	const uint32_t frames =
		recorded ? harness_frames(harness, scene.commands, FRAMES, harness_probes_hold, &scene.check, &misses) : 0;
	check("cube and 3D: 30 frames drawn and presented", frames == FRAMES);
	check("cube and 3D: every frame shows each cube face at levels 0 and 1, and each 3D slice, quadrant by quadrant",
		frames == FRAMES && misses == 0);
	check(
		"cube and 3D: the image on screen shows them too", harness_screen(harness, harness_probes_hold, &scene.check));
	const bool read = uploaded && submit_once(harness, record_readback, &scene);
	check("cube and 3D: level 1's face 3 and slices 5 and 6 copied back out", read && read_back_out(&scene));
	check("cube and 3D: a face copied into another cube's face 4, and its faces 0 and 1 cleared, read back",
		read && read_back_other(&scene));
	destroy(harness, &scene);
}
