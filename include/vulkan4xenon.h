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

typedef struct {
	// FNV-1a over the SPIR-V's words, each taken as its four bytes low first, and the SPIR-V's size.
	uint64_t spirv_hash;
	uint32_t spirv_size_bytes;
	const uint32_t *container;
	uint32_t container_size_bytes;
	const V4xShaderLiteral *literals;
	uint32_t literal_count;
} V4xShader;

typedef struct {
	const V4xShader *shaders;
	uint32_t count;
} V4xShaderBundle;

// The app's bundle, which tools/build_shaders.py writes; vkCreateShaderModule looks SPIR-V up in it, and finds
// nothing when the app links none.
extern const V4xShaderBundle v4x_shader_bundle;

#ifdef __cplusplus
}
#endif

#endif
