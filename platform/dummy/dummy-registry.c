/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <string.h>

#include <platform.h>

#include "dummy-fs.h"

/* ── In-memory registry ─────────────────────────────────────────────────────
 * Deterministic, dependency-free stand-in for the host registry. Backs
 * PlatformRegistryRead / PlatformRegistryRemove. The real PlatformRegistryWrite
 * is a chunked state machine that streams an image to a host file and renames
 * it under the install ref — out of scope for the dummy. Tests populate via
 * DummyRegistrySeed.
 * ───────────────────────────────────────────────────────────────────────── */

#define DUMMY_REG_MAX_ENTRIES 8

typedef struct {
    int used;
    char name[WAPP_MAX_NAME_LEN];
    char version[WAPP_MAX_VERSION_LEN];
    size_t size;
} dummy_reg_entry_t;

static dummy_reg_entry_t g_registry[DUMMY_REG_MAX_ENTRIES];

static int reg_find(const char *name) {
    for (int i = 0; i < DUMMY_REG_MAX_ENTRIES; i++) {
        if (g_registry[i].used &&
            strncmp(g_registry[i].name, name, WAPP_MAX_NAME_LEN) == 0)
            return i;
    }
    return -1;
}

static int reg_alloc(void) {
    for (int i = 0; i < DUMMY_REG_MAX_ENTRIES; i++) {
        if (!g_registry[i].used)
            return i;
    }
    return -1;
}

int PlatformRegistryReadImage(const reg_entry_t *entry, uint8_t *buf,
                              size_t maxLen) {
    (void)entry;
    if (buf == NULL || maxLen == 0)
        return -EINVAL;

    /* A canned ustar-headed image: the "app.wasm" member name at offset 0 and a
     * minimal wasm declaring (memory 1 4) at the 512-byte content boundary, so
     * the descriptor render path is exercisable without real image storage. */
    static const uint8_t wasm[] = {0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00,
                                   0x00, 0x05, 0x04, 0x01, 0x01, 0x01, 0x04};
    size_t total = 512 + sizeof(wasm);
    size_t copy = total < maxLen ? total : maxLen;
    memset(buf, 0, copy);
    if (copy >= 8)
        memcpy(buf, "app.wasm", 8);
    if (copy > 512)
        memcpy(buf + 512, wasm, copy - 512);
    return (int)copy;
}

/* ── Test control ───────────────────────────────────────────────────────── */

void DummyRegistryReset(void) { memset(g_registry, 0, sizeof(g_registry)); }

int DummyRegistrySeed(const reg_entry_t *entries, size_t count) {
    if (!entries)
        return -EINVAL;

    int stored = 0;
    for (size_t i = 0; i < count; i++) {
        int idx = reg_find(entries[i].name);
        if (idx < 0) {
            idx = reg_alloc();
            if (idx < 0)
                return -ENOSPC;
            g_registry[idx].used = 1;
        }
        strncpy(g_registry[idx].name, entries[i].name, WAPP_MAX_NAME_LEN - 1);
        g_registry[idx].name[WAPP_MAX_NAME_LEN - 1] = '\0';
        strncpy(g_registry[idx].version, entries[i].version,
                WAPP_MAX_VERSION_LEN - 1);
        g_registry[idx].version[WAPP_MAX_VERSION_LEN - 1] = '\0';
        g_registry[idx].size = entries[i].size;
        stored++;
    }
    return stored;
}

/* ── Platform registry API ──────────────────────────────────────────────── */

int PlatformRegistryRead(reg_entry_t *registryList, size_t len) {
    int n = 0;
    for (int i = 0; i < DUMMY_REG_MAX_ENTRIES; i++) {
        if (!g_registry[i].used)
            continue;
        if (registryList != NULL && (size_t)n < len) {
            memset(&registryList[n], 0, sizeof(registryList[n]));
            strncpy(registryList[n].name, g_registry[i].name,
                    WAPP_MAX_NAME_LEN - 1);
            strncpy(registryList[n].version, g_registry[i].version,
                    WAPP_MAX_VERSION_LEN - 1);
            registryList[n].size = g_registry[i].size;
        }
        n++;
    }
    return n;
}

int PlatformRegistryRemove(const reg_entry_t *entry) {
    if (!entry)
        return -EINVAL;
    int idx = reg_find(entry->name);
    if (idx < 0)
        return -ENOENT;
    memset(&g_registry[idx], 0, sizeof(g_registry[idx]));
    return 0;
}
