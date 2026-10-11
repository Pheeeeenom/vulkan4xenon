#ifndef V4X_BENCH_VMATH_H
#define V4X_BENCH_VMATH_H

#include <math.h>
#include <string.h>

// Column-major 4x4 matrices, as GLSL reads them, from vulkan4vita:bench/vmath.h.

static inline void mat4_mul(float out[16], const float a[16], const float b[16])
{
	float r[16];
	for (int c = 0; c < 4; c++) {
		for (int row = 0; row < 4; row++)
			r[c * 4 + row] = a[0 * 4 + row] * b[c * 4 + 0] + a[1 * 4 + row] * b[c * 4 + 1] +
				a[2 * 4 + row] * b[c * 4 + 2] + a[3 * 4 + row] * b[c * 4 + 3];
	}
	memcpy(out, r, sizeof r);
}

static inline void mat4_ortho(float m[16], float l, float r, float b, float t, float n, float f)
{
	memset(m, 0, 16 * sizeof(float));
	m[0] = 2.0f / (r - l);
	m[5] = 2.0f / (t - b);
	m[10] = -2.0f / (f - n);
	m[12] = -(r + l) / (r - l);
	m[13] = -(t + b) / (t - b);
	m[14] = -(f + n) / (f - n);
	m[15] = 1.0f;
}

// Ry(yaw) * Rx(pitch) * a uniform scale, no translation.
static inline void mat4_rotation_yx(float m[16], float yaw, float pitch, float scale)
{
	const float sa = sinf(yaw);
	const float ca = cosf(yaw);
	const float sb = sinf(pitch);
	const float cb = cosf(pitch);
	const float rotation[16] = { ca * scale, 0.0f, -sa * scale, 0.0f, sa * sb * scale, cb * scale, ca * sb * scale,
		0.0f, sa * cb * scale, -sb * scale, ca * cb * scale, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f };
	memcpy(m, rotation, sizeof rotation);
}

#endif
