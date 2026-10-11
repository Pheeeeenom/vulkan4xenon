#ifndef V4X_SHADER_H
#define V4X_SHADER_H

#include <stdint.h>

#include "vulkan4xenon.h"

// The bundle's shader compiled from this SPIR-V, or NULL.
const V4xShader *v4x_shader_find(const V4xShaderBundle *bundle, const uint32_t *code, uint32_t size_bytes);

#endif
