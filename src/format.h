#ifndef V4X_FORMAT_H
#define V4X_FORMAT_H

#include <stdbool.h>
#include <stdint.h>

#include <vulkan/vulkan.h>

#include "xsdk/shader.h"

// The image and vertex formats the layer takes: what each is in xsdk, and how an image's texel holds its channels for
// the CPU's clears and blits (docs/SUPPORT.md, Formats).

enum {
	// The most bytes an uncompressed texel takes, R16G16B16A16_SFLOAT's.
	V4X_MAX_TEXEL_BYTES = 8,
	// Texels across and down a block of a compressed format.
	V4X_BLOCK_SIDE = 4,
};

// How a channel's bits read: fixed point from 0 to 1, or a float of 16 or 32 bits.
enum {
	V4X_NUMBER_UNORM,
	V4X_NUMBER_FLOAT,
};

typedef struct {
	VkFormat format;
	uint32_t xsdk_format;
	// The XSDK_CHANNEL_* the format's red, green, blue and alpha are, as an image view samples them.
	uint8_t channels[4];
	// Bytes in a texel, or in a block of 4x4 texels if compressed.
	uint8_t texel_bytes;
	bool compressed;
	// A texel is one unit of texel_bytes in the CPU's byte order, as Vulkan has a packed format on a big-endian host;
	// each of red, green, blue and alpha takes its bits from its shift up, 0 bits for one it lacks.
	uint8_t bits[4];
	uint8_t shifts[4];
	// A V4X_NUMBER_*.
	uint8_t number;
	// Its optimal tiling features.
	VkFormatFeatureFlags features;
} V4xFormat;

// The format's description, or NULL if the layer takes no colour images of it.
const V4xFormat *v4x_format(VkFormat format);
// A texel's red, green, blue and alpha from its bytes as an image holds them; one the format lacks is 0, alpha 1.
void v4x_format_unpack(const V4xFormat *format, const uint8_t *texel, float rgba[4]);
// The texel's bytes for the channels, each clamped to what the format holds and rounded to the nearest.
void v4x_format_pack(const V4xFormat *format, const float rgba[4], uint8_t *texel);
// Sets the element's xsdk format and the components it picks for a vertex attribute of this format; false if the
// layer takes none.
bool v4x_format_vertex(VkFormat format, XsdkVertexElement *element);

#endif
