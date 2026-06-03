#include <errno.h>

#include <platform.h>

/* Clock, PRNG, VFS platform driver, and fs functions live in dummy-fs.c.
 * In-memory registry (Read/Remove) lives in dummy-registry.c.
 * In-memory wapp runtime state (Load/Unload/Start/Stop/Loop/GetState) lives
 * in dummy-wapps.c.
 * Network mock (PlatformNet*) lives in dummy-net.c. */

/* PlatformRegistryWrite requires parsing a WASM manifest to derive name and
 * version (FINISH_WRITE); WASM loading is out of scope for the dummy, so this
 * remains a stub. PlatformRegistryWappLoad chains a WASM load for the same
 * reason. Read/Remove are implemented in dummy-registry.c. */
int PlatformRegistryWrite(write_state_t s, const uint8_t *buf, size_t nbytes) {
    (void)s; (void)buf; (void)nbytes; return -ENOSYS;
}
int PlatformRegistryWappLoad(const reg_entry_t *entry, wapp_t *w) { (void)entry; (void)w; return -ENOSYS; }

/* PlatformNet* live in dummy-net.c (controllable in-memory mock). */

void PlatformMemoryStats(size_t *heap_used, size_t *heap_total) {
    if (heap_used) *heap_used = 0;
    if (heap_total) *heap_total = 0;
}
