#ifndef V4X_OBJECTS_H
#define V4X_OBJECTS_H

#include <stdbool.h>
#include <stdint.h>

#include <vulkan/vulkan.h>

#include "vulkan4xenon.h"
#include "xsdk/shader.h"
#include "xsdk/texture.h"

// Every object the layer hands out, and the limits they're built to.

enum {
	V4X_MIN_SWAPCHAIN_IMAGES = 2,
	// xsdk_present's limit (xsdk:include/xsdk/present.h).
	V4X_MAX_SWAPCHAIN_IMAGES = 3,
	V4X_DISPLAY_WIDTH = 1280,
	V4X_DISPLAY_HEIGHT = 720,
	// Each binding takes one of xsdk's vertex streams.
	V4X_MAX_VERTEX_BINDINGS = XSDK_VERTEX_STREAMS,
	V4X_MAX_VERTEX_ATTRIBUTES = 16,
	// A fetch's stride is 8 bits of dwords (xsdk:src/shader.c, FETCH_STRIDE_MASK).
	V4X_MAX_VERTEX_STRIDE_BYTES = 255 * 4,
	V4X_COMMANDS_PER_BUFFER = 4096,
	// Push constants, dynamic offsets and copy regions, which vary in size, go here in recording order.
	V4X_COMMAND_DATA_BYTES = 16384,
	V4X_MAX_DESCRIPTOR_SETS = 4,
	// Binding numbers run from 0 to this less one.
	V4X_MAX_DESCRIPTOR_BINDINGS = 32,
	V4X_MAX_PUSH_CONSTANT_BYTES = 128,
	// Float constant registers a stage has, each four floats (xsdk:include/xsdk/gpu.h).
	V4X_FLOAT_CONSTANTS = 256,
	// Fetch constants a shader may sample from: below the ones the 16 vertex streams take (xsdk:src/gpu_draw.c).
	V4X_MAX_SAMPLERS = 16,
	V4X_MAX_SHADER_RESOURCES = 2 * (1 + V4X_MAX_DESCRIPTOR_SETS * V4X_MAX_DESCRIPTOR_BINDINGS),
};

struct VkPhysicalDevice_T {
	VkInstance instance;
};

struct VkInstance_T {
	VkAllocationCallbacks allocator;
	bool has_allocator;
	struct VkPhysicalDevice_T physical_device;
};

// What eDRAM's colour holds between render passes: a resolve clears it to a known colour.
typedef struct {
	bool known;
	uint32_t argb;
} Edram;

struct VkQueue_T {
	VkDevice device;
	Edram edram;
};

struct VkDevice_T {
	VkAllocationCallbacks allocator;
	bool has_allocator;
	struct VkQueue_T queue;
};

typedef struct {
	volatile uint32_t *pointer;
	uint32_t address;
	uint32_t size_bytes;
} DeviceMemory;

typedef struct {
	VkDeviceSize size;
	const DeviceMemory *memory;
	uint32_t offset_bytes;
} Buffer;

// A swapchain image, a front buffer of xsdk_present at address, or a texture in memory bound to it.
typedef struct {
	uint32_t address;
	uint32_t width;
	uint32_t height;
	VkFormat format;
	bool is_texture;
	XsdkTexture texture;
} Image;

typedef struct {
	const Image *image;
} ImageView;

typedef struct {
	const V4xShader *shader;
} ShaderModule;

typedef struct {
	XsdkSamplerState state;
} Sampler;

typedef struct {
	VkDescriptorType type;
	VkShaderStageFlags stages;
	// Where this binding's offset sits among the set's dynamic offsets.
	uint32_t dynamic_index;
	bool used;
} LayoutBinding;

typedef struct {
	LayoutBinding bindings[V4X_MAX_DESCRIPTOR_BINDINGS];
	uint32_t dynamic_count;
} DescriptorSetLayout;

typedef struct {
	VkDescriptorType type;
	// A uniform buffer's bytes, mapped, and how many of them the descriptor covers.
	volatile const uint32_t *buffer;
	uint32_t range_bytes;
	const Image *image;
	const Sampler *sampler;
} Descriptor;

typedef struct DescriptorSet {
	const DescriptorSetLayout *layout;
	struct DescriptorSet *next;
	Descriptor descriptors[V4X_MAX_DESCRIPTOR_BINDINGS];
} DescriptorSet;

typedef struct {
	DescriptorSet *sets;
} DescriptorPool;

typedef struct {
	const DescriptorSetLayout *sets[V4X_MAX_DESCRIPTOR_SETS];
	uint32_t set_count;
	VkShaderStageFlags push_stages;
} PipelineLayout;

// A shader resource with the stage it's for.
typedef struct {
	V4xShaderResource resource;
	bool vertex;
} StageResource;

typedef struct {
	uint32_t binding;
	uint32_t stride_bytes;
} VertexStream;

typedef struct {
	XsdkShader vertex;
	XsdkShader pixel;
	const V4xShader *vertex_source;
	const V4xShader *pixel_source;
	VertexStream streams[V4X_MAX_VERTEX_BINDINGS];
	uint32_t stream_count;
	VkViewport viewport;
	VkRect2D scissor;
	StageResource resources[V4X_MAX_SHADER_RESOURCES];
	uint32_t resource_count;
} Pipeline;

typedef struct {
	VkFormat format;
	VkAttachmentLoadOp load_op;
	VkAttachmentStoreOp store_op;
} RenderPass;

typedef struct {
	const Image *attachment;
	uint32_t width;
	uint32_t height;
} Framebuffer;

typedef enum {
	COMMAND_BEGIN_RENDER_PASS,
	COMMAND_END_RENDER_PASS,
	COMMAND_BIND_PIPELINE,
	COMMAND_BIND_VERTEX_BUFFER,
	COMMAND_DRAW,
	COMMAND_BIND_DESCRIPTOR_SET,
	COMMAND_PUSH_CONSTANTS,
	COMMAND_COPY_BUFFER_TO_IMAGE,
} CommandType;

typedef struct {
	CommandType type;
	union {
		struct {
			const RenderPass *render_pass;
			const Framebuffer *framebuffer;
			uint32_t clear_argb;
		} begin;
		const Pipeline *pipeline;
		struct {
			uint32_t binding;
			uint32_t address;
			uint32_t size_bytes;
		} vertex_buffer;
		struct {
			uint32_t vertex_count;
			uint32_t first_vertex;
		} draw;
		struct {
			uint32_t index;
			const DescriptorSet *set;
			// The set's dynamic offsets, in the command buffer's data.
			uint32_t offsets_at;
		} descriptor_set;
		struct {
			uint32_t offset_bytes;
			uint32_t size_bytes;
			uint32_t data_at;
		} push_constants;
		struct {
			volatile const uint32_t *source;
			Image *image;
			uint32_t level;
			uint32_t row_bytes;
		} copy;
	};
} Command;

typedef enum {
	COMMAND_BUFFER_INITIAL,
	COMMAND_BUFFER_RECORDING,
	COMMAND_BUFFER_EXECUTABLE,
} CommandBufferState;

struct VkCommandBuffer_T {
	VkDevice device;
	struct CommandPool *pool;
	struct VkCommandBuffer_T *next;
	CommandBufferState state;
	VkResult result;
	Command *commands;
	uint32_t count;
	uint8_t *data;
	uint32_t data_bytes;
	bool in_render_pass;
};

typedef struct CommandPool {
	struct VkCommandBuffer_T *buffers;
} CommandPool;

typedef struct {
	bool signaled;
	bool pending;
	uint32_t number;
} Fence;

typedef struct {
	uint32_t unused;
} Semaphore;

typedef struct {
	uint32_t width;
	uint32_t height;
} Surface;

typedef struct {
	Image images[V4X_MAX_SWAPCHAIN_IMAGES];
	uint32_t image_count;
	bool acquired;
	uint32_t acquired_index;
} Swapchain;

// Non-dispatchable handles are 64-bit integers on 32-bit targets, so they carry the object's pointer.
static inline uint64_t v4x_handle(const void *object)
{
	return (uint64_t)(uintptr_t)object;
}

static inline void *v4x_object(uint64_t handle)
{
	return (void *)(uintptr_t)handle;
}

#endif
