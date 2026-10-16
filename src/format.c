#include "format.h"

#include <math.h>
#include <string.h>

#include <vulkan/vulkan.h>

#include "xsdk/texture.h"

enum {
	SAMPLED = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT,
	FILTERED = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT,
	// Blits run on the CPU (src/transfer.c), for formats whose texels it unpacks.
	BLITTED = FILTERED | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT,
	// Resolves write R8G8B8A8 and B8G8R8A8 alone (src/render_pass.c).
	ATTACHMENT = BLITTED | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT,
	UNORM = V4X_NUMBER_UNORM,
	FLOAT = V4X_NUMBER_FLOAT,
	// A half float's exponent bias against a float's, and the bits of each below the exponent.
	HALF_REBIAS = 127 - 15,
	HALF_MANTISSA_BITS = 10,
	FLOAT_MANTISSA_BITS = 23,
	HALF_INFINITY = 0x7C00,
	FLOAT_INFINITY = 0x7F800000,
	// The smallest float a half can't hold, 65520, and the smallest normal half, 2^-14, as float bits.
	HALF_OVERFLOW = 0x477FF000,
	HALF_SMALLEST_NORMAL = 0x38800000,
};

// The xsdk channels each of a format's red, green, blue and alpha samples (xsdk:include/xsdk/texture.h).
enum {
	RED = XSDK_CHANNEL_RED,
	GREEN = XSDK_CHANNEL_GREEN,
	BLUE = XSDK_CHANNEL_BLUE,
	ALPHA = XSDK_CHANNEL_ALPHA,
	ONE = XSDK_CHANNEL_ONE,
};

// Each Vulkan format on the xsdk format that holds its bits as Vulkan does: units in the CPU's order, as xsdk's uploads
// take them, red to alpha where xsdk's own channels put them (xsdk:docs/HARDWARE.md, "Texture formats, milestone 16").
// Where Vulkan orders the channels otherwise, the fetch constant's swizzle picks them, which reads the same bits.
static const V4xFormat formats[] = {
	{ VK_FORMAT_R8G8B8A8_UNORM, XSDK_FORMAT_R8G8B8A8, { RED, GREEN, BLUE, ALPHA }, 4, false, { 8, 8, 8, 8 },
		{ 24, 16, 8, 0 }, UNORM, ATTACHMENT },
	{ VK_FORMAT_B8G8R8A8_UNORM, XSDK_FORMAT_B8G8R8A8, { RED, GREEN, BLUE, ALPHA }, 4, false, { 8, 8, 8, 8 },
		{ 8, 16, 24, 0 }, UNORM, ATTACHMENT },
	// Filtering 32-bit floats hasn't been tried on the console.
	{ VK_FORMAT_R32_SFLOAT, XSDK_FORMAT_R32F, { RED, GREEN, BLUE, ALPHA }, 4, false, { 32, 0, 0, 0 }, { 0, 0, 0, 0 },
		FLOAT, SAMPLED },
	{ VK_FORMAT_R8_UNORM, XSDK_FORMAT_R8, { RED, GREEN, BLUE, ALPHA }, 1, false, { 8, 0, 0, 0 }, { 0, 0, 0, 0 }, UNORM,
		BLITTED },
	{ VK_FORMAT_R8G8_UNORM, XSDK_FORMAT_R8G8, { RED, GREEN, BLUE, ALPHA }, 2, false, { 8, 8, 0, 0 }, { 8, 0, 0, 0 },
		UNORM, BLITTED },
	// BC1 without alpha is DXT1 with its alpha read as 1, as BC1's colours are the same either way.
	{ VK_FORMAT_BC1_RGB_UNORM_BLOCK, XSDK_FORMAT_DXT1, { RED, GREEN, BLUE, ONE }, 8, true, { 0 }, { 0 }, UNORM,
		FILTERED },
	{ VK_FORMAT_BC1_RGBA_UNORM_BLOCK, XSDK_FORMAT_DXT1, { RED, GREEN, BLUE, ALPHA }, 8, true, { 0 }, { 0 }, UNORM,
		FILTERED },
	{ VK_FORMAT_BC2_UNORM_BLOCK, XSDK_FORMAT_DXT3, { RED, GREEN, BLUE, ALPHA }, 16, true, { 0 }, { 0 }, UNORM,
		FILTERED },
	{ VK_FORMAT_BC3_UNORM_BLOCK, XSDK_FORMAT_DXT5, { RED, GREEN, BLUE, ALPHA }, 16, true, { 0 }, { 0 }, UNORM,
		FILTERED },
	{ VK_FORMAT_R5G6B5_UNORM_PACK16, XSDK_FORMAT_R5G6B5, { RED, GREEN, BLUE, ALPHA }, 2, false, { 5, 6, 5, 0 },
		{ 11, 5, 0, 0 }, UNORM, BLITTED },
	{ VK_FORMAT_B5G6R5_UNORM_PACK16, XSDK_FORMAT_R5G6B5, { BLUE, GREEN, RED, ALPHA }, 2, false, { 5, 6, 5, 0 },
		{ 0, 5, 11, 0 }, UNORM, BLITTED },
	{ VK_FORMAT_A1R5G5B5_UNORM_PACK16, XSDK_FORMAT_A1R5G5B5, { RED, GREEN, BLUE, ALPHA }, 2, false, { 5, 5, 5, 1 },
		{ 10, 5, 0, 15 }, UNORM, BLITTED },
	{ VK_FORMAT_R4G4B4A4_UNORM_PACK16, XSDK_FORMAT_A4R4G4B4, { ALPHA, RED, GREEN, BLUE }, 2, false, { 4, 4, 4, 4 },
		{ 12, 8, 4, 0 }, UNORM, BLITTED },
	{ VK_FORMAT_B4G4R4A4_UNORM_PACK16, XSDK_FORMAT_A4R4G4B4, { GREEN, RED, ALPHA, BLUE }, 2, false, { 4, 4, 4, 4 },
		{ 4, 8, 12, 0 }, UNORM, BLITTED },
	{ VK_FORMAT_A2B10G10R10_UNORM_PACK32, XSDK_FORMAT_A2B10G10R10, { RED, GREEN, BLUE, ALPHA }, 4, false,
		{ 10, 10, 10, 2 }, { 0, 10, 20, 30 }, UNORM, BLITTED },
	{ VK_FORMAT_A2R10G10B10_UNORM_PACK32, XSDK_FORMAT_A2B10G10R10, { BLUE, GREEN, RED, ALPHA }, 4, false,
		{ 10, 10, 10, 2 }, { 20, 10, 0, 30 }, UNORM, BLITTED },
	{ VK_FORMAT_R16_SFLOAT, XSDK_FORMAT_R16F, { RED, GREEN, BLUE, ALPHA }, 2, false, { 16, 0, 0, 0 }, { 0, 0, 0, 0 },
		FLOAT, BLITTED },
	{ VK_FORMAT_R16G16_SFLOAT, XSDK_FORMAT_R16G16F, { RED, GREEN, BLUE, ALPHA }, 4, false, { 16, 16, 0, 0 },
		{ 16, 0, 0, 0 }, FLOAT, BLITTED },
	{ VK_FORMAT_R16G16B16A16_SFLOAT, XSDK_FORMAT_R16G16B16A16F, { RED, GREEN, BLUE, ALPHA }, 8, false,
		{ 16, 16, 16, 16 }, { 48, 32, 16, 0 }, FLOAT, BLITTED },
};

const V4xFormat *v4x_format(VkFormat format)
{
	for (uint32_t i = 0; i < sizeof formats / sizeof formats[0]; i++) {
		if (formats[i].format == format)
			return &formats[i];
	}
	return NULL;
}

// A unit as its 32-bit word, or an 8-byte one as two, the high one first; no channel straddles the two.
static uint32_t word_of(const V4xFormat *format, uint32_t shift)
{
	return format->texel_bytes > 4 && shift < 32;
}

static void read_unit(const V4xFormat *format, const uint8_t *texel, uint32_t words[2])
{
	words[0] = 0;
	words[1] = 0;
	for (uint32_t i = 0; i < format->texel_bytes; i++)
		words[i / 4] = words[i / 4] << 8 | texel[i];
}

static void write_unit(const V4xFormat *format, const uint32_t words[2], uint8_t *texel)
{
	const uint32_t word_bytes = format->texel_bytes > 4 ? 4 : format->texel_bytes;
	for (uint32_t i = 0; i < format->texel_bytes; i++)
		texel[i] = (uint8_t)(words[i / 4] >> 8 * (word_bytes - 1 - i % 4));
}

static float float_of(uint32_t bits)
{
	float value;
	memcpy(&value, &bits, sizeof value);
	return value;
}

static uint32_t bits_of(float value)
{
	uint32_t bits;
	memcpy(&bits, &value, sizeof bits);
	return bits;
}

static float half_to_float(uint32_t half)
{
	const uint32_t sign = (half & 0x8000) << 16;
	const uint32_t exponent = half >> HALF_MANTISSA_BITS & 0x1F;
	const uint32_t mantissa = half & 0x3FF;
	if (!exponent) {
		const float magnitude = (float)mantissa / (float)(1 << 24);
		return sign ? -magnitude : magnitude;
	}
	if (exponent == 0x1F)
		return float_of(sign | FLOAT_INFINITY | mantissa << (FLOAT_MANTISSA_BITS - HALF_MANTISSA_BITS));
	return float_of(sign | (exponent + HALF_REBIAS) << FLOAT_MANTISSA_BITS |
		mantissa << (FLOAT_MANTISSA_BITS - HALF_MANTISSA_BITS));
}

// Rounded to the nearest half, ties to even; past the largest is infinity.
static uint32_t float_to_half(float value)
{
	const uint32_t bits = bits_of(value);
	const uint32_t sign = bits >> 16 & 0x8000;
	const uint32_t magnitude = bits & 0x7FFFFFFF;
	if (magnitude > FLOAT_INFINITY)
		return sign | HALF_INFINITY | 0x200;
	if (magnitude >= HALF_OVERFLOW)
		return sign | HALF_INFINITY;
	if (magnitude < HALF_SMALLEST_NORMAL)
		return sign | (uint32_t)lrintf(float_of(magnitude) * (float)(1 << 24));
	const uint32_t rebiased = magnitude - ((uint32_t)HALF_REBIAS << FLOAT_MANTISSA_BITS);
	const uint32_t dropped = FLOAT_MANTISSA_BITS - HALF_MANTISSA_BITS;
	return sign | (rebiased + (1u << (dropped - 1)) - 1 + (rebiased >> dropped & 1)) >> dropped;
}

static float channel_value(uint32_t number, uint32_t bits, uint32_t field)
{
	if (number == V4X_NUMBER_FLOAT)
		return bits == 16 ? half_to_float(field) : float_of(field);
	return (float)field / (float)((1u << bits) - 1);
}

static uint32_t channel_field(uint32_t number, uint32_t bits, float value)
{
	if (number == V4X_NUMBER_FLOAT)
		return bits == 16 ? float_to_half(value) : bits_of(value);
	const float clamped = !(value > 0.0f) ? 0.0f : value > 1.0f ? 1.0f : value;
	return (uint32_t)lrintf(clamped * (float)((1u << bits) - 1));
}

void v4x_format_unpack(const V4xFormat *format, const uint8_t *texel, float rgba[4])
{
	uint32_t words[2];
	read_unit(format, texel, words);
	for (uint32_t c = 0; c < 4; c++) {
		const uint32_t bits = format->bits[c];
		const uint32_t shift = format->shifts[c] % 32;
		const uint32_t mask = bits == 32 ? 0xFFFFFFFF : (1u << bits) - 1;
		const uint32_t field = words[word_of(format, format->shifts[c])] >> shift & mask;
		rgba[c] = bits ? channel_value(format->number, bits, field) : c == 3 ? 1.0f : 0.0f;
	}
}

void v4x_format_pack(const V4xFormat *format, const float rgba[4], uint8_t *texel)
{
	uint32_t words[2] = { 0, 0 };
	for (uint32_t c = 0; c < 4; c++) {
		if (format->bits[c])
			words[word_of(format, format->shifts[c])] |= channel_field(format->number, format->bits[c], rgba[c])
				<< format->shifts[c] % 32;
	}
	write_unit(format, words, texel);
}
