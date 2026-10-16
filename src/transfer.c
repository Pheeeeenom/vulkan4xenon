#include "transfer.h"

#include <math.h>
#include <string.h>

#include <vulkan/vulkan.h>

#include "image.h"
#include "objects.h"
#include "xsdk/gpu.h"
#include "xsdk/log.h"
#include "xsdk/texture.h"

enum {
	IDLE_TIMEOUT_MS = 10000,
	// A row of the widest image, at 4 bytes a texel.
	ROW_BYTES = XSDK_TEXTURE_MAX_SIZE * 4,
	// The source rows a blit filters between, and the row it writes.
	ROW_ABOVE = 0,
	ROW_BELOW = 1,
	ROW_OUT = 2,
	ROWS = 3,
	// D24's largest value, depth 1.
	DEPTH_MAX = 0xFFFFFF,
};

// Rows of texels on their way between images; the queue is externally synchronized, so one set will do.
static uint8_t rows[ROWS][ROW_BYTES] __attribute__((aligned(16)));

static uint32_t level_size(uint32_t size, uint32_t level)
{
	return size >> level ? size >> level : 1;
}

// The CPU reads or writes the image, so the GPU must be done with what used it: nothing to wait for if no submission
// has; the fence the last one that did ended with; a fence now if this one did (docs/ROADMAP.md, milestones 2 and 12).
static void wait_for_image(VkQueue queue, const Image *image)
{
	if (!image->used_serial)
		return;
	uint32_t fence;
	if (image->used_serial == queue->serial)
		fence = xsdk_gpu_fence();
	else if (queue->serial - image->used_serial < V4X_SUBMIT_FENCES)
		fence = queue->submit_fences[image->used_serial % V4X_SUBMIT_FENCES];
	else
		// Older than the fences kept: the last submission's covers it.
		fence = queue->submit_fences[(queue->serial - 1) % V4X_SUBMIT_FENCES];
	if (xsdk_gpu_fence_done(fence))
		return;
	const NTSTATUS status = xsdk_gpu_wait(fence, IDLE_TIMEOUT_MS);
	if (!NT_SUCCESS(status))
		xsdk_log_write("vulkan4xenon: the GPU didn't finish with an image for a copy: 0x%08X", (unsigned)status);
}

// The CPU writes the buffer, so the GPU must be done with the draws before, which may read it.
// shortcut: waits for every draw; keep each memory's last use, as images keep theirs, if a port's copies stall on it.
static void wait_for_draws(VkQueue queue)
{
	if (!queue->drawn)
		return;
	const NTSTATUS status = xsdk_gpu_wait(xsdk_gpu_fence(), IDLE_TIMEOUT_MS);
	if (!NT_SUCCESS(status))
		xsdk_log_write(
			"vulkan4xenon: the GPU didn't finish its draws for a copy into a buffer: 0x%08X", (unsigned)status);
	queue->drawn = false;
}

// The GPU's caches may hold what the CPU replaced; cached memory, which the GPU never reads, is at address 0.
static void buffer_written(uint32_t address, uint32_t size_bytes)
{
	if (address)
		xsdk_gpu_invalidate(address, size_bytes);
}

// The same for an image, which eDRAM no longer holds as it was.
static void image_written(VkQueue queue, const Image *image)
{
	xsdk_gpu_invalidate(image->texture.address, image->texture.size_bytes);
	if (queue->color.image == image)
		queue->color = (EdramPart){ .contents = EDRAM_UNKNOWN };
}

void v4x_transfer_buffer_to_image(VkQueue queue, const Command *command)
{
	Image *const image = command->copy.image;
	wait_for_image(queue, image);
	xsdk_texture_upload(
		&image->texture, command->copy.level, (const void *)(uintptr_t)command->copy.buffer, command->copy.row_bytes);
	image_written(queue, image);
}

void v4x_transfer_image_to_buffer(VkQueue queue, const Command *command)
{
	const Image *const image = command->copy.image;
	const uint32_t height = level_size(image->height, command->copy.level);
	wait_for_image(queue, image);
	wait_for_draws(queue);
	xsdk_texture_download_rows(&image->texture, command->copy.level, 0, height, (void *)(uintptr_t)command->copy.buffer,
		command->copy.row_bytes);
	buffer_written(command->copy.address, height * command->copy.row_bytes);
}

// A row at a time, as the two levels' layouts may differ.
void v4x_transfer_copy_image(VkQueue queue, const Command *command)
{
	const Image *const source = command->image_copy.source;
	Image *const destination = command->image_copy.destination;
	wait_for_image(queue, source);
	wait_for_image(queue, destination);
	const uint32_t height = level_size(source->height, command->image_copy.source_level);
	for (uint32_t y = 0; y < height; y++) {
		xsdk_texture_download_rows(&source->texture, command->image_copy.source_level, y, 1, rows[ROW_OUT], 0);
		xsdk_texture_upload_rows(&destination->texture, command->image_copy.destination_level, y, 1, rows[ROW_OUT], 0);
	}
	image_written(queue, destination);
}

// Where a destination texel's centre falls among the source's, as a blit samples: the nearest, or the two either side
// and how far towards the second, edges clamped.
typedef struct {
	uint32_t first;
	uint32_t second;
	float towards_second;
} Taps;

static Taps taps(uint32_t index, uint32_t to, uint32_t from, bool linear)
{
	const float centre = ((float)index + 0.5f) * (float)from / (float)to;
	if (!linear) {
		const uint32_t nearest = (uint32_t)centre < from ? (uint32_t)centre : from - 1;
		return (Taps){ nearest, nearest, 0.0f };
	}
	const int32_t below = (int32_t)floorf(centre - 0.5f);
	const uint32_t first = below < 0 ? 0 : (uint32_t)below;
	const uint32_t second = below + 1 >= (int32_t)from ? from - 1 : (uint32_t)(below + 1);
	return (Taps){ first, second, centre - 0.5f - (float)below };
}

static float mix(float a, float b, float towards_b)
{
	return a + (b - a) * towards_b;
}

typedef struct {
	uint32_t from_width;
	uint32_t to_width;
	// Bytes a texel, a channel each.
	uint32_t channels;
	bool linear;
} BlitRow;

// A destination row from the source rows either side of it.
static void blit_row(const BlitRow *blit, float towards_below)
{
	for (uint32_t x = 0; x < blit->to_width; x++) {
		const Taps across = taps(x, blit->to_width, blit->from_width, blit->linear);
		for (uint32_t c = 0; c < blit->channels; c++) {
			const uint32_t first = across.first * blit->channels + c;
			const uint32_t second = across.second * blit->channels + c;
			const float above = mix(rows[ROW_ABOVE][first], rows[ROW_ABOVE][second], across.towards_second);
			const float below = mix(rows[ROW_BELOW][first], rows[ROW_BELOW][second], across.towards_second);
			rows[ROW_OUT][x * blit->channels + c] = (uint8_t)(mix(above, below, towards_below) + 0.5f);
		}
	}
}

// Images of 8-bit channels alone (vkCmdBlitImage), a destination row at a time.
void v4x_transfer_blit(VkQueue queue, const Command *command)
{
	const Image *const source = command->image_copy.source;
	Image *const destination = command->image_copy.destination;
	const uint32_t from_level = command->image_copy.source_level;
	const uint32_t to_level = command->image_copy.destination_level;
	const BlitRow blit = { .from_width = level_size(source->width, from_level),
		.to_width = level_size(destination->width, to_level),
		.channels = v4x_texel_bytes(source->format),
		.linear = command->image_copy.linear };
	const uint32_t from_height = level_size(source->height, from_level);
	const uint32_t to_height = level_size(destination->height, to_level);
	wait_for_image(queue, source);
	wait_for_image(queue, destination);
	for (uint32_t y = 0; y < to_height; y++) {
		const Taps down = taps(y, to_height, from_height, blit.linear);
		xsdk_texture_download_rows(&source->texture, from_level, down.first, 1, rows[ROW_ABOVE], 0);
		xsdk_texture_download_rows(&source->texture, from_level, down.second, 1, rows[ROW_BELOW], 0);
		blit_row(&blit, down.towards_second);
		xsdk_texture_upload_rows(&destination->texture, to_level, y, 1, rows[ROW_OUT], 0);
	}
	image_written(queue, destination);
}

// Each level takes the one row of the texel over and over.
void v4x_transfer_clear(VkQueue queue, const Command *command)
{
	Image *const image = command->clear.image;
	const uint32_t texel_bytes = v4x_texel_bytes(image->format);
	const uint32_t width = level_size(image->width, command->clear.first_level);
	for (uint32_t i = 0; i < width * texel_bytes; i += texel_bytes)
		memcpy(&rows[ROW_OUT][i], &command->clear.texel, texel_bytes);
	wait_for_image(queue, image);
	const uint32_t end = command->clear.first_level + command->clear.level_count;
	for (uint32_t level = command->clear.first_level; level < end; level++)
		xsdk_texture_upload_rows(&image->texture, level, 0, level_size(image->height, level), rows[ROW_OUT], 0);
	image_written(queue, image);
}

// memcpy into write-combined memory is safe, as the toolchain's stores bytes or aligned words (docs/SUPPORT.md).
void v4x_transfer_copy_buffer(VkQueue queue, const Command *command, volatile const void *source)
{
	wait_for_draws(queue);
	memcpy((void *)(uintptr_t)command->buffer_copy.destination, (const void *)(uintptr_t)source,
		command->buffer_copy.size_bytes);
	buffer_written(command->buffer_copy.address, command->buffer_copy.size_bytes);
}

void v4x_transfer_fill_buffer(VkQueue queue, const Command *command)
{
	wait_for_draws(queue);
	volatile uint32_t *const words = (volatile uint32_t *)(uintptr_t)command->buffer_copy.destination;
	for (uint32_t i = 0; i < command->buffer_copy.size_bytes / 4; i++)
		words[i] = command->buffer_copy.data;
	buffer_written(command->buffer_copy.address, command->buffer_copy.size_bytes);
}

// One word for every texel, so the texture's tiled order doesn't matter.
void v4x_transfer_fill_depth(VkQueue queue, Image *image, float depth, uint8_t stencil)
{
	const uint32_t word = (uint32_t)lrintf(depth * DEPTH_MAX) << 8 | stencil;
	wait_for_image(queue, image);
	volatile uint32_t *const texels = image->texture.texels;
	for (uint32_t i = 0; i < image->texture.size_bytes / 4; i++)
		texels[i] = word;
	image_written(queue, image);
}
