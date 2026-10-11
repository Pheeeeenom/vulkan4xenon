#ifndef V4X_HARNESS_H
#define V4X_HARNESS_H

#include <stdbool.h>
#include <stdint.h>

#include <vulkan/vulkan.h>

#include "vulkan4xenon.h"

// What every test shares: the device, the swapchain on the display, and a way to read the screen.

enum {
	HARNESS_WIDTH = 1280,
	HARNESS_HEIGHT = 720,
	HARNESS_MAX_IMAGES = 3,
	// Two frames' worth of vblanks, for a present to reach the screen.
	HARNESS_SETTLE_MS = 50,
};

typedef struct {
	VkInstance instance;
	VkPhysicalDevice physical_device;
	VkDevice device;
	VkQueue queue;
	VkSurfaceKHR surface;
	VkSwapchainKHR swapchain;
	uint32_t image_count;
	VkImage images[HARNESS_MAX_IMAGES];
	VkImageView views[HARNESS_MAX_IMAGES];
	VkCommandPool pool;
	VkFence fence;
} Harness;

typedef struct {
	uint32_t red;
	uint32_t green;
	uint32_t blue;
} Rgb;

void check(const char *name, bool passed);
// Logs a failed call and returns whether it succeeded.
bool harness_ok(const char *call, VkResult result);
VkResult harness_create_device(Harness *harness);
VkResult harness_create_swapchain(Harness *harness, uint32_t image_count);
void harness_destroy(Harness *harness);
// A buffer of host-visible memory holding the data.
VkResult harness_buffer(const Harness *harness, VkBufferUsageFlags usage, const void *data, uint32_t size_bytes,
	VkBuffer *buffer, VkDeviceMemory *memory);
VkResult harness_shader(const Harness *harness, const uint32_t *spirv, uint32_t size_bytes, VkShaderModule *module);
// The GPU address of swapchain image index, as xsdk_present lays the images out.
uint32_t harness_image_address(uint32_t index);
// A pixel of the front buffer at this address.
Rgb harness_pixel(uint32_t address, uint32_t x, uint32_t y);
// Whether each channel is within tolerance of the one wanted.
bool harness_near(Rgb pixel, Rgb want, uint32_t tolerance);

// What a test draws with: shaders, vertex input and the pipeline layout, over harness_render_pass's pass.
typedef struct {
	const uint32_t *vertex_spirv;
	uint32_t vertex_bytes;
	const uint32_t *fragment_spirv;
	uint32_t fragment_bytes;
	const VkVertexInputBindingDescription *bindings;
	uint32_t binding_count;
	const VkVertexInputAttributeDescription *attributes;
	uint32_t attribute_count;
	VkPipelineLayout layout;
} HarnessPipeline;

// Whether an image the test drew holds what it should; failures are counted and the first few logged by the test.
typedef bool (*HarnessCheck)(uint32_t address, const char *what, void *context);

// One subpass that clears a swapchain image and stores it, and a framebuffer for each image.
VkResult harness_render_pass(const Harness *harness, VkRenderPass *render_pass, VkFramebuffer *framebuffers);
void harness_destroy_render_pass(const Harness *harness, VkRenderPass render_pass, const VkFramebuffer *framebuffers);
// Triangle lists over the whole screen: no culling, depth or blending.
VkResult harness_pipeline(
	const Harness *harness, const HarnessPipeline *spec, VkRenderPass render_pass, VkPipeline *pipeline);
// A command buffer for each swapchain image, begun; the test records into them and ends them.
VkResult harness_command_buffers(const Harness *harness, VkCommandBuffer *commands);
// Runs frames: acquires, replays the image's commands, checks the image they drew, then presents it. Returns how many
// frames went through; misses counts the images that failed the check.
uint32_t harness_frames(const Harness *harness, const VkCommandBuffer *commands, uint32_t frames, HarnessCheck check,
	void *context, uint32_t *misses);
// Once the frames have shown, whether the image on screen is a swapchain image and passes the check.
bool harness_screen(const Harness *harness, HarnessCheck check, void *context);
// Frames the display has flipped to so far.
uint32_t harness_flips(void);

// A pixel a test expects a colour at.
typedef struct {
	const char *name;
	uint32_t x;
	uint32_t y;
	Rgb want;
} HarnessProbe;

typedef struct {
	const char *test;
	const HarnessProbe *probes;
	uint32_t count;
	uint32_t tolerance;
	uint32_t logged;
} HarnessProbes;

// A HarnessCheck: every probe holds its colour; the first few misses are logged. The context is a HarnessProbes.
bool harness_probes_hold(uint32_t address, const char *what, void *context);

#endif
