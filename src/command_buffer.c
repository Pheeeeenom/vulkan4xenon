#include <math.h>

#include <vulkan/vulkan.h>

#include "host.h"
#include "objects.h"

// Commands go into a list each command buffer gets when it's allocated; vkQueueSubmit replays them (queue.c).

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
	buffer->in_render_pass = false;
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
		v4x_host_alloc(allocator, V4X_COMMANDS_PER_BUFFER * sizeof(Command), VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
	if (!buffer->commands) {
		v4x_host_free(allocator, buffer);
		return NULL;
	}
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
	if (pAllocateInfo->level != VK_COMMAND_BUFFER_LEVEL_PRIMARY)
		return v4x_host_unsupported("secondary command buffers");
	CommandPool *const pool = v4x_object(pAllocateInfo->commandPool);
	for (uint32_t i = 0; i < pAllocateInfo->commandBufferCount; i++) {
		pCommandBuffers[i] = allocate_buffer(device, pool);
		if (!pCommandBuffers[i]) {
			vkFreeCommandBuffers(device, pAllocateInfo->commandPool, i, pCommandBuffers);
			for (uint32_t j = 0; j < pAllocateInfo->commandBufferCount; j++)
				pCommandBuffers[j] = VK_NULL_HANDLE;
			return VK_ERROR_OUT_OF_HOST_MEMORY;
		}
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

// The next free command, or NULL with the error saved for vkEndCommandBuffer once the list is full.
static Command *record(VkCommandBuffer buffer, CommandType type)
{
	if (buffer->count == V4X_COMMANDS_PER_BUFFER) {
		buffer->result = VK_ERROR_OUT_OF_HOST_MEMORY;
		return NULL;
	}
	Command *const command = &buffer->commands[buffer->count++];
	command->type = type;
	return command;
}

static uint32_t unorm8(float value)
{
	const float clamped = value < 0.0f ? 0.0f : value > 1.0f ? 1.0f : value;
	return (uint32_t)lrintf(clamped * 255.0f);
}

VKAPI_ATTR void VKAPI_CALL vkCmdBeginRenderPass(
	VkCommandBuffer commandBuffer, const VkRenderPassBeginInfo *pRenderPassBegin, VkSubpassContents contents)
{
	const RenderPass *const render_pass = v4x_object(pRenderPassBegin->renderPass);
	const Framebuffer *const framebuffer = v4x_object(pRenderPassBegin->framebuffer);
	const VkRect2D *const area = &pRenderPassBegin->renderArea;
	if (contents != VK_SUBPASS_CONTENTS_INLINE || area->offset.x || area->offset.y ||
		area->extent.width != framebuffer->width || area->extent.height != framebuffer->height) {
		commandBuffer->result = v4x_host_unsupported("render areas smaller than the framebuffer");
		return;
	}
	Command *const command = record(commandBuffer, COMMAND_BEGIN_RENDER_PASS);
	if (!command)
		return;
	uint32_t argb = 0;
	if (render_pass->load_op == VK_ATTACHMENT_LOAD_OP_CLEAR) {
		const float *const rgba = pRenderPassBegin->pClearValues[0].color.float32;
		argb = unorm8(rgba[3]) << 24 | unorm8(rgba[0]) << 16 | unorm8(rgba[1]) << 8 | unorm8(rgba[2]);
	}
	command->begin.render_pass = render_pass;
	command->begin.framebuffer = framebuffer;
	command->begin.clear_argb = argb;
	commandBuffer->in_render_pass = true;
}

VKAPI_ATTR void VKAPI_CALL vkCmdEndRenderPass(VkCommandBuffer commandBuffer)
{
	(void)record(commandBuffer, COMMAND_END_RENDER_PASS);
	commandBuffer->in_render_pass = false;
}

VKAPI_ATTR void VKAPI_CALL vkCmdBindPipeline(
	VkCommandBuffer commandBuffer, VkPipelineBindPoint pipelineBindPoint, VkPipeline pipeline)
{
	(void)pipelineBindPoint;
	Command *const command = record(commandBuffer, COMMAND_BIND_PIPELINE);
	if (command)
		command->pipeline = v4x_object(pipeline);
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

VKAPI_ATTR void VKAPI_CALL vkCmdDraw(VkCommandBuffer commandBuffer, uint32_t vertexCount, uint32_t instanceCount,
	uint32_t firstVertex, uint32_t firstInstance)
{
	if (instanceCount != 1 || firstInstance) {
		commandBuffer->result = v4x_host_unsupported("instanced draws");
		return;
	}
	if (!vertexCount)
		return;
	Command *const command = record(commandBuffer, COMMAND_DRAW);
	if (!command)
		return;
	command->draw.vertex_count = vertexCount;
	command->draw.first_vertex = firstVertex;
}
