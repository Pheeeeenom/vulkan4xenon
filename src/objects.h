#ifndef V4X_OBJECTS_H
#define V4X_OBJECTS_H

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

#include <vulkan/vulkan.h>

#include "vulkan4xenon.h"
#include "xsdk/gpu.h"
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
	// Room a command buffer starts with; it doubles when full.
	V4X_FIRST_COMMANDS = 4096,
	// DRAW_INDX_2 and DRAW_INDX carry the count in 16 bits (xsdk:src/gpu_draw.c:219, 309).
	V4X_MAX_DRAW_VERTICES = 65535,
	// Push constants, dynamic offsets and copy regions, which vary in size, go in a command buffer's data in recording
	// order; it starts with this much and doubles when full.
	V4X_FIRST_COMMAND_DATA_BYTES = 16384,
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

typedef enum {
	EDRAM_UNKNOWN,
	// A resolve cleared it to known values, over a framebuffer of a size.
	EDRAM_CLEARED,
	// It holds an image's contents.
	EDRAM_IMAGE,
} EdramContents;

// What eDRAM's colour, or a depth and stencil, holds between render passes (docs/ROADMAP.md, milestone 4); a depth's
// first tile, colour's being tile 0.
typedef struct {
	EdramContents contents;
	uint32_t width;
	uint32_t height;
	uint32_t argb;
	float depth;
	uint32_t stencil;
	const struct Image *image;
	uint32_t tile;
} EdramPart;

// The layer's own shaders and state for drawing an image back into eDRAM.
typedef struct {
	XsdkShader vertex;
	XsdkShader pixel;
	volatile uint32_t *vertices;
	uint32_t vertices_address;
	uint32_t texture_slot;
	XsdkSamplerState sampler;
	XsdkBlendState blend;
	XsdkRasterState raster;
	XsdkDepthStencilState depth_stencil;
} Blit;

enum {
	// The fences of this many submissions are kept, for copies into images they used.
	V4X_SUBMIT_FENCES = 64,
	// The depths eDRAM keeps track of at once.
	V4X_EDRAM_DEPTHS = 4,
};

struct VkQueue_T {
	VkDevice device;
	EdramPart color;
	// Depths at tiles of their own, so a pass with depth of its own needn't take an earlier pass's (milestone 14).
	EdramPart depths[V4X_EDRAM_DEPTHS];
	// The depth and stencil resolves clear to, as last given to xsdk_gpu_set_depth_clear.
	float depth_clear;
	uint32_t stencil_clear;
	Blit blit;
	// xsdk's count of frames XAM drew the Guide in, as last seen; XAM's drawing leaves eDRAM holding none of the parts.
	uint32_t system_draws;
	// The submission being replayed, numbered from 1, and the fence each recent one ended with, at serial % the count.
	uint32_t serial;
	uint32_t submit_fences[V4X_SUBMIT_FENCES];
};

struct VkDevice_T {
	VkAllocationCallbacks allocator;
	bool has_allocator;
	struct VkQueue_T queue;
	// Shaders compiled on the console (shader.c), kept till the device goes; shaders_lock guards them, as Vulkan lets
	// modules be made on several threads.
	pthread_mutex_t shaders_lock;
	V4xShader **shaders;
	uint32_t shader_count;
	uint32_t shader_capacity;
	// The samplerAnisotropy feature, enabled when the device was made.
	bool sampler_anisotropy;
};

typedef struct {
	volatile uint32_t *pointer;
	uint32_t address;
	uint32_t size_bytes;
	// The CPU's cached memory, which the GPU never reads: transfer-source buffers' alone (docs/SUPPORT.md).
	bool cached;
} DeviceMemory;

typedef struct {
	VkDeviceSize size;
	VkBufferUsageFlags usage;
	const DeviceMemory *memory;
	uint32_t offset_bytes;
} Buffer;

// A swapchain image, a front buffer of xsdk_present at address, or a texture in memory bound to it. Both have a
// texture, to sample or to draw back into eDRAM.
typedef struct Image {
	uint32_t address;
	uint32_t width;
	uint32_t height;
	VkFormat format;
	bool is_texture;
	// Render passes may resolve into it.
	bool is_attachment;
	// A depth attachment lives in eDRAM, its memory never used unless it's made to be sampled: then a render pass that
	// stores it and leaves it to be read resolves it into its texture, D24S8.
	bool is_depth;
	bool is_sampled_depth;
	XsdkTexture texture;
	// The submission that last sampled it, resolved into it or drew it back into eDRAM, so a copy waits for that
	// one alone; 0 for none.
	uint32_t used_serial;
} Image;

typedef struct {
	Image *image;
	// The image's texture as the view samples it, its components mapped.
	XsdkTexture texture;
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
	// A uniform buffer's bytes, mapped and as the GPU addresses them, and how many of them the descriptor covers.
	volatile const uint32_t *buffer;
	uint32_t buffer_address;
	uint32_t range_bytes;
	Image *image;
	// The view's texture.
	const XsdkTexture *texture;
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
	bool dynamic_viewport;
	bool dynamic_scissor;
	uint32_t primitive;
	// Vertices a primitive takes in a list, so long lists split on whole primitives; 0 for strips and fans.
	uint32_t list_vertices;
	XsdkBlendState blend;
	XsdkRasterState raster;
	XsdkDepthStencilState depth_stencil;
	StageResource resources[V4X_MAX_SHADER_RESOURCES];
	uint32_t resource_count;
} Pipeline;

typedef struct {
	VkFormat format;
	VkAttachmentLoadOp load_op;
	VkAttachmentStoreOp store_op;
	uint32_t color_index;
	bool has_depth;
	uint32_t depth_index;
	bool clears_depth;
	bool loads_depth;
	// It stores depth or stencil and leaves the attachment in a layout to sample it in, so a sampled depth image is
	// resolved into its texture at its end; one kept as an attachment stays in eDRAM alone.
	bool leaves_depth_to_read;
} RenderPass;

typedef struct {
	Image *attachment;
	Image *depth;
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
	COMMAND_SET_VIEWPORT,
	COMMAND_SET_SCISSOR,
	COMMAND_BIND_INDEX_BUFFER,
	COMMAND_DRAW_INDEXED,
} CommandType;

typedef struct {
	CommandType type;
	union {
		struct {
			const RenderPass *render_pass;
			const Framebuffer *framebuffer;
			uint32_t clear_argb;
			float clear_depth;
			uint32_t clear_stencil;
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
		// A VkViewport or VkRect2D in the command buffer's data.
		uint32_t state_at;
		struct {
			uint32_t address;
			bool is_32_bit;
		} index_buffer;
		struct {
			uint32_t index_count;
			uint32_t first_index;
			int32_t vertex_offset;
		} draw_indexed;
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
	uint32_t capacity;
	uint8_t *data;
	uint32_t data_bytes;
	uint32_t data_capacity_bytes;
	bool in_render_pass;
	// The pipeline bound last while recording, to check draws against its topology.
	const Pipeline *pipeline;
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
	// vkGetEventStatus isn't externally synchronized against vkSetEvent, so it's read and written atomically.
	bool set;
} Event;

typedef struct {
	uint32_t unused;
} PipelineCache;

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
