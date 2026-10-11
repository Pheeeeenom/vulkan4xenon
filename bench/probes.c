#include "probes.h"

#include <altivec.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "xsdk/gpu.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/thread.h"

enum {
	SLEEPS = 100,
	WRITE_BYTES = 1024 * 1024,
	WRITE_WORDS = WRITE_BYTES / 4,
	// Each write is timed this many times, and the fastest counts.
	WRITE_ROUNDS = 8,
	// The CPU's cache line (libxenon:drivers/ppc/cache.S:1).
	CACHE_LINE_BYTES = 128,
	// A copy from the cache, small enough that its source stays there between rounds.
	WARM_BYTES = 256 * 1024,
	PAGE_BYTES = 0x10000,
	NANOSECONDS_PER_MICROSECOND = 1000,
};

typedef enum {
	WRITE_COMBINED_STORES,
	WRITE_COMBINED_MEMCPY,
	CACHED_STORES,
	CACHED_FLUSH,
	WRITE_KINDS,
} WriteKind;

static uint64_t now_ns(void)
{
	struct timespec now;
	(void)clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * 1000000000u + (uint64_t)now.tv_nsec;
}

static uint32_t microseconds_since(uint64_t start_ns)
{
	return (uint32_t)((now_ns() - start_ns + NANOSECONDS_PER_MICROSECOND / 2) / NANOSECONDS_PER_MICROSECOND);
}

// The ring sleeps 1 ms at a time while it waits for room (xsdk:src/gpu.c).
static void probe_sleep(void)
{
	uint64_t total_us = 0;
	uint32_t shortest_us = UINT32_MAX;
	uint32_t longest_us = 0;
	for (int i = 0; i < SLEEPS; i++) {
		const uint64_t start = now_ns();
		(void)xsdk_thread_sleep(1);
		const uint32_t took_us = microseconds_since(start);
		total_us += took_us;
		shortest_us = took_us < shortest_us ? took_us : shortest_us;
		longest_us = took_us > longest_us ? took_us : longest_us;
	}
	xsdk_log_write("vkbench: a 1 ms sleep took %u us on average, from %u to %u, over %d", (unsigned)(total_us / SLEEPS),
		(unsigned)shortest_us, (unsigned)longest_us, SLEEPS);
}

// One round of each write, keeping each one's fastest time. The stores write what they compute, as packets are.
static void time_writes(
	volatile uint32_t *combined, volatile uint32_t *cached, const uint32_t *source, uint32_t fastest_us[WRITE_KINDS])
{
	uint64_t start = now_ns();
	for (uint32_t i = 0; i < WRITE_WORDS; i++)
		combined[i] = i;
	uint32_t took_us[WRITE_KINDS];
	took_us[WRITE_COMBINED_STORES] = microseconds_since(start);

	start = now_ns();
	memcpy((void *)combined, source, WRITE_BYTES);
	took_us[WRITE_COMBINED_MEMCPY] = microseconds_since(start);

	start = now_ns();
	for (uint32_t i = 0; i < WRITE_WORDS; i++)
		cached[i] = i;
	took_us[CACHED_STORES] = microseconds_since(start);

	// What libxenon does before the GPU reads memory the CPU wrote (libxenon:drivers/ppc/cache.S:22-36).
	start = now_ns();
	for (uint32_t offset = 0; offset < WRITE_BYTES; offset += CACHE_LINE_BYTES)
		__asm__ volatile("dcbst 0, %0" : : "r"((volatile uint8_t *)cached + offset) : "memory");
	__asm__ volatile("sync" ::: "memory");
	took_us[CACHED_FLUSH] = microseconds_since(start);

	for (int k = 0; k < WRITE_KINDS; k++)
		fastest_us[k] = took_us[k] < fastest_us[k] ? took_us[k] : fastest_us[k];
}

static uint32_t megabytes_per_second(uint32_t microseconds)
{
	return microseconds ? WRITE_BYTES / microseconds : 0;
}

// The ring and vertex streams are write-combined; libxenon records into cached memory and flushes it instead.
static void probe_writes(void)
{
	uint32_t *const source = malloc(WRITE_BYTES);
	volatile uint32_t *const combined = xsdk_gpu_alloc(WRITE_BYTES);
	volatile uint32_t *const cached =
		MmAllocatePhysicalMemoryEx(0, WRITE_BYTES, PAGE_READWRITE | MEM_LARGE_PAGES, 0, UINT32_MAX, PAGE_BYTES);
	if (!source || !combined || !cached) {
		xsdk_log_write("vkbench: no memory to time writes in");
		free(source);
		xsdk_gpu_free(combined);
		if (cached)
			MmFreePhysicalMemory(0, (void *)cached);
		return;
	}
	for (uint32_t i = 0; i < WRITE_WORDS; i++)
		source[i] = i;
	uint32_t fastest_us[WRITE_KINDS] = { UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX };
	for (int round = 0; round < WRITE_ROUNDS; round++)
		time_writes(combined, cached, source, fastest_us);
	const uint32_t cached_us = fastest_us[CACHED_STORES] + fastest_us[CACHED_FLUSH];
	xsdk_log_write(
		"vkbench: writing 1 MB, fastest of %d: write-combined, 32-bit stores %u us (%u MB/s), memcpy %u us (%u MB/s); "
		"cached, 32-bit stores %u us then dcbst %u us (%u MB/s)",
		WRITE_ROUNDS, (unsigned)fastest_us[WRITE_COMBINED_STORES],
		(unsigned)megabytes_per_second(fastest_us[WRITE_COMBINED_STORES]), (unsigned)fastest_us[WRITE_COMBINED_MEMCPY],
		(unsigned)megabytes_per_second(fastest_us[WRITE_COMBINED_MEMCPY]), (unsigned)fastest_us[CACHED_STORES],
		(unsigned)fastest_us[CACHED_FLUSH], (unsigned)megabytes_per_second(cached_us));
	MmFreePhysicalMemory(0, (void *)cached);
	xsdk_gpu_free(combined);
	free(source);
}

// Pushes memory's lines out of the cache, so a copy reads or writes memory itself.
static void flush_from_cache(const void *memory)
{
	for (uint32_t offset = 0; offset < WRITE_BYTES; offset += CACHE_LINE_BYTES)
		__asm__ volatile("dcbf 0, %0" : : "r"((const uint8_t *)memory + offset) : "memory");
	__asm__ volatile("sync" ::: "memory");
}

// Asks for the source's line some lines ahead, so it's in the cache when the copy gets there.
static void prefetch(const uint8_t *source, uint32_t offset, uint32_t bytes, uint32_t lines)
{
	if (offset + lines * CACHE_LINE_BYTES < bytes)
		__asm__ volatile("dcbt 0, %0" : : "r"(source + offset + lines * CACHE_LINE_BYTES));
}

// The SDK's own since milestone 20 (xsdk:src/copy.c), as every memcpy in the title is.
static void copy_memcpy(void *destination, const void *source, uint32_t bytes, uint32_t prefetch_lines)
{
	(void)prefetch_lines;
	memcpy(destination, source, bytes);
}

// A line through eight VMX registers, stored in order: the barriers keep GCC from reordering the stores.
static void copy_line_vmx(vector unsigned char *out, const vector unsigned char *in)
{
	const vector unsigned char v0 = in[0], v1 = in[1], v2 = in[2], v3 = in[3];
	const vector unsigned char v4 = in[4], v5 = in[5], v6 = in[6], v7 = in[7];
	__asm__ volatile("" ::: "memory");
	out[0] = v0;
	__asm__ volatile("" ::: "memory");
	out[1] = v1;
	__asm__ volatile("" ::: "memory");
	out[2] = v2;
	__asm__ volatile("" ::: "memory");
	out[3] = v3;
	__asm__ volatile("" ::: "memory");
	out[4] = v4;
	__asm__ volatile("" ::: "memory");
	out[5] = v5;
	__asm__ volatile("" ::: "memory");
	out[6] = v6;
	__asm__ volatile("" ::: "memory");
	out[7] = v7;
	__asm__ volatile("" ::: "memory");
}

static void copy_vmx_dcbt(void *destination, const void *source, uint32_t bytes, uint32_t prefetch_lines)
{
	for (uint32_t offset = 0; offset < bytes; offset += CACHE_LINE_BYTES) {
		prefetch(source, offset, bytes, prefetch_lines);
		copy_line_vmx((vector unsigned char *)((uint8_t *)destination + offset),
			(const vector unsigned char *)((const uint8_t *)source + offset));
	}
}

// dcbz zeroes the destination's line in the cache, so it isn't read from memory first; cached memory only.
static void copy_vmx_dcbt_dcbz(void *destination, const void *source, uint32_t bytes, uint32_t prefetch_lines)
{
	for (uint32_t offset = 0; offset < bytes; offset += CACHE_LINE_BYTES) {
		prefetch(source, offset, bytes, prefetch_lines);
		__asm__ volatile("dcbz 0, %0" : : "r"((uint8_t *)destination + offset) : "memory");
		copy_line_vmx((vector unsigned char *)((uint8_t *)destination + offset),
			(const vector unsigned char *)((const uint8_t *)source + offset));
	}
}

typedef struct {
	const char *name;
	void (*copy)(void *destination, const void *source, uint32_t bytes, uint32_t prefetch_lines);
	uint32_t prefetch_lines;
	uint32_t bytes;
	// Into cached memory, else write-combined.
	bool cached;
	// From a source already in the cache, else from memory.
	bool warm;
} CopyProbe;

static const CopyProbe copy_probes[] = {
	{ "memcpy", copy_memcpy, 0, WRITE_BYTES, false, false },
	{ "memcpy", copy_memcpy, 0, WARM_BYTES, false, true },
	{ "VMX stores and dcbt 8 lines ahead", copy_vmx_dcbt, 8, WRITE_BYTES, false, false },
	{ "VMX stores and dcbt 16 lines ahead", copy_vmx_dcbt, 16, WRITE_BYTES, false, false },
	{ "VMX stores and dcbt 32 lines ahead", copy_vmx_dcbt, 32, WRITE_BYTES, false, false },
	{ "memcpy", copy_memcpy, 0, WRITE_BYTES, true, false },
	{ "VMX stores, dcbt and dcbz", copy_vmx_dcbt_dcbz, 8, WRITE_BYTES, true, false },
};

static bool copied(volatile const uint32_t *destination, const uint32_t *source, uint32_t bytes)
{
	for (uint32_t i = 0; i < bytes / 4; i++) {
		if (destination[i] != source[i])
			return false;
	}
	return true;
}

// Each copy gets its own line as soon as it's timed, so a copy that crashes the title shows in the log.
static void time_copy(const CopyProbe *probe, uint32_t *source, volatile uint32_t *combined, volatile uint32_t *cached)
{
	volatile uint32_t *const destination = probe->cached ? cached : combined;
	uint32_t fastest_us = UINT32_MAX;
	for (int round = 0; round < WRITE_ROUNDS; round++) {
		if (probe->warm)
			probe->copy((void *)destination, source, probe->bytes, probe->prefetch_lines);
		else
			flush_from_cache(source);
		if (probe->cached)
			flush_from_cache((const void *)destination);
		const uint64_t start = now_ns();
		probe->copy((void *)destination, source, probe->bytes, probe->prefetch_lines);
		const uint32_t took_us = microseconds_since(start);
		fastest_us = took_us < fastest_us ? took_us : fastest_us;
	}
	xsdk_log_write("vkbench: copying %u KB from %s into %s memory, fastest of %d, %s: %u us (%u MB/s)%s",
		(unsigned)(probe->bytes / 1024), probe->warm ? "the cache" : "memory",
		probe->cached ? "cached" : "write-combined", WRITE_ROUNDS, probe->name, (unsigned)fastest_us,
		(unsigned)(fastest_us ? probe->bytes / fastest_us : 0),
		copied(destination, source, probe->bytes) ? "" : "; the copy came out wrong");
	memset((void *)destination, 0, WRITE_BYTES);
}

// How fast the SDK's memcpy copies, and how far ahead it should prefetch (xsdk:SCOPE.md, milestone 20).
static void probe_copies(void)
{
	uint32_t *const source =
		MmAllocatePhysicalMemoryEx(0, WRITE_BYTES, PAGE_READWRITE | MEM_LARGE_PAGES, 0, UINT32_MAX, PAGE_BYTES);
	volatile uint32_t *const combined = xsdk_gpu_alloc(WRITE_BYTES);
	volatile uint32_t *const cached =
		MmAllocatePhysicalMemoryEx(0, WRITE_BYTES, PAGE_READWRITE | MEM_LARGE_PAGES, 0, UINT32_MAX, PAGE_BYTES);
	if (source && combined && cached) {
		for (uint32_t i = 0; i < WRITE_WORDS; i++)
			source[i] = i * 2654435761u;
		for (uint32_t p = 0; p < sizeof copy_probes / sizeof copy_probes[0]; p++)
			time_copy(&copy_probes[p], source, combined, cached);
	} else {
		xsdk_log_write("vkbench: no memory to time copies in");
	}
	if (cached)
		MmFreePhysicalMemory(0, (void *)cached);
	xsdk_gpu_free(combined);
	if (source)
		MmFreePhysicalMemory(0, source);
}

void probes_run(void)
{
	probe_sleep();
	probe_writes();
	probe_copies();
}
