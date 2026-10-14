#ifndef VULKAN4XENON_H
#define VULKAN4XENON_H

#include <stdint.h>

#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

// A shader compiled offline by tools/build_shaders.py: xost's container for one SPIR-V module.

// A float constant xost pooled for a literal, which the layer writes when a pipeline using the shader is bound.
typedef struct {
	uint32_t reg;
	float values[4];
} V4xShaderLiteral;

enum {
	V4X_RESOURCE_PUSH_CONSTANTS,
	V4X_RESOURCE_UNIFORM_BUFFER,
	V4X_RESOURCE_SAMPLER,
};

// Where xost put a block or sampler the shader reads (xost:docs/xsc.md, "Interface conventions").
typedef struct {
	uint8_t kind;
	// The descriptor set and binding; 0 for push constants.
	uint8_t set;
	uint8_t binding;
	// The first float constant register, holding the block's first 16 bytes, or the fetch constant.
	uint16_t first;
	// Registers the block fills; 1 for a sampler.
	uint16_t count;
} V4xShaderResource;

typedef struct {
	// FNV-1a over the SPIR-V's words, each taken as its four bytes low first, and the SPIR-V's size.
	uint64_t spirv_hash;
	uint32_t spirv_size_bytes;
	const uint32_t *container;
	uint32_t container_size_bytes;
	const V4xShaderLiteral *literals;
	uint32_t literal_count;
	const V4xShaderResource *resources;
	uint32_t resource_count;
} V4xShader;

typedef struct {
	const V4xShader *shaders;
	uint32_t count;
} V4xShaderBundle;

// The app's bundle, which tools/build_shaders.py writes; vkCreateShaderModule looks SPIR-V up in it, and finds
// nothing when the app links none.
extern const V4xShaderBundle v4x_shader_bundle;

// Keeps the shaders the layer compiles in this folder and reads them back from it; call before making any modules.
void v4x_set_shader_cache(const char *directory);

// Shaders the layer has compiled, and read back from its cache, since the title started.
typedef struct {
	uint32_t compiled;
	uint32_t read;
} V4xShaderCounts;

V4xShaderCounts v4x_shader_counts(void);

#ifdef __cplusplus
}
#endif

#endif
