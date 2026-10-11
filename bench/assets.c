#include "assets.h"

#include <stdint.h>

#include "bench.h"

void assets_build_cube(MeshVertex vertices[CUBE_VERTEX_COUNT], uint16_t indices[CUBE_INDEX_COUNT])
{
	// Each face's normal, then its u and v axes.
	static const float faces[6][3][3] = {
		{ { 1, 0, 0 }, { 0, 0, -1 }, { 0, 1, 0 } },
		{ { -1, 0, 0 }, { 0, 0, 1 }, { 0, 1, 0 } },
		{ { 0, 1, 0 }, { 1, 0, 0 }, { 0, 0, -1 } },
		{ { 0, -1, 0 }, { 1, 0, 0 }, { 0, 0, 1 } },
		{ { 0, 0, 1 }, { 1, 0, 0 }, { 0, 1, 0 } },
		{ { 0, 0, -1 }, { -1, 0, 0 }, { 0, 1, 0 } },
	};
	for (int f = 0; f < 6; f++) {
		const float *const n = faces[f][0];
		const float *const u = faces[f][1];
		const float *const v = faces[f][2];
		for (int c = 0; c < 4; c++) {
			const float su = (c & 1) ? 1.0f : -1.0f;
			const float sv = (c & 2) ? 1.0f : -1.0f;
			vertices[f * 4 + c] =
				(MeshVertex){ 0.5f * (n[0] + su * u[0] + sv * v[0]), 0.5f * (n[1] + su * u[1] + sv * v[1]),
					0.5f * (n[2] + su * u[2] + sv * v[2]), (c & 1) ? 1.0f : 0.0f, (c & 2) ? 0.0f : 1.0f };
		}
	}
	assets_build_quad_indices(indices, 6);
}

void assets_build_quad_indices(uint16_t *indices, int quad_count)
{
	for (int q = 0; q < quad_count; q++) {
		const uint16_t base = (uint16_t)(q * 4);
		uint16_t *const out = &indices[q * QUAD_INDICES];
		out[0] = base;
		out[1] = base + 1;
		out[2] = base + 2;
		out[3] = base + 2;
		out[4] = base + 1;
		out[5] = base + 3;
	}
}

void assets_build_texture(uint8_t pixels[TEXTURE_BYTES], int index)
{
	static const uint8_t palette[TEXTURE_COUNT][2][3] = {
		{ { 224, 80, 48 }, { 240, 240, 240 } },
		{ { 48, 160, 224 }, { 24, 32, 48 } },
		{ { 96, 200, 96 }, { 240, 220, 120 } },
		{ { 200, 96, 200 }, { 64, 64, 64 } },
	};
	for (int y = 0; y < TEXTURE_SIZE; y++) {
		for (int x = 0; x < TEXTURE_SIZE; x++) {
			const uint8_t *const c = palette[index][((x / 8) ^ (y / 8)) & 1];
			uint8_t *const out = &pixels[(y * TEXTURE_SIZE + x) * 4];
			out[0] = c[0];
			out[1] = c[1];
			out[2] = c[2];
			out[3] = 255;
		}
	}
}

void assets_build_fullscreen_quad(SpriteVertex vertices[FULLSCREEN_VERTICES])
{
	for (int c = 0; c < FULLSCREEN_VERTICES; c++)
		vertices[c] = (SpriteVertex){ (c & 1) ? 1.0f : -1.0f, (c & 2) ? 1.0f : -1.0f, (c & 1) ? 1.0f : 0.0f,
			(c & 2) ? 0.0f : 1.0f, { 1.0f, 1.0f, 1.0f, 32.0f / 255.0f } };
}
