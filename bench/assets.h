#ifndef V4X_BENCH_ASSETS_H
#define V4X_BENCH_ASSETS_H

#include <stdint.h>

#include "bench.h"

// vkbench's meshes and textures (vulkan4vita:bench/assets.c).

enum {
	TEXTURE_BYTES = TEXTURE_SIZE * TEXTURE_SIZE * 4,
	FULLSCREEN_VERTICES = 4,
	QUAD_INDICES = 6,
};

void assets_build_cube(MeshVertex vertices[CUBE_VERTEX_COUNT], uint16_t indices[CUBE_INDEX_COUNT]);
// Each quad's 4 vertices go bottom left, bottom right, top left, top right.
void assets_build_quad_indices(uint16_t *indices, int quad_count);
// A checkerboard in texture index's two colours, as bytes red, green, blue then alpha.
void assets_build_texture(uint8_t pixels[TEXTURE_BYTES], int index);
// A quad over clip space, white at an alpha of 32.
void assets_build_fullscreen_quad(SpriteVertex vertices[FULLSCREEN_VERTICES]);

#endif
