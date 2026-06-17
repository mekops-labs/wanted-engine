/* SPDX-License-Identifier: Apache-2.0 */

#ifndef PLATFORM_H
#define PLATFORM_H

#include <stddef.h>
#include <stdint.h>

#include <vfs.h>
#include <wanted-api.h>

typedef uint32_t plat_clk_id_t;

#define THREAD_STACK_SIZE 8196

#define PLAT_CLOCKID_REALTIME 0U
#define PLAT_CLOCKID_MONOTONIC 1U
#define PLAT_CLOCKID_PROCESS_CPUTIME_ID 2U
#define PLAT_CLOCKID_THREAD_CPUTIME_ID 3U

typedef uint64_t plat_timestamp_t;
typedef uint16_t plat_clk_flags_t;

#define PLAT_CLOCK_FLAGS_ABSTIME 1U

typedef enum {
    START_WRITE,
    CONTINUE_WRITE,
    FINISH_WRITE,
    ABORT_WRITE,
} write_state_t;

int PlatformClockGetRes(plat_clk_id_t clk_id, uint64_t *resolution);
int PlatformClockGetTime(plat_clk_id_t clk_id, plat_timestamp_t *time);
int PlatformClockNanoSleep(plat_clk_id_t clk_id, plat_timestamp_t timeout,
                           plat_clk_flags_t flags);
int64_t PlatfromGetRandom(uint8_t *buf, size_t buf_len);

/* Opaque cross-platform mutex. src/ must not call native threading directly,
 * so shared state (e.g. the inter-wapp pipe store) guards itself through these.
 * Lock/Unlock/Free tolerate a NULL handle so callers need not special-case an
 * allocation failure from PlatformMutexNew. */
typedef struct platform_mutex_t platform_mutex_t;
platform_mutex_t *PlatformMutexNew(void);
void PlatformMutexLock(platform_mutex_t *m);
void PlatformMutexUnlock(platform_mutex_t *m);
void PlatformMutexFree(platform_mutex_t *m);

int PlatformWappLoad(const char *name, wapp_t *wapp);
int PlatformWappUnload(const wapp_t *wapp);
int PlatformWappStart(wapp_t *wapp);
/* Effective native C-stack size a worker thread is created with — the
 * configured WASM_WORKER_STACK_SIZE after the platform's own flooring (e.g.
 * PTHREAD_STACK_MIN). Reported via /proc/wanted so the value reflects what the
 * threads actually get, not just the compile-time request. */
size_t PlatformWorkerStackSize(void);
int PlatformWappStop(const char *name);
/* Release a wapp's platform slot by name: free its image + struct and drop the
 * record so the name stops being reported by PlatformWappGetState. Only a
 * terminal slot (EXITED/FAILURE) is releasable — a running or starting wapp
 * returns -EBUSY (stop it first); an unknown name returns -ENOENT. */
int PlatformWappRelease(const char *name);
void PlatformWappLoop(void);
int PlatformWappGetState(wapp_state_t *apps, size_t appsLen);
void PlatformMemoryStats(size_t *heap_used, size_t *heap_total);

/* Short identifier for the target the engine was built against ("linux",
 * "nuttx", "dummy"). Static storage; the caller must not free it. Exposed at
 * /proc/wanted so a wapp can read which platform hosts it. */
const char *PlatformName(void);

/* System control. A privileged wapp triggers these through the wanted host
 * module; PlatformWappLoop normally respawns a vanished supervisor forever, so
 * they are the only paths that end the engine. The request just sets a flag —
 * PlatformWappLoop performs the action after the current iteration so the
 * calling worker unwinds first. Shutdown stops the engine; reboot re-execs it
 * (host) or resets the board (NuttX). PlatformSetProcessArgs hands main()'s
 * argv to the platform so the host re-exec can target the same image. */
void PlatformSetProcessArgs(int argc, char **argv);
void PlatformRequestShutdown(void);
void PlatformRequestReboot(void);

int PlatformRegistryRead(reg_entry_t *registryList, size_t len);
/* Stream-install an image into the registry under an explicit target ref
 * ("<name>:<version>"), supplied at START_WRITE and used to name the stored
 * file at FINISH_WRITE. The ref is the image's identity. `ref` is ignored on
 * CONTINUE/FINISH/ABORT. */
int PlatformRegistryWrite(write_state_t s, const char *ref, const uint8_t *buf,
                          size_t nbytes);
int PlatformRegistryRemove(const reg_entry_t *entry);
int PlatformRegistryWappLoad(const reg_entry_t *entry, wapp_t *w);

/* Open a host-side directory that will be exposed to a wapp as a WASI preopen.
 * Read-write mounts create the directory if absent; a read-only mount requires
 * it to already exist (creating a directory only to deny writes is incoherent),
 * so a missing host dir on a `readonly` open returns -ENOENT. Returns a native
 * fd usable with openat(2)-class APIs, or a negative errno on failure. The
 * returned fd's lifetime is owned by the VFS layer (closed at VfsDestroy). */
int PlatformOpenStateDir(const char *path, bool readonly);

/* Host directory under which engine-managed wapp volumes live. A `volume` mount
 * backs a wapp's named store at <root>/<wapp>/<volname>, created on first use;
 * the wapp never sees this host path. On a host filesystem this is a directory
 * under the engine state root; on a flash target it is a persistent partition
 * mountpoint (e.g. LittleFS). Returns a stable, non-NULL absolute path. */
const char *PlatformVolumeRoot(void);

/* Thin wrappers over native fs primitives, used by VFS path_rename and
 * path_create_directory to operate on preopen-rooted directories. Both fds
 * are native (openat-class) directory descriptors. */
int PlatformFsRename(int old_fd, const char *old_path, int new_fd,
                     const char *new_path);
int PlatformFsMkdir(int fd, const char *path);

void *PlatformNetOpen(int socket_type);
int PlatformNetConnect(void *ctx, const char *hostname, uint16_t port);
int PlatformNetClose(void *ctx);
int PlatformNetRecv(void *ctx, void *buf, size_t nbyte, int flags);
int PlatformNetSend(void *ctx, const void *buf, size_t nbyte, int flags);
int PlatformNetAccept(void *ctx);
int PlatformNetShutdown(void *ctx, int how);
int PlatformNetFree(void *ctx);

#endif /* PLATFORM_H */
