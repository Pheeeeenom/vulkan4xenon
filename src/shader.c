#include "shader.h"

#include <pthread.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>

#include <vulkan/vulkan.h>

#include "host.h"
#include "objects.h"
#include "vulkan4xenon.h"
#include "xsdk/log.h"
#include "xsdk/xsc.h"

enum {
	FNV_OFFSET_BASIS_HIGH = 0xCBF29CE4,
	FNV_OFFSET_BASIS_LOW = 0x84222325,
	FNV_PRIME_HIGH = 0x00000100,
	FNV_PRIME_LOW = 0x000001B3,
	// The widths of V4xShaderResource's fields.
	MAX_RESOURCE_INDEX = 0xFF,
	MAX_RESOURCE_REGISTER = 0xFFFF,
	NANOSECONDS_PER_MICROSECOND = 1000,
	MICROSECONDS_PER_SECOND = 1000000,
};

// Weak, so an app that links no bundle still links: the address is then NULL. A weak definition instead gets folded.
extern const V4xShaderBundle v4x_shader_bundle __attribute__((weak));

// 64-bit FNV-1a over each word's bytes, low first, so the tool and the console agree whatever their byte order.
static uint64_t spirv_hash(const uint32_t *code, uint32_t size_bytes)
{
	const uint64_t prime = (uint64_t)FNV_PRIME_HIGH << 32 | FNV_PRIME_LOW;
	uint64_t hash = (uint64_t)FNV_OFFSET_BASIS_HIGH << 32 | FNV_OFFSET_BASIS_LOW;
	for (uint32_t i = 0; i < size_bytes / 4; i++) {
		for (uint32_t shift = 0; shift < 32; shift += 8) {
			hash ^= code[i] >> shift & 0xFF;
			hash *= prime;
		}
	}
	return hash;
}

static const V4xShader *in_bundle(const V4xShaderBundle *bundle, uint64_t hash, uint32_t size_bytes)
{
	for (uint32_t i = 0; bundle && i < bundle->count; i++) {
		const V4xShader *const shader = &bundle->shaders[i];
		if (shader->spirv_hash == hash && shader->spirv_size_bytes == size_bytes)
			return shader;
	}
	return NULL;
}

const V4xShader *v4x_shader_find(const V4xShaderBundle *bundle, const uint32_t *code, uint32_t size_bytes)
{
	const uint64_t hash = spirv_hash(code, size_bytes);
	const V4xShader *const found = in_bundle(bundle, hash, size_bytes);
	if (!found)
		xsdk_log_write("vulkan4xenon: no compiled shader for SPIR-V %08X%08X, %u bytes", (unsigned)(hash >> 32),
			(unsigned)hash, (unsigned)size_bytes);
	return found;
}

static uint64_t now_us(void)
{
	struct timespec now;
	(void)clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * MICROSECONDS_PER_SECOND + (uint64_t)now.tv_nsec / NANOSECONDS_PER_MICROSECOND;
}

static uint8_t resource_kind(XsdkXscResourceKind kind)
{
	if (kind == XSDK_XSC_PUSH_CONSTANTS)
		return V4X_RESOURCE_PUSH_CONSTANTS;
	return kind == XSDK_XSC_UNIFORM_BUFFER ? V4X_RESOURCE_UNIFORM_BUFFER : V4X_RESOURCE_SAMPLER;
}

// What the layer can't hand the GPU: a hardware loop's count or array, as tools/build_shaders.py refuses too, and
// resources past V4xShaderResource's fields.
static bool layer_takes(const XsdkXscShader *compiled)
{
	for (size_t i = 0; i < compiled->need_count; i++) {
		if (compiled->needs[i].kind != XSDK_XSC_LITERAL)
			return false;
	}
	for (size_t i = 0; i < compiled->resource_count; i++) {
		const XsdkXscResource *const r = &compiled->resources[i];
		if (r->set > MAX_RESOURCE_INDEX || r->binding > MAX_RESOURCE_INDEX || r->reg > MAX_RESOURCE_REGISTER ||
			r->regs > MAX_RESOURCE_REGISTER)
			return false;
	}
	return true;
}

// A compiled shader in one allocation: the V4xShader, then its container, literals and resources.
static V4xShader *shader_from(
	const VkAllocationCallbacks *allocator, const XsdkXscShader *compiled, uint64_t hash, uint32_t size_bytes)
{
	const size_t container_words = (compiled->container_bytes + 3) / 4;
	const size_t bytes = sizeof(V4xShader) + container_words * sizeof(uint32_t) +
		compiled->need_count * sizeof(V4xShaderLiteral) + compiled->resource_count * sizeof(V4xShaderResource);
	V4xShader *const shader = v4x_host_alloc(allocator, bytes, VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
	if (!shader)
		return NULL;
	uint32_t *const container = (uint32_t *)(shader + 1);
	V4xShaderLiteral *const literals = (V4xShaderLiteral *)(container + container_words);
	V4xShaderResource *const resources = (V4xShaderResource *)(literals + compiled->need_count);
	memcpy(container, compiled->container, compiled->container_bytes);
	for (size_t i = 0; i < compiled->need_count; i++) {
		const XsdkXscNeed *const need = &compiled->needs[i];
		literals[i] = (V4xShaderLiteral){ need->reg,
			{ (float)need->value[0], (float)need->value[1], (float)need->value[2], (float)need->value[3] } };
	}
	for (size_t i = 0; i < compiled->resource_count; i++) {
		const XsdkXscResource *const r = &compiled->resources[i];
		resources[i] = (V4xShaderResource){ resource_kind(r->kind), (uint8_t)r->set, (uint8_t)r->binding,
			(uint16_t)r->reg, (uint16_t)r->regs };
	}
	*shader = (V4xShader){ .spirv_hash = hash,
		.spirv_size_bytes = size_bytes,
		.container = container,
		.container_size_bytes = (uint32_t)compiled->container_bytes,
		.literals = literals,
		.literal_count = (uint32_t)compiled->need_count,
		.resources = resources,
		.resource_count = (uint32_t)compiled->resource_count };
	return shader;
}

// SPIR-V the bundle doesn't hold, through xost's compiler at xsdk's split with the optimiser on, as Dusklight's pack
// is built (xsdk:docs/scope/shader-compiling.md, step 3).
static V4xShader *compile(VkDevice device, const uint32_t *code, uint32_t size_bytes, uint64_t hash)
{
	XsdkXscOptions options;
	xsdk_xsc_default_options(&options);
	options.optimize = true;
	XsdkXscShader compiled;
	const uint64_t start = now_us();
	const XsdkXscStatus status = xsdk_xsc_compile(code, size_bytes / sizeof(uint32_t), &options, &compiled);
	const uint64_t us = now_us() - start;
	const unsigned high = (unsigned)(hash >> 32);
	const unsigned low = (unsigned)hash;
	if (status != XSDK_XSC_OK) {
		xsdk_log_write("vulkan4xenon: xost refused SPIR-V %08X%08X, %u bytes: %s", high, low, (unsigned)size_bytes,
			compiled.message);
		return NULL;
	}
	V4xShader *shader = NULL;
	if (!layer_takes(&compiled))
		xsdk_log_write("vulkan4xenon: SPIR-V %08X%08X wants a hardware loop's constants or a resource past %u", high,
			low, (unsigned)MAX_RESOURCE_REGISTER);
	else
		shader = shader_from(v4x_host_device_allocator(device, NULL), &compiled, hash, size_bytes);
	xsdk_xsc_free(&compiled);
	if (shader)
		xsdk_log_write("vulkan4xenon: compiled SPIR-V %08X%08X, %u bytes, in %u us", high, low, (unsigned)size_bytes,
			(unsigned)us);
	return shader;
}

// Under device->shaders_lock.
static const V4xShader *kept(VkDevice device, uint64_t hash, uint32_t size_bytes)
{
	for (uint32_t i = 0; i < device->shader_count; i++) {
		const V4xShader *const shader = device->shaders[i];
		if (shader->spirv_hash == hash && shader->spirv_size_bytes == size_bytes)
			return shader;
	}
	return NULL;
}

// Under device->shaders_lock.
static bool keep(VkDevice device, V4xShader *shader)
{
	if (device->shader_count == device->shader_capacity) {
		const uint32_t capacity = device->shader_capacity ? 2 * device->shader_capacity : 16;
		V4xShader **const grown = v4x_host_realloc(v4x_host_device_allocator(device, NULL), device->shaders,
			capacity * sizeof *grown, VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
		if (!grown)
			return false;
		device->shaders = grown;
		device->shader_capacity = capacity;
	}
	device->shaders[device->shader_count++] = shader;
	return true;
}

// The shader compiled from this SPIR-V before, else compiled now and kept.
static const V4xShader *compiled_shader(VkDevice device, const uint32_t *code, uint32_t size_bytes, uint64_t hash)
{
	(void)pthread_mutex_lock(&device->shaders_lock);
	const V4xShader *found = kept(device, hash, size_bytes);
	if (!found) {
		V4xShader *const shader = compile(device, code, size_bytes, hash);
		if (shader && !keep(device, shader))
			v4x_host_free(v4x_host_device_allocator(device, NULL), shader);
		else
			found = shader;
	}
	(void)pthread_mutex_unlock(&device->shaders_lock);
	return found;
}

void v4x_shader_release(VkDevice device)
{
	const VkAllocationCallbacks *const allocator = v4x_host_device_allocator(device, NULL);
	for (uint32_t i = 0; i < device->shader_count; i++)
		v4x_host_free(allocator, device->shaders[i]);
	v4x_host_free(allocator, device->shaders);
	device->shaders = NULL;
	device->shader_count = device->shader_capacity = 0;
	(void)pthread_mutex_destroy(&device->shaders_lock);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateShaderModule(VkDevice device, const VkShaderModuleCreateInfo *pCreateInfo,
	const VkAllocationCallbacks *pAllocator, VkShaderModule *pShaderModule)
{
	const uint32_t size_bytes = (uint32_t)pCreateInfo->codeSize;
	const uint64_t hash = spirv_hash(pCreateInfo->pCode, size_bytes);
	const V4xShader *found = in_bundle(&v4x_shader_bundle, hash, size_bytes);
	if (!found)
		found = compiled_shader(device, pCreateInfo->pCode, size_bytes, hash);
	if (!found)
		return VK_ERROR_FEATURE_NOT_PRESENT;
	ShaderModule *const module = v4x_host_alloc(
		v4x_host_device_allocator(device, pAllocator), sizeof *module, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
	if (!module)
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	module->shader = found;
	*pShaderModule = v4x_handle(module);
	return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyShaderModule(
	VkDevice device, VkShaderModule shaderModule, const VkAllocationCallbacks *pAllocator)
{
	v4x_host_free(v4x_host_device_allocator(device, pAllocator), v4x_object(shaderModule));
}
