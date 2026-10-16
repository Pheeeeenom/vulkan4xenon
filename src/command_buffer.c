#include <math.h>
#include <string.h>

#include <vulkan/vulkan.h>

#include "format.h"
#include "host.h"
#include "image.h"
#include "objects.h"

// Commands go into a list that grows as a command buffer records; vkQueueSubmit replays them (queue.c).

VKAPI_ATTR VkResult VKAPI_CALL vkCreateCommandPool(VkDevice device, const VkCommandPoolCreateInfo *pCreateInfo,
	const VkAllocationCallbacks *pAllocator, VkCommandPool *pCommandPool)
{
	if (pCreateInfo->queueFamilyIndex != 0)
		return VK_ERROR_INITIALIZATION_FAILED;
	CommandPool *const pool =
		v4x_host_alloc(v4x_host_device_allocator(device, pAllocator), sizeof *pool, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
	if (!pool)
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	*pCommandPool = v4x_handle(pool);
	return VK_SUCCESS;
}

static void free_buffer(struct VkCommandBuffer_T *buffer)
{
	const VkAllocationCallbacks *const allocator = v4x_host_device_allocator(buffer->device, NULL);
	v4x_host_free(allocator, buffer->data);
	v4x_host_free(allocator, buffer->commands);
	v4x_host_free(allocator, buffer);
}

VKAPI_ATTR void VKAPI_CALL vkDestroyCommandPool(
	VkDevice device, VkCommandPool commandPool, const VkAllocationCallbacks *pAllocator)
{
	CommandPool *const pool = v4x_object(commandPool);
	if (!pool)
		return;
	while (pool->buffers) {
		struct VkCommandBuffer_T *const next = pool->buffers->next;
		free_buffer(pool->buffers);
		pool->buffers = next;
	}
	v4x_host_free(v4x_host_device_allocator(device, pAllocator), pool);
}

static void reset_buffer(struct VkCommandBuffer_T *buffer)
{
	buffer->state = COMMAND_BUFFER_INITIAL;
	buffer->result = VK_SUCCESS;
	buffer->count = 0;
	buffer->data_bytes = 0;
	buffer->render_pass = NULL;
	buffer->framebuffer = NULL;
	buffer->subpass = 0;
	buffer->pipeline = NULL;
}

VKAPI_ATTR VkResult VKAPI_CALL vkResetCommandPool(
	VkDevice device, VkCommandPool commandPool, VkCommandPoolResetFlags flags)
{
	(void)device;
	(void)flags;
	const CommandPool *const pool = v4x_object(commandPool);
	for (struct VkCommandBuffer_T *buffer = pool->buffers; buffer; buffer = buffer->next)
		reset_buffer(buffer);
	return VK_SUCCESS;
}

static struct VkCommandBuffer_T *allocate_buffer(VkDevice device, CommandPool *pool)
{
	const VkAllocationCallbacks *const allocator = v4x_host_device_allocator(device, NULL);
	struct VkCommandBuffer_T *const buffer =
		v4x_host_alloc(allocator, sizeof *buffer, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
	if (!buffer)
		return NULL;
	buffer->commands =
		v4x_host_alloc(allocator, V4X_FIRST_COMMANDS * sizeof(Command), VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
	buffer->data = v4x_host_alloc(allocator, V4X_FIRST_COMMAND_DATA_BYTES, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
	if (!buffer->commands || !buffer->data) {
		v4x_host_free(allocator, buffer->data);
		v4x_host_free(allocator, buffer->commands);
		v4x_host_free(allocator, buffer);
		return NULL;
	}
	buffer->capacity = V4X_FIRST_COMMANDS;
	buffer->data_capacity_bytes = V4X_FIRST_COMMAND_DATA_BYTES;
	buffer->device = device;
	buffer->pool = pool;
	buffer->next = pool->buffers;
	pool->buffers = buffer;
	return buffer;
}

static void unlink_buffer(struct VkCommandBuffer_T *buffer)
{
	struct VkCommandBuffer_T **link = &buffer->pool->buffers;
	while (*link != buffer)
		link = &(*link)->next;
	*link = buffer->next;
}

VKAPI_ATTR void VKAPI_CALL vkFreeCommandBuffers(
	VkDevice device, VkCommandPool commandPool, uint32_t commandBufferCount, const VkCommandBuffer *pCommandBuffers)
{
	(void)device;
	(void)commandPool;
	for (uint32_t i = 0; i < commandBufferCount; i++) {
		if (!pCommandBuffers[i])
			continue;
		unlink_buffer(pCommandBuffers[i]);
		free_buffer(pCommandBuffers[i]);
	}
}

VKAPI_ATTR VkResult VKAPI_CALL vkAllocateCommandBuffers(
	VkDevice device, const VkCommandBufferAllocateInfo *pAllocateInfo, VkCommandBuffer *pCommandBuffers)
{
	CommandPool *const pool = v4x_object(pAllocateInfo->commandPool);
	for (uint32_t i = 0; i < pAllocateInfo->commandBufferCount; i++) {
		pCommandBuffers[i] = allocate_buffer(device, pool);
		if (!pCommandBuffers[i]) {
			vkFreeCommandBuffers(device, pAllocateInfo->commandPool, i, pCommandBuffers);
			for (uint32_t j = 0; j < pAllocateInfo->commandBufferCount; j++)
				pCommandBuffers[j] = VK_NULL_HANDLE;
			return VK_ERROR_OUT_OF_HOST_MEMORY;
		}
		pCommandBuffers[i]->secondary = pAllocateInfo->level == VK_COMMAND_BUFFER_LEVEL_SECONDARY;
	}
	return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkBeginCommandBuffer(
	VkCommandBuffer commandBuffer, const VkCommandBufferBeginInfo *pBeginInfo)
{
	(void)pBeginInfo;
	reset_buffer(commandBuffer);
	commandBuffer->state = COMMAND_BUFFER_RECORDING;
	return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkEndCommandBuffer(VkCommandBuffer commandBuffer)
{
	commandBuffer->state = COMMAND_BUFFER_EXECUTABLE;
	return commandBuffer->result;
}

VKAPI_ATTR VkResult VKAPI_CALL vkResetCommandBuffer(VkCommandBuffer commandBuffer, VkCommandBufferResetFlags flags)
{
	(void)flags;
	reset_buffer(commandBuffer);
	return VK_SUCCESS;
}

// Doubles the commands' room, or saves the error for vkEndCommandBuffer when there's no memory for it.
static bool grow_commands(VkCommandBuffer buffer)
{
	const VkAllocationCallbacks *const allocator = v4x_host_device_allocator(buffer->device, NULL);
	Command *const grown = v4x_host_realloc(
		allocator, buffer->commands, 2 * buffer->capacity * sizeof(Command), VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
	if (!grown) {
		buffer->result = VK_ERROR_OUT_OF_HOST_MEMORY;
		return false;
	}
	buffer->commands = grown;
	buffer->capacity *= 2;
	return true;
}

// The same for the data.
static bool grow_data(VkCommandBuffer buffer)
{
	const VkAllocationCallbacks *const allocator = v4x_host_device_allocator(buffer->device, NULL);
	uint8_t *const grown =
		v4x_host_realloc(allocator, buffer->data, 2 * buffer->data_capacity_bytes, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
	if (!grown) {
		buffer->result = VK_ERROR_OUT_OF_HOST_MEMORY;
		return false;
	}
	buffer->data = grown;
	buffer->data_capacity_bytes *= 2;
	return true;
}

// The next free command, or NULL with the error saved for vkEndCommandBuffer.
static Command *record(VkCommandBuffer buffer, CommandType type)
{
	if (buffer->count == buffer->capacity && !grow_commands(buffer))
		return NULL;
	Command *const command = &buffer->commands[buffer->count++];
	command->type = type;
	return command;
}

// Copies bytes into the command buffer's data and returns where, or saves the error.
static bool record_data(VkCommandBuffer buffer, const void *bytes, uint32_t size_bytes, uint32_t *at)
{
	const uint32_t aligned = (size_bytes + 3) & ~3u;
	while (buffer->data_bytes + aligned > buffer->data_capacity_bytes) {
		if (!grow_data(buffer))
			return false;
	}
	*at = buffer->data_bytes;
	memcpy(buffer->data + buffer->data_bytes, bytes, size_bytes);
	buffer->data_bytes += aligned;
	return true;
}

static uint32_t unorm8(float value)
{
	const float clamped = value < 0.0f ? 0.0f : value > 1.0f ? 1.0f : value;
	return (uint32_t)lrintf(clamped * 255.0f);
}

// Each subpass begins as a render pass of its own, its clears from the values the render pass began with.
static void record_subpass(VkCommandBuffer commandBuffer)
{
	const RenderPass *const render_pass = &commandBuffer->render_pass->subpasses[commandBuffer->subpass];
	Command *const command = record(commandBuffer, COMMAND_BEGIN_RENDER_PASS);
	if (!command)
		return;
	uint32_t argb = 0;
	if (render_pass->load_op == VK_ATTACHMENT_LOAD_OP_CLEAR) {
		const float *const rgba = commandBuffer->clears[render_pass->color_index].color.float32;
		argb = unorm8(rgba[3]) << 24 | unorm8(rgba[0]) << 16 | unorm8(rgba[1]) << 8 | unorm8(rgba[2]);
	}
	command->begin.render_pass = render_pass;
	command->begin.framebuffer = &commandBuffer->framebuffer->subpasses[commandBuffer->subpass];
	command->begin.clear_argb = argb;
	command->begin.clear_depth = 1.0f;
	command->begin.clear_stencil = 0;
	if (render_pass->clears_depth) {
		const VkClearDepthStencilValue *const value = &commandBuffer->clears[render_pass->depth_index].depthStencil;
		command->begin.clear_depth = value->depth;
		command->begin.clear_stencil = value->stencil;
	}
}

// The subpass's draws are recorded here or in secondary command buffers, which vkCmdExecuteCommands replays in place.
VKAPI_ATTR void VKAPI_CALL vkCmdBeginRenderPass(
	VkCommandBuffer commandBuffer, const VkRenderPassBeginInfo *pRenderPassBegin, VkSubpassContents contents)
{
	(void)contents;
	const FramebufferSubpasses *const framebuffer = v4x_object(pRenderPassBegin->framebuffer);
	const VkRect2D *const area = &pRenderPassBegin->renderArea;
	if (area->offset.x || area->offset.y || area->extent.width != framebuffer->width ||
		area->extent.height != framebuffer->height) {
		commandBuffer->result = v4x_host_unsupported("render areas smaller than the framebuffer");
		return;
	}
	const uint32_t clears = pRenderPassBegin->clearValueCount < V4X_MAX_ATTACHMENTS ? pRenderPassBegin->clearValueCount
																					: V4X_MAX_ATTACHMENTS;
	if (clears)
		memcpy(commandBuffer->clears, pRenderPassBegin->pClearValues, clears * sizeof(VkClearValue));
	commandBuffer->render_pass = v4x_object(pRenderPassBegin->renderPass);
	commandBuffer->framebuffer = framebuffer;
	commandBuffer->subpass = 0;
	record_subpass(commandBuffer);
}

VKAPI_ATTR void VKAPI_CALL vkCmdNextSubpass(VkCommandBuffer commandBuffer, VkSubpassContents contents)
{
	(void)contents;
	(void)record(commandBuffer, COMMAND_END_RENDER_PASS);
	commandBuffer->subpass++;
	record_subpass(commandBuffer);
}

VKAPI_ATTR void VKAPI_CALL vkCmdEndRenderPass(VkCommandBuffer commandBuffer)
{
	(void)record(commandBuffer, COMMAND_END_RENDER_PASS);
	commandBuffer->render_pass = NULL;
}

// Replayed in place, at vkQueueSubmit, as recorded by then.
VKAPI_ATTR void VKAPI_CALL vkCmdExecuteCommands(
	VkCommandBuffer commandBuffer, uint32_t commandBufferCount, const VkCommandBuffer *pCommandBuffers)
{
	for (uint32_t i = 0; i < commandBufferCount; i++) {
		Command *const command = record(commandBuffer, COMMAND_EXECUTE_COMMANDS);
		if (!command)
			return;
		command->secondary = pCommandBuffers[i];
	}
}

VKAPI_ATTR void VKAPI_CALL vkCmdBindPipeline(
	VkCommandBuffer commandBuffer, VkPipelineBindPoint pipelineBindPoint, VkPipeline pipeline)
{
	(void)pipelineBindPoint;
	Command *const command = record(commandBuffer, COMMAND_BIND_PIPELINE);
	if (!command)
		return;
	command->pipeline = v4x_object(pipeline);
	commandBuffer->pipeline = command->pipeline;
}

// A draw past the 65535 vertices a draw packet carries goes in pieces, but a fan can't be split (pipeline_state.c).
static bool splittable(VkCommandBuffer buffer, uint32_t count)
{
	if (count <= V4X_MAX_DRAW_VERTICES || !buffer->pipeline || buffer->pipeline->piece_step)
		return true;
	buffer->result = v4x_host_unsupported("triangle fans of more than 65535 vertices");
	return false;
}

VKAPI_ATTR void VKAPI_CALL vkCmdBindVertexBuffers(VkCommandBuffer commandBuffer, uint32_t firstBinding,
	uint32_t bindingCount, const VkBuffer *pBuffers, const VkDeviceSize *pOffsets)
{
	for (uint32_t i = 0; i < bindingCount; i++) {
		Command *const command = record(commandBuffer, COMMAND_BIND_VERTEX_BUFFER);
		if (!command)
			return;
		const Buffer *const buffer = v4x_object(pBuffers[i]);
		command->vertex_buffer.binding = firstBinding + i;
		command->vertex_buffer.address = buffer->memory->address + buffer->offset_bytes + (uint32_t)pOffsets[i];
		command->vertex_buffer.size_bytes = (uint32_t)(buffer->size - pOffsets[i]);
	}
}

// Each instance is replayed as a draw of its own (queue.c).
VKAPI_ATTR void VKAPI_CALL vkCmdDraw(VkCommandBuffer commandBuffer, uint32_t vertexCount, uint32_t instanceCount,
	uint32_t firstVertex, uint32_t firstInstance)
{
	if (!vertexCount || !instanceCount || !splittable(commandBuffer, vertexCount))
		return;
	Command *const command = record(commandBuffer, COMMAND_DRAW);
	if (!command)
		return;
	command->draw.vertex_count = vertexCount;
	command->draw.first_vertex = firstVertex;
	command->draw.instance_count = instanceCount;
	command->draw.first_instance = firstInstance;
}

// Each set's dynamic offsets follow its layout's dynamic bindings, in binding order.
VKAPI_ATTR void VKAPI_CALL vkCmdBindDescriptorSets(VkCommandBuffer commandBuffer, VkPipelineBindPoint pipelineBindPoint,
	VkPipelineLayout layout, uint32_t firstSet, uint32_t descriptorSetCount, const VkDescriptorSet *pDescriptorSets,
	uint32_t dynamicOffsetCount, const uint32_t *pDynamicOffsets)
{
	(void)pipelineBindPoint;
	(void)layout;
	(void)dynamicOffsetCount;
	uint32_t used = 0;
	for (uint32_t i = 0; i < descriptorSetCount; i++) {
		const DescriptorSet *const set = v4x_object(pDescriptorSets[i]);
		const uint32_t count = set->layout->dynamic_count;
		uint32_t at = 0;
		if (count && !record_data(commandBuffer, pDynamicOffsets + used, count * 4, &at))
			return;
		used += count;
		Command *const command = record(commandBuffer, COMMAND_BIND_DESCRIPTOR_SET);
		if (!command)
			return;
		command->descriptor_set.index = firstSet + i;
		command->descriptor_set.set = set;
		command->descriptor_set.offsets_at = at;
	}
}

VKAPI_ATTR void VKAPI_CALL vkCmdPushConstants(VkCommandBuffer commandBuffer, VkPipelineLayout layout,
	VkShaderStageFlags stageFlags, uint32_t offset, uint32_t size, const void *pValues)
{
	(void)layout;
	(void)stageFlags;
	uint32_t at = 0;
	if (!record_data(commandBuffer, pValues, size, &at))
		return;
	Command *const command = record(commandBuffer, COMMAND_PUSH_CONSTANTS);
	if (!command)
		return;
	command->push_constants.offset_bytes = offset;
	command->push_constants.size_bytes = size;
	command->push_constants.data_at = at;
}

static uint32_t level_size(uint32_t size, uint32_t level)
{
	return size >> level ? size >> level : 1;
}

// A buffer's bytes at an offset, as the CPU maps them.
static volatile uint8_t *buffer_bytes(const Buffer *buffer, VkDeviceSize offset)
{
	return (volatile uint8_t *)buffer->memory->pointer + buffer->offset_bytes + (uint32_t)offset;
}

// The same as the GPU addresses them; 0 in cached memory, which the GPU never reads.
static uint32_t buffer_address(const Buffer *buffer, VkDeviceSize offset)
{
	return buffer->memory->cached ? 0 : buffer->memory->address + buffer->offset_bytes + (uint32_t)offset;
}

// The layers of a cube, or the slices of a 3D image, a copy region covers.
typedef struct {
	uint32_t first;
	uint32_t count;
} Slices;

// Whether a region is the whole of a level of a colour image across and down, from its corner, in some of its layers
// or 3D slices, and which: a cube's faces are its layers, a 3D image's slices its depth.
static bool whole_level(
	const Image *image, const VkImageSubresourceLayers *layers, VkOffset3D offset, VkExtent3D extent, Slices *slices)
{
	const uint32_t level = layers->mipLevel;
	const bool is_3d = image->texture.dimension == XSDK_TEXTURE_3D;
	*slices =
		is_3d ? (Slices){ (uint32_t)offset.z, extent.depth } : (Slices){ layers->baseArrayLayer, layers->layerCount };
	const bool one_layer = is_3d ? !layers->baseArrayLayer && layers->layerCount == 1 : !offset.z && extent.depth == 1;
	return image->is_texture && level < image->texture.levels && !offset.x && !offset.y &&
		extent.width == level_size(image->width, level) && extent.height == level_size(image->height, level) &&
		one_layer && slices->count && slices->first < image->texture.depth &&
		slices->count <= image->texture.depth - slices->first;
}

// Whole levels between a buffer and an image, in tightly packed rows or rows bufferRowLength texels long, each layer's
// or slice's rows bufferImageHeight rows after the last's.
static void record_level_copies(VkCommandBuffer commandBuffer, CommandType type, const Buffer *buffer, Image *image,
	uint32_t regionCount, const VkBufferImageCopy *pRegions)
{
	for (uint32_t i = 0; i < regionCount; i++) {
		const VkBufferImageCopy *const region = &pRegions[i];
		const uint32_t level = region->imageSubresource.mipLevel;
		Slices slices;
		if (!whole_level(image, &region->imageSubresource, region->imageOffset, region->imageExtent, &slices) ||
			(region->bufferOffset + buffer->offset_bytes) % 4) {
			commandBuffer->result = v4x_host_unsupported("copies of less than a whole level, or at unaligned offsets");
			return;
		}
		const uint32_t row_texels = region->bufferRowLength ? region->bufferRowLength : level_size(image->width, level);
		const uint32_t rows = region->bufferImageHeight ? region->bufferImageHeight : level_size(image->height, level);
		const uint32_t row_bytes = v4x_texel_blocks(image->format, row_texels) * v4x_texel_bytes(image->format);
		const LevelCopy copy = { .buffer = buffer_bytes(buffer, region->bufferOffset),
			.address = buffer_address(buffer, region->bufferOffset),
			.image = image,
			.level = level,
			.row_bytes = row_bytes,
			.first_slice = slices.first,
			.slice_count = slices.count,
			.slice_bytes = v4x_texel_blocks(image->format, rows) * row_bytes };
		uint32_t at = 0;
		if (!record_data(commandBuffer, &copy, sizeof copy, &at))
			return;
		Command *const command = record(commandBuffer, type);
		if (!command)
			return;
		command->state_at = at;
	}
}

VKAPI_ATTR void VKAPI_CALL vkCmdCopyBufferToImage(VkCommandBuffer commandBuffer, VkBuffer srcBuffer, VkImage dstImage,
	VkImageLayout dstImageLayout, uint32_t regionCount, const VkBufferImageCopy *pRegions)
{
	(void)dstImageLayout;
	record_level_copies(commandBuffer, COMMAND_COPY_BUFFER_TO_IMAGE, v4x_object(srcBuffer), v4x_object(dstImage),
		regionCount, pRegions);
}

VKAPI_ATTR void VKAPI_CALL vkCmdCopyImageToBuffer(VkCommandBuffer commandBuffer, VkImage srcImage,
	VkImageLayout srcImageLayout, VkBuffer dstBuffer, uint32_t regionCount, const VkBufferImageCopy *pRegions)
{
	(void)srcImageLayout;
	record_level_copies(commandBuffer, COMMAND_COPY_IMAGE_TO_BUFFER, v4x_object(dstBuffer), v4x_object(srcImage),
		regionCount, pRegions);
}

static void record_image_copy(VkCommandBuffer commandBuffer, CommandType type, Image *source, uint32_t source_level,
	Slices source_slices, Image *destination, uint32_t destination_level, Slices destination_slices, bool linear)
{
	Command *const command = record(commandBuffer, type);
	if (!command)
		return;
	command->image_copy.source = source;
	command->image_copy.destination = destination;
	command->image_copy.source_level = (uint8_t)source_level;
	command->image_copy.destination_level = (uint8_t)destination_level;
	command->image_copy.linear = linear;
	command->image_copy.source_slice = (uint16_t)source_slices.first;
	command->image_copy.destination_slice = (uint16_t)destination_slices.first;
	command->image_copy.slice_count = (uint16_t)source_slices.count;
}

// Whether one image's texels are compressed blocks and the other's aren't, which a copy would have to swap
// (src/transfer.c).
static bool compressed_apart(const Image *source, const Image *destination)
{
	const V4xFormat *const from = v4x_format(source->format);
	const V4xFormat *const to = v4x_format(destination->format);
	return (from && from->compressed) != (to && to->compressed);
}

// Whole levels of the same size, their bits as they are between formats of one texel size, compressed or not.
VKAPI_ATTR void VKAPI_CALL vkCmdCopyImage(VkCommandBuffer commandBuffer, VkImage srcImage, VkImageLayout srcImageLayout,
	VkImage dstImage, VkImageLayout dstImageLayout, uint32_t regionCount, const VkImageCopy *pRegions)
{
	(void)srcImageLayout;
	(void)dstImageLayout;
	Image *const source = v4x_object(srcImage);
	Image *const destination = v4x_object(dstImage);
	for (uint32_t i = 0; i < regionCount; i++) {
		const VkImageCopy *const region = &pRegions[i];
		Slices from;
		Slices to;
		if (!whole_level(source, &region->srcSubresource, region->srcOffset, region->extent, &from) ||
			!whole_level(destination, &region->dstSubresource, region->dstOffset, region->extent, &to) ||
			from.count != to.count || v4x_texel_bytes(source->format) != v4x_texel_bytes(destination->format) ||
			compressed_apart(source, destination)) {
			commandBuffer->result = v4x_host_unsupported(
				"image copies of less than a whole level, or between compressed and uncompressed formats");
			return;
		}
		record_image_copy(commandBuffer, COMMAND_COPY_IMAGE, source, region->srcSubresource.mipLevel, from, destination,
			region->dstSubresource.mipLevel, to, false);
	}
}

// Whether a blit's corners are a whole level's, unflipped, and its layers or 3D slices.
static bool whole_blit_level(
	const Image *image, const VkImageSubresourceLayers *layers, const VkOffset3D offsets[2], Slices *slices)
{
	const VkExtent3D extent = { (uint32_t)offsets[1].x, (uint32_t)offsets[1].y,
		(uint32_t)(offsets[1].z - offsets[0].z) };
	return whole_level(image, layers, offsets[0], extent, slices);
}

// Whole levels to whole levels of an image of the same format, one the CPU unpacks (v4x_blit_format).
VKAPI_ATTR void VKAPI_CALL vkCmdBlitImage(VkCommandBuffer commandBuffer, VkImage srcImage, VkImageLayout srcImageLayout,
	VkImage dstImage, VkImageLayout dstImageLayout, uint32_t regionCount, const VkImageBlit *pRegions, VkFilter filter)
{
	(void)srcImageLayout;
	(void)dstImageLayout;
	Image *const source = v4x_object(srcImage);
	Image *const destination = v4x_object(dstImage);
	for (uint32_t i = 0; i < regionCount; i++) {
		const VkImageBlit *const region = &pRegions[i];
		Slices from;
		Slices to;
		// Each layer or 3D slice into one of its own: a blit doesn't scale a 3D image's depth.
		if (!whole_blit_level(source, &region->srcSubresource, region->srcOffsets, &from) ||
			!whole_blit_level(destination, &region->dstSubresource, region->dstOffsets, &to) ||
			from.count != to.count || source->format != destination->format || !v4x_blit_format(source->format) ||
			filter > VK_FILTER_LINEAR) {
			commandBuffer->result = v4x_host_unsupported(
				"blits of less than a whole level, flipped, scaled in depth, "
				"between formats or of formats that don't blit");
			return;
		}
		record_image_copy(commandBuffer, COMMAND_BLIT_IMAGE, source, region->srcSubresource.mipLevel, from, destination,
			region->dstSubresource.mipLevel, to, filter == VK_FILTER_LINEAR);
	}
}

VKAPI_ATTR void VKAPI_CALL vkCmdClearColorImage(VkCommandBuffer commandBuffer, VkImage image, VkImageLayout imageLayout,
	const VkClearColorValue *pColor, uint32_t rangeCount, const VkImageSubresourceRange *pRanges)
{
	(void)imageLayout;
	Image *const object = v4x_object(image);
	for (uint32_t i = 0; i < rangeCount; i++) {
		const VkImageSubresourceRange *const range = &pRanges[i];
		const uint32_t levels = range->levelCount == VK_REMAINING_MIP_LEVELS
			? object->texture.levels - range->baseMipLevel
			: range->levelCount;
		const V4xFormat *const format = v4x_format(object->format);
		// A cube's faces are its layers; a 3D image's one layer is all its slices.
		const uint32_t layers = object->texture.dimension == XSDK_TEXTURE_CUBE ? XSDK_CUBE_FACES : 1;
		const uint32_t layer_count =
			range->layerCount == VK_REMAINING_ARRAY_LAYERS ? layers - range->baseArrayLayer : range->layerCount;
		// Vulkan clears no compressed image.
		if (!object->is_texture || !format || format->compressed || range->baseArrayLayer >= layers || !layer_count ||
			layer_count > layers - range->baseArrayLayer) {
			commandBuffer->result =
				v4x_host_unsupported("colour clears of images other than uncompressed colour images");
			return;
		}
		const bool is_3d = object->texture.dimension == XSDK_TEXTURE_3D;
		Command *const command = record(commandBuffer, COMMAND_CLEAR_COLOR_IMAGE);
		if (!command)
			return;
		command->clear.image = object;
		command->clear.first_level = (uint8_t)range->baseMipLevel;
		command->clear.level_count = (uint8_t)levels;
		command->clear.first_slice = (uint16_t)(is_3d ? 0 : range->baseArrayLayer);
		command->clear.slice_count = (uint16_t)(is_3d ? object->texture.depth : layer_count);
		v4x_format_pack(format, pColor->float32, command->clear.texel);
	}
}

// Depth and stencil clear together, as one register holds both (docs/SUPPORT.md, Render passes), so a range of one
// alone is refused.
VKAPI_ATTR void VKAPI_CALL vkCmdClearDepthStencilImage(VkCommandBuffer commandBuffer, VkImage image,
	VkImageLayout imageLayout, const VkClearDepthStencilValue *pDepthStencil, uint32_t rangeCount,
	const VkImageSubresourceRange *pRanges)
{
	(void)imageLayout;
	Image *const object = v4x_object(image);
	const VkImageAspectFlags both = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
	for (uint32_t i = 0; i < rangeCount; i++) {
		if (!object->is_depth || pRanges[i].aspectMask != both) {
			commandBuffer->result = v4x_host_unsupported("depth image clears of depth or stencil alone");
			return;
		}
	}
	Command *const command = rangeCount ? record(commandBuffer, COMMAND_CLEAR_DEPTH_IMAGE) : NULL;
	if (!command)
		return;
	command->depth_clear.image = object;
	command->depth_clear.depth = pDepthStencil->depth;
	command->depth_clear.stencil = (uint8_t)pDepthStencil->stencil;
}

// Each rectangle a draw the scissor cuts to it, of one layer, in the pass's one colour attachment and its depth.
VKAPI_ATTR void VKAPI_CALL vkCmdClearAttachments(VkCommandBuffer commandBuffer, uint32_t attachmentCount,
	const VkClearAttachment *pAttachments, uint32_t rectCount, const VkClearRect *pRects)
{
	ClearRect clear = { .clears_color = false };
	for (uint32_t i = 0; i < attachmentCount; i++) {
		const VkClearAttachment *const attachment = &pAttachments[i];
		if (attachment->aspectMask & VK_IMAGE_ASPECT_COLOR_BIT) {
			clear.clears_color = true;
			memcpy(clear.color, attachment->clearValue.color.float32, sizeof clear.color);
		}
		if (attachment->aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT) {
			clear.clears_depth = true;
			clear.depth = attachment->clearValue.depthStencil.depth;
		}
		if (attachment->aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT) {
			clear.clears_stencil = true;
			clear.stencil = attachment->clearValue.depthStencil.stencil;
		}
	}
	for (uint32_t i = 0; i < rectCount; i++) {
		if (pRects[i].baseArrayLayer || pRects[i].layerCount != 1) {
			commandBuffer->result = v4x_host_unsupported("attachment clears of layers past the first");
			return;
		}
		clear.rect = pRects[i].rect;
		uint32_t at = 0;
		if (!record_data(commandBuffer, &clear, sizeof clear, &at))
			return;
		Command *const command = record(commandBuffer, COMMAND_CLEAR_ATTACHMENTS);
		if (!command)
			return;
		command->state_at = at;
	}
}

static void record_buffer_copy(VkCommandBuffer commandBuffer, CommandType type, volatile const uint8_t *source,
	const Buffer *destination, VkDeviceSize offset, uint32_t size_bytes, uint32_t data)
{
	Command *const command = record(commandBuffer, type);
	if (!command)
		return;
	command->buffer_copy.source = source;
	command->buffer_copy.destination = buffer_bytes(destination, offset);
	command->buffer_copy.address = buffer_address(destination, offset);
	command->buffer_copy.size_bytes = size_bytes;
	command->buffer_copy.data = data;
}

VKAPI_ATTR void VKAPI_CALL vkCmdCopyBuffer(VkCommandBuffer commandBuffer, VkBuffer srcBuffer, VkBuffer dstBuffer,
	uint32_t regionCount, const VkBufferCopy *pRegions)
{
	for (uint32_t i = 0; i < regionCount; i++)
		record_buffer_copy(commandBuffer, COMMAND_COPY_BUFFER,
			buffer_bytes(v4x_object(srcBuffer), pRegions[i].srcOffset), v4x_object(dstBuffer), pRegions[i].dstOffset,
			(uint32_t)pRegions[i].size, 0);
}

// The bytes go into the command buffer's data as it's recorded, as the spec has it.
VKAPI_ATTR void VKAPI_CALL vkCmdUpdateBuffer(
	VkCommandBuffer commandBuffer, VkBuffer dstBuffer, VkDeviceSize dstOffset, VkDeviceSize dataSize, const void *pData)
{
	uint32_t at = 0;
	if (record_data(commandBuffer, pData, (uint32_t)dataSize, &at))
		record_buffer_copy(
			commandBuffer, COMMAND_UPDATE_BUFFER, NULL, v4x_object(dstBuffer), dstOffset, (uint32_t)dataSize, at);
}

// The word goes in the CPU's byte order, as the spec has it: the host's.
VKAPI_ATTR void VKAPI_CALL vkCmdFillBuffer(
	VkCommandBuffer commandBuffer, VkBuffer dstBuffer, VkDeviceSize dstOffset, VkDeviceSize size, uint32_t data)
{
	const Buffer *const buffer = v4x_object(dstBuffer);
	const VkDeviceSize bytes = size == VK_WHOLE_SIZE ? (buffer->size - dstOffset) & ~(VkDeviceSize)3 : size;
	record_buffer_copy(commandBuffer, COMMAND_FILL_BUFFER, NULL, buffer, dstOffset, (uint32_t)bytes, data);
}

// Replay is in order and copies wait for the GPU, so barriers need nothing. But a sampled depth image's depth reaches
// its texture only from a render pass that leaves it to be read (render_pass.c), so a barrier that moves it from an
// attachment to be read is refused rather than sampling what it held before.
static void check_image_barriers(
	VkCommandBuffer commandBuffer, uint32_t imageMemoryBarrierCount, const VkImageMemoryBarrier *pImageMemoryBarriers)
{
	for (uint32_t i = 0; i < imageMemoryBarrierCount; i++) {
		const VkImageMemoryBarrier *const barrier = &pImageMemoryBarriers[i];
		const Image *const image = v4x_object(barrier->image);
		if (image->is_sampled_depth && barrier->oldLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL &&
			v4x_read_layout(barrier->newLayout)) {
			commandBuffer->result = v4x_host_unsupported(
				"barriers moving a sampled depth image from an attachment to be read; a render pass must leave it so");
			return;
		}
	}
}

VKAPI_ATTR void VKAPI_CALL vkCmdPipelineBarrier(VkCommandBuffer commandBuffer, VkPipelineStageFlags srcStageMask,
	VkPipelineStageFlags dstStageMask, VkDependencyFlags dependencyFlags, uint32_t memoryBarrierCount,
	const VkMemoryBarrier *pMemoryBarriers, uint32_t bufferMemoryBarrierCount,
	const VkBufferMemoryBarrier *pBufferMemoryBarriers, uint32_t imageMemoryBarrierCount,
	const VkImageMemoryBarrier *pImageMemoryBarriers)
{
	(void)srcStageMask;
	(void)dstStageMask;
	(void)dependencyFlags;
	(void)memoryBarrierCount;
	(void)pMemoryBarriers;
	(void)bufferMemoryBarrierCount;
	(void)pBufferMemoryBarriers;
	check_image_barriers(commandBuffer, imageMemoryBarrierCount, pImageMemoryBarriers);
}

// The GPU writes the event's word once the work before it is done, whatever the stages.
static void record_event(VkCommandBuffer commandBuffer, VkEvent event, uint32_t value)
{
	Command *const command = record(commandBuffer, COMMAND_WRITE_EVENT);
	if (!command)
		return;
	command->event.address = ((const Event *)v4x_object(event))->address;
	command->event.value = value;
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetEvent(VkCommandBuffer commandBuffer, VkEvent event, VkPipelineStageFlags stageMask)
{
	(void)stageMask;
	record_event(commandBuffer, event, 1);
}

VKAPI_ATTR void VKAPI_CALL vkCmdResetEvent(VkCommandBuffer commandBuffer, VkEvent event, VkPipelineStageFlags stageMask)
{
	(void)stageMask;
	record_event(commandBuffer, event, 0);
}

// The GPU waits on each event's word; the barriers are a pipeline barrier's.
VKAPI_ATTR void VKAPI_CALL vkCmdWaitEvents(VkCommandBuffer commandBuffer, uint32_t eventCount, const VkEvent *pEvents,
	VkPipelineStageFlags srcStageMask, VkPipelineStageFlags dstStageMask, uint32_t memoryBarrierCount,
	const VkMemoryBarrier *pMemoryBarriers, uint32_t bufferMemoryBarrierCount,
	const VkBufferMemoryBarrier *pBufferMemoryBarriers, uint32_t imageMemoryBarrierCount,
	const VkImageMemoryBarrier *pImageMemoryBarriers)
{
	(void)srcStageMask;
	(void)dstStageMask;
	(void)memoryBarrierCount;
	(void)pMemoryBarriers;
	(void)bufferMemoryBarrierCount;
	(void)pBufferMemoryBarriers;
	check_image_barriers(commandBuffer, imageMemoryBarrierCount, pImageMemoryBarriers);
	uint32_t at = 0;
	if (!record_data(commandBuffer, pEvents, eventCount * sizeof *pEvents, &at))
		return;
	Command *const command = record(commandBuffer, COMMAND_WAIT_EVENTS);
	if (!command)
		return;
	command->wait_events.events_at = at;
	command->wait_events.count = eventCount;
}

VKAPI_ATTR void VKAPI_CALL vkCmdResetQueryPool(
	VkCommandBuffer commandBuffer, VkQueryPool queryPool, uint32_t firstQuery, uint32_t queryCount)
{
	Command *const command = record(commandBuffer, COMMAND_RESET_QUERIES);
	if (!command)
		return;
	command->queries.pool = v4x_object(queryPool);
	command->queries.first = firstQuery;
	command->queries.count = queryCount;
}

// Only time stamp pools can be made (query.c), and they're never begun.
VKAPI_ATTR void VKAPI_CALL vkCmdBeginQuery(
	VkCommandBuffer commandBuffer, VkQueryPool queryPool, uint32_t query, VkQueryControlFlags flags)
{
	(void)queryPool;
	(void)query;
	(void)flags;
	commandBuffer->result = v4x_host_unsupported("occlusion queries");
}

VKAPI_ATTR void VKAPI_CALL vkCmdEndQuery(VkCommandBuffer commandBuffer, VkQueryPool queryPool, uint32_t query)
{
	(void)queryPool;
	(void)query;
	commandBuffer->result = v4x_host_unsupported("occlusion queries");
}

// At the top of the pipe the clock is copied as the command processor reaches it; at any later stage once the GPU is
// idle, the latest any stage finishes.
VKAPI_ATTR void VKAPI_CALL vkCmdWriteTimestamp(
	VkCommandBuffer commandBuffer, VkPipelineStageFlagBits pipelineStage, VkQueryPool queryPool, uint32_t query)
{
	Command *const command = record(commandBuffer, COMMAND_WRITE_TIMESTAMP);
	if (!command)
		return;
	command->timestamp.pool = v4x_object(queryPool);
	command->timestamp.query = query;
	command->timestamp.after_work = pipelineStage != VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
}

VKAPI_ATTR void VKAPI_CALL vkCmdCopyQueryPoolResults(VkCommandBuffer commandBuffer, VkQueryPool queryPool,
	uint32_t firstQuery, uint32_t queryCount, VkBuffer dstBuffer, VkDeviceSize dstOffset, VkDeviceSize stride,
	VkQueryResultFlags flags)
{
	const Buffer *const buffer = v4x_object(dstBuffer);
	const QueryCopy copy = {
		.pool = v4x_object(queryPool),
		.first = firstQuery,
		.count = queryCount,
		.destination = buffer_bytes(buffer, dstOffset),
		.address = buffer_address(buffer, dstOffset),
		.stride_bytes = (uint32_t)stride,
		.flags = flags,
	};
	uint32_t at = 0;
	if (!record_data(commandBuffer, &copy, sizeof copy, &at))
		return;
	Command *const command = record(commandBuffer, COMMAND_COPY_QUERY_RESULTS);
	if (command)
		command->state_at = at;
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetViewport(
	VkCommandBuffer commandBuffer, uint32_t firstViewport, uint32_t viewportCount, const VkViewport *pViewports)
{
	uint32_t at = 0;
	if (firstViewport || viewportCount != 1) {
		commandBuffer->result = v4x_host_unsupported("viewports other than the first");
		return;
	}
	if (!record_data(commandBuffer, pViewports, sizeof *pViewports, &at))
		return;
	Command *const command = record(commandBuffer, COMMAND_SET_VIEWPORT);
	if (command)
		command->state_at = at;
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetScissor(
	VkCommandBuffer commandBuffer, uint32_t firstScissor, uint32_t scissorCount, const VkRect2D *pScissors)
{
	uint32_t at = 0;
	if (firstScissor || scissorCount != 1) {
		commandBuffer->result = v4x_host_unsupported("scissors other than the first");
		return;
	}
	if (!record_data(commandBuffer, pScissors, sizeof *pScissors, &at))
		return;
	Command *const command = record(commandBuffer, COMMAND_SET_SCISSOR);
	if (command)
		command->state_at = at;
}

// Without wideLines, 1 is the only width, and lines are drawn 1 pixel wide (docs/SUPPORT.md, Pipelines).
VKAPI_ATTR void VKAPI_CALL vkCmdSetLineWidth(VkCommandBuffer commandBuffer, float lineWidth)
{
	if (lineWidth != 1.0f)
		commandBuffer->result = v4x_host_unsupported("line widths other than 1");
}

// Both faces share one stencil setting (docs/SUPPORT.md, Pipelines), so a setting for one face alone is refused.
static void record_stencil(
	VkCommandBuffer commandBuffer, VkStencilFaceFlags faceMask, VkDynamicState state, uint32_t value)
{
	if (faceMask != VK_STENCIL_FACE_FRONT_AND_BACK) {
		commandBuffer->result = v4x_host_unsupported("stencil masks and references set for one face alone");
		return;
	}
	Command *const command = record(commandBuffer, COMMAND_SET_STENCIL);
	if (!command)
		return;
	command->stencil.state = (uint32_t)state;
	command->stencil.value = (uint8_t)value;
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetStencilCompareMask(
	VkCommandBuffer commandBuffer, VkStencilFaceFlags faceMask, uint32_t compareMask)
{
	record_stencil(commandBuffer, faceMask, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, compareMask);
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetStencilWriteMask(
	VkCommandBuffer commandBuffer, VkStencilFaceFlags faceMask, uint32_t writeMask)
{
	record_stencil(commandBuffer, faceMask, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, writeMask);
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetStencilReference(
	VkCommandBuffer commandBuffer, VkStencilFaceFlags faceMask, uint32_t reference)
{
	record_stencil(commandBuffer, faceMask, VK_DYNAMIC_STATE_STENCIL_REFERENCE, reference);
}

// Without depthBiasClamp, the clamp is 0 (docs/SUPPORT.md, Pipelines).
VKAPI_ATTR void VKAPI_CALL vkCmdSetDepthBias(
	VkCommandBuffer commandBuffer, float depthBiasConstantFactor, float depthBiasClamp, float depthBiasSlopeFactor)
{
	if (depthBiasClamp != 0.0f) {
		commandBuffer->result = v4x_host_unsupported("depth bias clamps");
		return;
	}
	Command *const command = record(commandBuffer, COMMAND_SET_DEPTH_BIAS);
	if (!command)
		return;
	command->depth_bias.constant = depthBiasConstantFactor;
	command->depth_bias.slope = depthBiasSlopeFactor;
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetBlendConstants(VkCommandBuffer commandBuffer, const float blendConstants[4])
{
	Command *const command = record(commandBuffer, COMMAND_SET_BLEND_CONSTANTS);
	if (command)
		memcpy(command->blend_constants, blendConstants, sizeof command->blend_constants);
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetDepthBounds(
	VkCommandBuffer commandBuffer, float minDepthBounds, float maxDepthBounds)
{
	(void)minDepthBounds;
	(void)maxDepthBounds;
	commandBuffer->result = v4x_host_unsupported("depth bounds");
}

VKAPI_ATTR void VKAPI_CALL vkCmdDispatch(
	VkCommandBuffer commandBuffer, uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ)
{
	(void)groupCountX;
	(void)groupCountY;
	(void)groupCountZ;
	commandBuffer->result = v4x_host_unsupported("compute dispatches");
}

VKAPI_ATTR void VKAPI_CALL vkCmdDispatchIndirect(VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset)
{
	(void)buffer;
	(void)offset;
	commandBuffer->result = v4x_host_unsupported("compute dispatches");
}

// Images have one sample (docs/SUPPORT.md, Images), so there's none to resolve.
VKAPI_ATTR void VKAPI_CALL vkCmdResolveImage(VkCommandBuffer commandBuffer, VkImage srcImage,
	VkImageLayout srcImageLayout, VkImage dstImage, VkImageLayout dstImageLayout, uint32_t regionCount,
	const VkImageResolve *pRegions)
{
	(void)srcImage;
	(void)srcImageLayout;
	(void)dstImage;
	(void)dstImageLayout;
	(void)regionCount;
	(void)pRegions;
	commandBuffer->result = v4x_host_unsupported("resolving multisampled images");
}

VKAPI_ATTR void VKAPI_CALL vkCmdBindIndexBuffer(
	VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset, VkIndexType indexType)
{
	Command *const command = record(commandBuffer, COMMAND_BIND_INDEX_BUFFER);
	if (!command)
		return;
	const Buffer *const object = v4x_object(buffer);
	command->index_buffer.address = object->memory->address + object->offset_bytes + (uint32_t)offset;
	command->index_buffer.is_32_bit = indexType == VK_INDEX_TYPE_UINT32;
}

VKAPI_ATTR void VKAPI_CALL vkCmdDrawIndexed(VkCommandBuffer commandBuffer, uint32_t indexCount, uint32_t instanceCount,
	uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance)
{
	if (!indexCount || !instanceCount || !splittable(commandBuffer, indexCount))
		return;
	Command *const command = record(commandBuffer, COMMAND_DRAW_INDEXED);
	if (!command)
		return;
	command->draw_indexed.index_count = indexCount;
	command->draw_indexed.first_index = firstIndex;
	command->draw_indexed.vertex_offset = vertexOffset;
	command->draw_indexed.instance_count = instanceCount;
	command->draw_indexed.first_instance = firstInstance;
}

// One draw at most, without multiDrawIndirect; its parameters are read at replay (queue.c).
static void record_indirect(
	VkCommandBuffer commandBuffer, CommandType type, VkBuffer buffer, VkDeviceSize offset, uint32_t drawCount)
{
	if (drawCount > 1) {
		commandBuffer->result = v4x_host_unsupported("indirect draws of more than one draw");
		return;
	}
	Command *const command = drawCount ? record(commandBuffer, type) : NULL;
	if (command)
		command->parameters = (volatile const uint32_t *)buffer_bytes(v4x_object(buffer), offset);
}

VKAPI_ATTR void VKAPI_CALL vkCmdDrawIndirect(
	VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset, uint32_t drawCount, uint32_t stride)
{
	(void)stride;
	record_indirect(commandBuffer, COMMAND_DRAW_INDIRECT, buffer, offset, drawCount);
}

VKAPI_ATTR void VKAPI_CALL vkCmdDrawIndexedIndirect(
	VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset, uint32_t drawCount, uint32_t stride)
{
	(void)stride;
	record_indirect(commandBuffer, COMMAND_DRAW_INDEXED_INDIRECT, buffer, offset, drawCount);
}
