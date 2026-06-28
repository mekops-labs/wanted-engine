/* SPDX-License-Identifier: Apache-2.0 */

#include <debug_trace.h>
#include <platform.h>
#include <wanted_malloc.h>

#ifndef WANTED_CUSTOM_MALLOC
static size_t allocated;

/* The engine's general allocator routes through the platform external-RAM heap
 * (PSRAM on ESP32, plain malloc elsewhere). Now that the wapp registry lives on
 * an SD card, no cache-disabling internal-flash read runs while a wapp holds
 * live PSRAM, so all engine bookkeeping (TarFS index, launch config, VFS
 * namespace, wapp structs) is safe in PSRAM — which keeps scarce internal DRAM
 * for the things that *must* be internal: worker stacks and WiFi/net DMA. An
 * allocation that genuinely needs internal RAM should call malloc() directly
 * rather than WantedMalloc(). */
void *WantedMalloc(size_t s) {
    void *p = PlatformExtramMalloc(s + sizeof(size_t));
    if (p != NULL) {
        allocated += s;
        *(size_t *)p = s;
        p = (size_t *)p + 1;
    }
    DEBUG_TRACE("%zu (%zu)", s, allocated);

    return p;
}

void WantedFree(void *ptr) {
    if (!ptr)
        return;
    ptr = (size_t *)ptr - 1;
    allocated -= *(size_t *)ptr;
    DEBUG_TRACE("%zu (%zu)", *(size_t *)ptr, allocated);
    PlatformExtramFree(ptr);
}

size_t WantedGetAllocatedMem(void) { return allocated; }

#endif
