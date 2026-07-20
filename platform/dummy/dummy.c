/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>

#include <platform.h>

/* Clock, PRNG, VFS platform driver, and fs functions live in dummy-fs.c.
 * In-memory registry (Read/Remove) lives in dummy-registry.c.
 * In-memory wapp runtime state (Load/Unload/Start/Stop/Loop/GetState) lives
 * in dummy-wapps.c.
 * Network mock (PlatformNet*) lives in dummy-net.c. */

/* The unit-test platform offers no platform-specific drivers; a test that needs
 * one registers a deliberate fake here. A config naming an absent driver fails
 * with -ENODEV. */
static const vfs_driver_table_t dummy_driver_table[] = {
    {NULL, NULL},
};

const vfs_driver_table_t *PlatformDriverTable(void) {
    return dummy_driver_table;
}

/* PlatformRegistryWrite streams an image to a host file and renames it under
 * the install ref; host filesystem writes are out of scope for the dummy, so
 * this remains a stub. PlatformRegistryWappLoad chains a WASM load, also out of
 * scope. Read/Remove are implemented in dummy-registry.c. */
int PlatformRegistryWrite(write_state_t s, const char *ref, const uint8_t *buf,
                          size_t nbytes) {
    (void)s;
    (void)ref;
    (void)buf;
    (void)nbytes;
    return -ENOSYS;
}
int PlatformRegistryWappLoad(const reg_entry_t *entry, wapp_t *w) {
    (void)entry;
    (void)w;
    return -ENOSYS;
}

/* PlatformNet* live in dummy-net.c (controllable in-memory mock). */

void PlatformMemoryStats(size_t *heap_used, size_t *heap_total) {
    if (heap_used)
        *heap_used = 0;
    if (heap_total)
        *heap_total = 0;
}

void PlatformStorageStats(size_t *free_b, size_t *total_b) {
    if (free_b)
        *free_b = 0;
    if (total_b)
        *total_b = 0;
}

const char *PlatformName(void) { return "dummy"; }

/* The dummy platform is single-threaded (unit tests), so the mutex is a no-op.
 * A non-NULL sentinel is returned so callers can still distinguish allocation
 * failure (NULL) from a successfully created lock. */
static int dummy_mutex_sentinel;

platform_mutex_t *PlatformMutexNew(void) {
    return (platform_mutex_t *)&dummy_mutex_sentinel;
}
void PlatformMutexLock(platform_mutex_t *m) { (void)m; }
void PlatformMutexUnlock(platform_mutex_t *m) { (void)m; }
void PlatformMutexFree(platform_mutex_t *m) { (void)m; }
