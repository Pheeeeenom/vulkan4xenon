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

// Colour attachments of 16 texels or less on a side sit in the packed mip tail, where a resolve can't write.
static void check_attachments(const Harness *harness)
{
	VkFormatProperties properties;
	vkGetPhysicalDeviceFormatProperties(harness->physical_device, VK_FORMAT_R8G8B8A8_UNORM, &properties);
	const VkFormatFeatureFlags wanted =
		VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT;
	VkImageCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = VK_FORMAT_R8G8B8A8_UNORM,
		.extent = { 16, 64, 1 },
		.mipLevels = 1,
		.arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
	};
	VkImage image;
	const bool narrow = vkCreateImage(harness->device, &info, NULL, &image) == VK_ERROR_FEATURE_NOT_PRESENT;
	info.extent = (VkExtent3D){ 64, 64, 1 };
	info.mipLevels = 2;
	const bool mipped = vkCreateImage(harness->device, &info, NULL, &image) == VK_ERROR_FEATURE_NOT_PRESENT;
	check("objects: R8G8B8A8_UNORM is a blendable colour attachment, but not 16 texels across or with mips",
		(properties.optimalTilingFeatures & wanted) == wanted && narrow && mipped);
	const VkAttachmentDescription attachments[2] = {
		{ .format = VK_FORMAT_B8G8R8A8_UNORM,
			.samples = VK_SAMPLE_COUNT_1_BIT,
			.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
			.storeOp = VK_ATTACHMENT_STORE_OP_STORE },
		{ .format = VK_FORMAT_D24_UNORM_S8_UINT,
			.samples = VK_SAMPLE_COUNT_1_BIT,
			.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
			.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD },
	};
	const VkAttachmentReference color = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
	const VkAttachmentReference depth = { 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
	const VkSubpassDescription subpass = {
		.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
		.colorAttachmentCount = 1,
		.pColorAttachments = &color,
		.pDepthStencilAttachment = &depth,
	};
	const VkRenderPassCreateInfo pass_info = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
		.attachmentCount = 2,
		.pAttachments = attachments,
		.subpassCount = 1,
		.pSubpasses = &subpass,
	};
	VkRenderPass render_pass;
	check("objects: a render pass that clears depth and loads stencil is refused",
		vkCreateRenderPass(harness->device, &pass_info, NULL, &render_pass) == VK_ERROR_FEATURE_NOT_PRESENT);
}

void test_objects(const Harness *harness)
{
	check_proc_addresses(harness);
	check_memory(harness);
	check_refusals(harness);
	check_attachments(harness);
}
