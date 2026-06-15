/* SPDX-License-Identifier: Apache-2.0 */

#include <debug_trace.h>
#include <wanted_malloc.h>

#ifndef WANTED_CUSTOM_MALLOC
static size_t allocated;

void *WantedMalloc(size_t s) {
    void *p = malloc(s + sizeof(size_t));
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
    free(ptr);
}

size_t WantedGetAllocatedMem(void) { return allocated; }

#endif
