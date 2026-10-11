#include "probes.h"

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

void probes_run(void)
{
	probe_sleep();
	probe_writes();
}
