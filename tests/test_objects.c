#include <stdbool.h>
#include <stdint.h>

#include "harness.h"
#include "tests.h"

enum {
	// Odd on purpose, so the requirement's rounding shows.
	BUFFER_BYTES = 100,
	BUFFER_ALIGNMENT = 16,
	ROUNDED_BYTES = 112,
};

static const uint32_t unknown_spirv[] = { 0x07230203, 0x00010000, 0, 1, 0 };

static void check_proc_addresses(const Harness *harness)
{
	check("objects: vkGetInstanceProcAddr finds global functions without an instance",
		vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance") == (PFN_vkVoidFunction)vkCreateInstance);
	check("objects: vkGetInstanceProcAddr finds instance functions only with an instance",
		!vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateDevice") &&
			vkGetInstanceProcAddr(harness->instance, "vkCreateDevice") == (PFN_vkVoidFunction)vkCreateDevice);
	check("objects: vkGetDeviceProcAddr finds device functions and not instance ones",
		vkGetDeviceProcAddr(harness->device, "vkCmdDraw") == (PFN_vkVoidFunction)vkCmdDraw &&
			!vkGetDeviceProcAddr(harness->device, "vkCreateDevice"));
	check("objects: an unknown name finds nothing", !vkGetInstanceProcAddr(harness->instance, "vkCmdDispatch"));
}

static void check_memory(const Harness *harness)
{
	VkPhysicalDeviceMemoryProperties memory;
	vkGetPhysicalDeviceMemoryProperties(harness->physical_device, &memory);
	const VkMemoryPropertyFlags wanted = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
		VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	check("objects: one memory type, device-local, host-visible and coherent",
		memory.memoryTypeCount == 1 && memory.memoryTypes[0].propertyFlags == wanted && memory.memoryHeapCount == 1);
	const VkBufferCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = BUFFER_BYTES,
		.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
	};
	VkBuffer buffer;
	const bool made = harness_ok("vkCreateBuffer", vkCreateBuffer(harness->device, &info, NULL, &buffer));
	VkMemoryRequirements requirements = { 0 };
	if (made)
		vkGetBufferMemoryRequirements(harness->device, buffer, &requirements);
	check("objects: a buffer's requirements round its size up to its alignment",
		made && requirements.size == ROUNDED_BYTES && requirements.alignment == BUFFER_ALIGNMENT &&
			requirements.memoryTypeBits == 1);
	if (made)
		vkDestroyBuffer(harness->device, buffer, NULL);
}

static void check_refusals(const Harness *harness)
{
	const VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1 };
	const VkInstanceCreateInfo info = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
	VkInstance instance;
	check("objects: an instance for Vulkan 1.1 is refused",
		vkCreateInstance(&info, NULL, &instance) == VK_ERROR_INCOMPATIBLE_DRIVER);
	VkShaderModule module;
	check("objects: SPIR-V missing from the bundle is refused",
		harness_shader(harness, unknown_spirv, sizeof unknown_spirv, &module) == VK_ERROR_FEATURE_NOT_PRESENT);
}

void test_objects(const Harness *harness)
{
	check_proc_addresses(harness);
	check_memory(harness);
	check_refusals(harness);
}
