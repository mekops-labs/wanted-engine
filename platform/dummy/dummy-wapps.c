/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <string.h>

#include <platform.h>

#include "dummy-fs.h"

/* ── In-memory wapp runtime state ───────────────────────────────────────────
 * Deterministic, dependency-free stand-in for the host wapp supervisor. Backs
 * PlatformWappGetState and is mutated by PlatformWappStart / PlatformWappStop
 * so control-plane round-trips (write a verb, read back state) are testable
 * without a real WASM runtime. Tests seed/clear it via DummyWappStateSeed /
 * DummyWappStateReset.
 * ───────────────────────────────────────────────────────────────────────── */

#define DUMMY_WAPP_MAX_ENTRIES MAX_WAPPS

static wapp_state_t g_state[DUMMY_WAPP_MAX_ENTRIES];
static int g_used[DUMMY_WAPP_MAX_ENTRIES];

static int state_find(const char *name) {
    for (int i = 0; i < DUMMY_WAPP_MAX_ENTRIES; i++) {
        if (g_used[i] && strncmp(g_state[i].name, name, WAPP_MAX_NAME_LEN) == 0)
            return i;
    }
    return -1;
}

static int state_alloc(void) {
    for (int i = 0; i < DUMMY_WAPP_MAX_ENTRIES; i++) {
        if (!g_used[i])
            return i;
    }
    return -1;
}

/* ── Test control ───────────────────────────────────────────────────────── */

void DummyWappStateReset(void) {
    memset(g_state, 0, sizeof(g_state));
    memset(g_used, 0, sizeof(g_used));
}

int DummyWappStateSeed(const wapp_state_t *states, size_t count) {
    if (!states)
        return -EINVAL;

    int stored = 0;
    for (size_t i = 0; i < count; i++) {
        int idx = state_find(states[i].name);
        if (idx < 0) {
            idx = state_alloc();
            if (idx < 0)
                return -ENOSPC;
            g_used[idx] = 1;
        }
        g_state[idx] = states[i];
        stored++;
    }
    return stored;
}

/* ── Platform wapp API ──────────────────────────────────────────────────── */

int PlatformWappLoad(const char *name, wapp_t *wapp) {
    (void)name;
    (void)wapp;
    return 0;
}

int PlatformWappUnload(const wapp_t *wapp) {
    (void)wapp;
    return 0;
}

/* Signature fixed by the platform.h PlatformWappStart contract. */
/* cppcheck-suppress constParameterPointer */
int PlatformWappStart(wapp_t *app) {
    if (!app)
        return -EINVAL;

    int idx = state_find(app->name);
    if (idx < 0) {
        idx = state_alloc();
        if (idx < 0)
            return -ENOSPC;
        memset(&g_state[idx], 0, sizeof(g_state[idx]));
        strncpy(g_state[idx].name, app->name, WAPP_MAX_NAME_LEN - 1);
        g_state[idx].name[WAPP_MAX_NAME_LEN - 1] = '\0';
        g_state[idx].id = (uint8_t)(idx + 1);
        g_used[idx] = 1;
    }
    strncpy(g_state[idx].image, app->image, WAPP_MAX_NAME_LEN - 1);
    g_state[idx].image[WAPP_MAX_NAME_LEN - 1] = '\0';
    strncpy(g_state[idx].version, app->version, WAPP_MAX_VERSION_LEN - 1);
    g_state[idx].version[WAPP_MAX_VERSION_LEN - 1] = '\0';
    g_state[idx].status = RUNNING;
    g_state[idx].exit_code = WAPP_EXIT_CODE_NONE;
    return 0;
}

int PlatformWappStop(const char *name) {
    if (!name)
        return -EINVAL;

    int idx = state_find(name);
    if (idx < 0)
        return -ENOENT;
    g_state[idx].status = EXITED;
    return 0;
}

int PlatformWappRelease(const char *name) {
    if (!name)
        return -EINVAL;

    int idx = state_find(name);
    if (idx < 0)
        return -ENOENT;

    /* Only a terminal slot can be released; a running/starting wapp must be
     * stopped first. */
    if (g_state[idx].status != EXITED && g_state[idx].status != FAILURE)
        return -EBUSY;

    g_used[idx] = 0;
    memset(&g_state[idx], 0, sizeof(g_state[idx]));
    return 0;
}

void PlatformWappLoop(void) {}

void PlatformSetProcessArgs(int argc, char **argv) {
    (void)argc;
    (void)argv;
}

void PlatformRequestShutdown(void) {}

void PlatformRequestReboot(void) {}

int PlatformWappGetState(wapp_state_t *apps, size_t appsLen) {
    int n = 0;
    for (int i = 0; i < DUMMY_WAPP_MAX_ENTRIES; i++) {
        if (!g_used[i])
            continue;
        if (apps != NULL && (size_t)n < appsLen)
            apps[n] = g_state[i];
        n++;
    }
    return n;
}
