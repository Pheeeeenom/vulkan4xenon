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
VkResult harness_buffer(
	const Harness *harness, const void *data, uint32_t size_bytes, VkBuffer *buffer, VkDeviceMemory *memory);
VkResult harness_shader(const Harness *harness, const uint32_t *spirv, uint32_t size_bytes, VkShaderModule *module);
// The GPU address of swapchain image index, as xsdk_present lays the images out.
uint32_t harness_image_address(uint32_t index);
// A pixel of the front buffer at this address.
Rgb harness_pixel(uint32_t address, uint32_t x, uint32_t y);

#endif
