#ifndef V4X_SHADER_H
#define V4X_SHADER_H

#include <stdint.h>

#include <vulkan/vulkan.h>

#include "vulkan4xenon.h"

// The bundle's shader compiled from this SPIR-V, or NULL.
const V4xShader *v4x_shader_find(const V4xShaderBundle *bundle, const uint32_t *code, uint32_t size_bytes);
// Frees the shaders the device compiled, as it goes.
void v4x_shader_release(VkDevice device);

#endif
