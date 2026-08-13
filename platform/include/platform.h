/* SPDX-License-Identifier: Apache-2.0 */

#ifndef PLATFORM_H
#define PLATFORM_H

#include <stddef.h>
#include <stdint.h>

#include <vfs-drivers.h>
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

/* Ed25519 signature verification sizes (RFC 8032). */
#define PLATFORM_ED25519_KEY_LEN 32
#define PLATFORM_ED25519_SIG_LEN 64

/* Verify an Ed25519 signature over `msg` with a raw 32-byte public key.
 * 0 when valid, -EBADMSG when not, -ENOSYS with no backend in this build. */
int PlatformEd25519Verify(const uint8_t pubkey[PLATFORM_ED25519_KEY_LEN],
                          const uint8_t sig[PLATFORM_ED25519_SIG_LEN],
                          const uint8_t *msg, size_t msgLen);

/* Streaming SHA-256 (FIPS 180-4). Every platform provides a real backend, so
 * there is no -ENOSYS path. New returns NULL only on allocation failure. */
#define PLATFORM_SHA256_DIGEST_LEN 32

void *PlatformSha256New(void);
void PlatformSha256Update(void *ctx, const uint8_t *data, size_t len);
void PlatformSha256Final(void *ctx, uint8_t out[PLATFORM_SHA256_DIGEST_LEN]);
void PlatformSha256Free(void *ctx);

/* Opaque cross-platform mutex; src/ must not call native threading directly.
 * Lock/Unlock/Free tolerate a NULL handle, so a failed New needs no branch. */
typedef struct platform_mutex_t platform_mutex_t;
platform_mutex_t *PlatformMutexNew(void);
void PlatformMutexLock(platform_mutex_t *m);
void PlatformMutexUnlock(platform_mutex_t *m);
void PlatformMutexFree(platform_mutex_t *m);

/* This platform's driver table (config name -> VfsInitFunction_t), NULL-
 * terminated; NULL or an empty table is valid. Searched after the core table,
 * so a platform cannot shadow a core name. Ownership stays there. */
const vfs_driver_table_t *PlatformDriverTable(void);

int PlatformWappLoad(const char *name, wapp_t *wapp);
int PlatformWappUnload(const wapp_t *wapp);
int PlatformWappStart(wapp_t *wapp);
/* Native C-stack size a worker thread actually gets: the configured
 * CONFIG_WANTED_WASM_WORKER_STACK_SIZE after the platform's own flooring. */
size_t PlatformWorkerStackSize(void);
int PlatformWappStop(const char *name);

/* A descriptor a blocking wait watches beside the one it is serving. Raising it
 * ends that wait, which is how a stop reaches a wapp parked in a host call.
 * -1 on a platform whose stop interrupts the worker by signal. */
int PlatformWakeCreate(void);
/* Raising an already-raised descriptor is a no-op; -1 is ignored. */
void PlatformWakeRaise(int fd);
void PlatformWakeClose(int fd);

/* Free a wapp's platform slot by name. Only a terminal slot (EXITED/FAILURE)
 * is releasable: -EBUSY while running or starting, -ENOENT if unknown. */
int PlatformWappRelease(const char *name);
void PlatformWappLoop(void);
int PlatformWappGetState(wapp_state_t *apps, size_t appsLen);
void PlatformMemoryStats(size_t *heap_used, size_t *heap_total);

/* Free/total bytes of the store backing the registry and volumes — flash on a
 * board, a filesystem on a host. 0 when the platform cannot report it. */
void PlatformStorageStats(size_t *free_b, size_t *total_b);

/* External-RAM (PSRAM) allocator for large engine buffers, leaving internal RAM
 * for task stacks. Falls back to the ordinary heap. malloc-compatible. */
void *PlatformExtramMalloc(size_t size);
void *PlatformExtramRealloc(void *ptr, size_t size);
void PlatformExtramFree(void *ptr);

/* Carve out the extram pool now, before other allocations fragment the region
 * it needs a contiguous block from. Idempotent; call as early as possible. */
void PlatformExtramEarlyInit(void);

/* Short identifier for the target the engine was built against ("linux",
 * "nuttx", "dummy"). Static storage; the caller must not free it. Exposed at
 * /proc/wanted so a wapp can read which platform hosts it. */
const char *PlatformName(void);

/* Hex digits in a firmware digest (SHA-256). A buffer holding one needs a
 * further byte for the terminator. */
#define FIRMWARE_DIGEST_HEX_LEN 64

/* Lowercase-hex digest of the running image, NUL-terminated, into `buf`.
 * Stamped at build time, so it separates two builds sharing a version string.
 * Returns the length written, -ENOSYS where unstamped, -ENOSPC if too small. */
int PlatformFirmwareDigest(char *buf, size_t bufLen);

/* System control: the only paths that end the engine, since PlatformWappLoop
 * otherwise respawns a vanished supervisor forever. The request sets a flag the
 * loop acts on after the current iteration, so the worker unwinds first. */

/* main()'s argv, so a host reboot can re-exec the same image. */
void PlatformSetProcessArgs(int argc, char **argv);
void PlatformRequestShutdown(void);
void PlatformRequestReboot(void);

int PlatformRegistryRead(reg_entry_t *registryList, size_t len);
/* How many images the registry can hold, or 0 where it is bounded only by the
 * filesystem. A backing that keeps images in fixed flash slots has a hard
 * ceiling, and a supervisor cannot see it coming without being told. */
size_t PlatformRegistrySlots(void);
/* Stream-install an image under an explicit ref ("<name>:<version>") supplied
 * at START_WRITE, which names the stored file and is the image's identity.
 * `ref` is ignored on CONTINUE/FINISH/ABORT. */
int PlatformRegistryWrite(write_state_t s, const char *ref, const uint8_t *buf,
                          size_t nbytes);
/* Record `ref` ("<name>[:<version>]") as firmware-provided. A seeded image is
 * written back at the next boot, so removing it is churn a supervisor cannot
 * know to avoid; PlatformRegistryRemove refuses one with -EPERM. Call it for
 * every seed on every boot, whether or not the image had to be written.
 * Backings that seed nothing ignore it. */
void PlatformRegistryMarkSeeded(const char *ref);
int PlatformRegistryRemove(const reg_entry_t *entry);
int PlatformRegistryWappLoad(const reg_entry_t *entry, wapp_t *w);
/* Read up to maxLen leading bytes of the stored .wapp archive without loading
 * its layers — a cheap header peek. Returns the count or a negative errno. */
int PlatformRegistryReadImage(const reg_entry_t *entry, uint8_t *buf,
                              size_t maxLen);

/* Open a host directory to be exposed as a WASI preopen; returns a native
 * openat(2)-class fd owned by the VFS layer. A read-write mount creates it, a
 * read-only one requires it to exist and answers -ENOENT when it does not. */
int PlatformOpenStateDir(const char *path, bool readonly);

/* Host root for engine-managed volumes; a named store lives at
 * <root>/<wapp>/<volname>, created on first use and never seen by the wapp.
 * Returns a stable, non-NULL absolute path. */
const char *PlatformVolumeRoot(void);

/* Thin wrappers over native fs primitives, used by VFS path_rename,
 * path_create_directory and path_remove_directory to operate on preopen-rooted
 * directories. Both fds are native (openat-class) directory descriptors. */
int PlatformFsRename(int old_fd, const char *old_path, int new_fd,
                     const char *new_path);
int PlatformFsMkdir(int fd, const char *path);
int PlatformFsRmdir(int fd, const char *path);
int PlatformFsUnlink(int fd, const char *path);

/* GPIO backing for the core /dev/gpio driver: the platform owns only the line,
 * and `address` is the grant's middle field, interpreted here and nowhere else.
 * Open answers -ENOSYS, -EINVAL or -ENOTSUP, each of which fails the launch. */
typedef struct platform_gpio_t platform_gpio_t;

#define PLAT_GPIO_DIR_IN 0
#define PLAT_GPIO_DIR_OUT 1

#define PLAT_GPIO_PULL_NONE 0
#define PLAT_GPIO_PULL_UP 1
#define PLAT_GPIO_PULL_DOWN 2

#define PLAT_GPIO_DRIVE_PP 0 /* push-pull */
#define PLAT_GPIO_DRIVE_OD 1 /* open-drain */

typedef struct plat_gpio_cfg_t {
    const char *address;
    uint8_t direction; /* PLAT_GPIO_DIR_* */
    uint8_t pull;      /* PLAT_GPIO_PULL_* */
    uint8_t drive;     /* PLAT_GPIO_DRIVE_*, output only */
    uint8_t init;      /* the level an output takes at open, 0 or 1 */
} plat_gpio_cfg_t;

int PlatformGpioOpen(const plat_gpio_cfg_t *cfg, platform_gpio_t **out);
int PlatformGpioRead(const platform_gpio_t *g, bool *level);
int PlatformGpioWrite(platform_gpio_t *g, bool level);
void PlatformGpioClose(platform_gpio_t *g);

/* UART backing for the core /dev/uart driver: the platform owns the port, the
 * driver everything above it. `options` carries the grant keys the driver left
 * unconsumed; reject an unknown one. The port must be made exclusive here. */
typedef struct platform_uart_t platform_uart_t;

typedef struct plat_uart_cfg_t {
    const char *port;
    const char *options;
    uint32_t baud;
    uint8_t databits; /* 5..8 */
    uint8_t parity;   /* 'N', 'E', or 'O' */
    uint8_t stopbits; /* 1..2 */
} plat_uart_cfg_t;

/* Open the port and apply the initial line configuration. Returns -ENOSYS
 * where the target drives no UART, -EINVAL on an unusable port, address, or
 * line setting, and -EBUSY when the port is already held. */
int PlatformUartOpen(const plat_uart_cfg_t *cfg, platform_uart_t **out);

/* Apply a new baud rate and frame format to a live port: drain the transmit
 * buffer, then discard the receive buffer. -EINVAL when the backing cannot
 * produce the request exactly; never substitute the nearest achievable rate. */
int PlatformUartConfigure(platform_uart_t *u, const plat_uart_cfg_t *cfg);

/* Non-blocking. Returns the byte count read, 0 when nothing is buffered, or a
 * negative errno. The driver builds its blocking read on top of this. */
int PlatformUartRead(platform_uart_t *u, void *buf, size_t nbyte);

/* Non-blocking. Returns the byte count accepted, which may be short, 0 when
 * the transmit buffer is full, or a negative errno. */
int PlatformUartWrite(platform_uart_t *u, const void *buf, size_t nbyte);

void PlatformUartClose(platform_uart_t *u);

/* One transport endpoint, owned by the platform. Opaque to the engine. */
struct netCtx;

struct netCtx *PlatformNetOpen(int socket_type);
int PlatformNetConnect(struct netCtx *ctx, const char *hostname, uint16_t port);
int PlatformNetClose(struct netCtx *ctx);
int PlatformNetRecv(struct netCtx *ctx, void *buf, size_t nbyte, int flags);
int PlatformNetSend(struct netCtx *ctx, const void *buf, size_t nbyte,
                    int flags);
int PlatformNetShutdown(struct netCtx *ctx, int how);
int PlatformNetFree(struct netCtx *ctx);

/* Bind `ctx` to <bindAddr>:<port> and, on a stream transport, listen `backlog`
 * deep. A datagram transport is left bound: Recv reads it and Send answers the
 * peer the last datagram came from. */
int PlatformNetListen(struct netCtx *ctx, const char *bindAddr, uint16_t port,
                      int backlog);

/* Take one connection off a listening context's queue into *out, a context of
 * its own that the caller closes and frees. */
int PlatformNetAccept(struct netCtx *ctx, struct netCtx **out);

/* Wait until `ctx` has a connection to take, or `wakeFd` is raised. Answers 0
 * when the accept below will not block, and -EINTR on a raised wake. Callers
 * holding no wake descriptor let the accept block instead. */
int PlatformNetWaitAccept(struct netCtx *ctx, int wakeFd);

/* A/B firmware OTA: dual-slot update plus rollback, backed by whatever the
 * target boots through. Slots are always named 'a' and 'b', so the /dev/ota
 * wire text reads the same on every platform. */
typedef struct {
    char active_slot;      /* 'a' or 'b': the slot currently running */
    bool confirmed;        /* active_slot is marked good; no rollback armed */
    bool pending_swap;     /* the inactive slot is scheduled to run on the
                            * next boot (commit issued, reboot not yet
                            * observed) */
    char last_failed_slot; /* 'a', 'b', or '\0' if no slot has ever failed */
    int boot_attempts;     /* boot attempts recorded for active_slot */
    /* Build-time digest of the staged image, lowercase hex, empty when nothing
     * is staged or the target stamps none. This is what
     * PlatformFirmwareDigest reports once that image boots, so it is the value
     * a control plane compares to tell a staged image took -- not the digest
     * of the bytes it downloaded, which hashes a different artifact. */
    char pending_digest[FIRMWARE_DIGEST_HEX_LEN + 1];
} platform_ota_state_t;

/* Read the current boot state (running slot + otadata/trailer state of both
 * slots) into an engine-global OTA context. Called once at startup, before
 * PlatformWappLoop starts the supervisor. */
int PlatformOtaInit(void);
/* Mark the active slot good, disarming any pending rollback. Idempotent --
 * a no-op if the slot is already confirmed or no OTA is pending. */
int PlatformOtaConfirm(void);
int PlatformOtaGetBootState(platform_ota_state_t *out);
/* Erase the inactive slot and open it for a streaming image write. */
int PlatformOtaBeginWrite(void);
/* Write `len` bytes at the current write cursor into the inactive slot.
 * -EPERM if BeginWrite has not been called or Commit already issued. */
int PlatformOtaWrite(const uint8_t *buf, size_t len);
/* Finalise the write: validate the image and schedule the inactive slot for
 * the next boot, unconfirmed until PlatformOtaConfirm. A malformed image is
 * rejected with -EBADMSG and the boot partition left unchanged. */
int PlatformOtaCommit(void);
/* Discard a streaming write and release the session, leaving the boot partition
 * unchanged. A session begun and never committed holds the slot, and every
 * later BeginWrite answers -EBUSY until reboot. Idempotent. */
int PlatformOtaAbort(void);
/* Revert to the other slot; reverts a booted image and does not end a streaming
 * write. May reboot during the call, so the caller must not assume it
 * returns. */
int PlatformOtaRollback(void);

#endif /* PLATFORM_H */
