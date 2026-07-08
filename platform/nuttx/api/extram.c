/* SPDX-License-Identifier: Apache-2.0 */

/* External-RAM (PSRAM) heap for large engine buffers.
 *
 * On the ESP32 a task stack can only live in internal DRAM (it must stay
 * reachable while the cache is disabled for flash ops / ISRs), so internal RAM
 * is the scarce resource and must be reserved for worker stacks. The big engine
 * buffers — the wapp image cache and WAMR's linear memory — are plain data that
 * is happy in PSRAM. This carves a private NuttX `mm` heap out of one large
 * allocation: anything bigger than internal RAM can only be satisfied from the
 * PSRAM region of the common heap, so the pool is guaranteed PSRAM-backed. The
 * engine then allocates image/linear buffers from this heap, leaving internal
 * RAM for stacks.
 *
 * On the host-scaffolding build (no PSRAM) the calls fall back to the ordinary
 * heap. */

#include <stdlib.h>

#include <platform.h>

#ifdef __NuttX__

#include <stdbool.h>
#include <stdint.h>
#include <malloc.h>

#include <nuttx/mm/mm.h>

#include <debug_trace.h>

#ifndef WANTED_EXTRAM_POOL_SIZE
#define WANTED_EXTRAM_POOL_SIZE (3 * 1024 * 1024)
#endif

/* PSRAM cached window, per target - this file is shared across every NuttX
 * board the engine runs on, and the two currently supported have PSRAM at
 * unrelated addresses. This range was hardcoded to the ESP32's window
 * everywhere, so on RP2350 every probe below always misses regardless of
 * whether it actually landed in PSRAM - fixed here to the correct window
 * (verified: an RP2350 PSRAM allocation genuinely returns an address in
 * this range). Note this alone does not explain the large-allocation
 * failure investigated in plans/wanted-sheriff-deputy-uart-transport.md's
 * M4 status note - the size probes below still fail outright (not just
 * address-mismatch) for 1 MiB+ requests even with this fix in place; kept
 * because it is independently correct regardless. */
#if defined(CONFIG_ARCH_CHIP_RP23XX)
/* RP2350 Feather, CS1 cached window (see plans/wanted-engine-nuttx-rp2350.md
 * M5 and research/rp2350-vs-esp32s3-psram-flash-coherency.md): 8 MB @
 * 0x11000000. */
#define PSRAM_LO 0x11000000u
#define PSRAM_HI 0x11800000u
#else
/* ESP32 PSRAM cached window (CONFIG_HEAP2_BASE, 4 MB). */
#define PSRAM_LO 0x3f800000u
#define PSRAM_HI 0x3fc00000u
#endif

static struct mm_heap_s *g_extram;
static uintptr_t
    g_extram_lo; /* pool bounds, for routing free()/realloc() of a */
static uintptr_t g_extram_hi; /* pointer to the right heap (see in_extram). */
static bool g_extram_tried;

/* True when ptr was handed out by the PSRAM pool (vs the ordinary heap). The
 * pool is one contiguous mm region, so a range check is exact. NULL is treated
 * as "not ours" so free(NULL)/realloc(NULL,..) take the libc path. */
static bool in_extram(const void *ptr) {
    uintptr_t a = (uintptr_t)ptr;
    return (g_extram != NULL) && (a >= g_extram_lo) && (a < g_extram_hi);
}

static void extram_init(void) {
    if (g_extram_tried)
        return;
    g_extram_tried = true;

    /* Grab as much PSRAM as possible from the common heap so almost no external
     * RAM is left there — the common heap then hands out internal RAM only,
     * which keeps WiFi DMA (esp_malloc_internal rejects PSRAM) and task stacks
     * on internal RAM without needing a separate DMA iheap. Try from near the
     * full HEAP2 size down. */
    static const size_t try_sizes[] = {
        0x3F0000, 0x3E0000, 0x3C0000, 0x380000, 0x300000, 0x200000, 0x100000,
    };
    void *pool = NULL;
    size_t got = 0;
    for (unsigned i = 0; i < sizeof(try_sizes) / sizeof(try_sizes[0]); i++) {
        pool = malloc(try_sizes[i]);
        if (pool == NULL)
            continue;
        uintptr_t a = (uintptr_t)pool;
        if (a < PSRAM_LO || a >= PSRAM_HI) {
            free(pool); /* internal — not what we want for the pool */
            pool = NULL;
            continue;
        }
        got = try_sizes[i];
        break;
    }

    if (pool == NULL) {
        DEBUG_TRACE("extram: no PSRAM pool, using internal heap");
        return;
    }

    g_extram = mm_initialize("wanted-extram", pool, got);
    g_extram_lo = (uintptr_t)pool;
    g_extram_hi = (uintptr_t)pool + got;
    DEBUG_TRACE("extram: PSRAM heap @ %p (%u bytes)", pool, (unsigned)got);
}

/* Force the PSRAM grab as early in boot as possible. On RP2350 the merged
 * SRAM+PSRAM heap (CONFIG_MM_REGIONS=2, one segregated-fit allocator) starts
 * fragmenting the instant any other subsystem allocates - littlefs/ROMFS
 * mount, registry seeding, etc. - which chips the one giant PSRAM free node
 * into pieces well under 1 MiB long before extram_init()'s own multi-MB
 * malloc() probes ever run, so every probe fails outright even though the
 * *aggregate* free memory is still ~8 MiB (confirmed via mm_heapfree() vs.
 * mm_heapfree_largest(): ~8.4 MiB free, ~456 KiB largest contiguous block,
 * right before the probes start failing). Calling this before any other
 * heap activity avoids the fragmentation entirely. Not needed on ESP32,
 * whose PSRAM lives in its own separate heap_caps pool from boot. */
void PlatformExtramEarlyInit(void) { extram_init(); }

void *PlatformExtramMalloc(size_t size) {
    extram_init();
    return (g_extram != NULL) ? mm_malloc(g_extram, size) : malloc(size);
}

void *PlatformExtramRealloc(void *ptr, size_t size) {
    extram_init();
    /* Grow a pool pointer (or a fresh alloc, ptr==NULL) in the pool; resize an
     * ordinary-heap pointer with libc. mm_realloc must never see a foreign
     * pointer, hence the explicit in_extram gate. */
    if (g_extram != NULL && (ptr == NULL || in_extram(ptr)))
        return mm_realloc(g_extram, ptr, size);
    return realloc(ptr, size);
}

void PlatformExtramFree(void *ptr) {
    if (in_extram(ptr))
        mm_free(g_extram, ptr);
    else
        free(ptr);
}

#else /* !__NuttX__ — host scaffolding, no PSRAM */

void *PlatformExtramMalloc(size_t size) { return malloc(size); }
void *PlatformExtramRealloc(void *ptr, size_t size) {
    return realloc(ptr, size);
}
void PlatformExtramFree(void *ptr) { free(ptr); }
void PlatformExtramEarlyInit(void) {}

#endif /* __NuttX__ */
