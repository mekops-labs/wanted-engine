/* SPDX-License-Identifier: Apache-2.0 */

#ifndef PLATFORM_H
#define PLATFORM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <registry-meta.h>
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
/* Set the wall clock to `time` ns since the Unix epoch. Only REALTIME is
 * settable. Returns 0, -EINVAL for another clock, or -errno when the host
 * refuses. */
int PlatformClockSetTime(plat_clk_id_t clk_id, plat_timestamp_t time);

/* True when an operating system under the engine owns the wall clock and keeps
 * it. False when the engine's platform starts the clock at zero. */
bool PlatformClockIsHosted(void);
int PlatformClockNanoSleep(plat_clk_id_t clk_id, plat_timestamp_t timeout,
                           plat_clk_flags_t flags);

/* Hand the CPU to another ready thread; returns 0 or a negative errno. */
int PlatformYield(void);

/* Fill `buf` with random bytes. Returns how many were written, at least 1 for a
 * non-empty buffer, or a negative errno. The caller asks again after a short
 * fill. */
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
/* Map a whole file read-only, or copy it where mapping is not available.
 * PlatformFileUnmap releases what PlatformFileMap returned. */
int PlatformFileMap(const char *path, uint8_t **ptr, size_t *len);
int PlatformFileUnmap(uint8_t *ptr, size_t len);
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
/* Whether `fd` is raised, without waiting. A polling driver asks between
 * attempts, where a descriptor to select on would not fit. */
bool PlatformWakeRaised(int fd);
void PlatformWakeClose(int fd);

/* Why the current boot started, as a short stable token ("poweron", "sw",
 * "panic", "task_wdt", "int_wdt", "brownout", "deepsleep", "unknown").
 * Writes at most `len` bytes incl. terminator; 0 if the platform can't tell. */
size_t PlatformResetReason(char *buf, size_t len);

/* Memory a reset does not clear — a watchdog, a panic, a commanded reboot —
 * holding whatever the previous boot left. NULL where the platform has none,
 * and on every platform it is lost when power is. `len` takes the size. */
void *PlatformPersistMem(size_t *len);

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

/* Brackets a read of memory that a registry store maps from flash. Where a
 * flash write can hand such a read erased bytes, the pair keeps the two apart.
 * Reads are done in short steps so a write is never held off for long. */
#ifdef PLATFORM_ESP_IDF
void PlatformFlashReadBegin(void);
void PlatformFlashReadEnd(void);
#else
static inline void PlatformFlashReadBegin(void) {}
static inline void PlatformFlashReadEnd(void) {}
#endif

#define PLATFORM_FLASH_READ_STEP 4096u

/* Copies `n` bytes out of a mapping under the guard. */
static inline void PlatformMappedCopy(void *dst, const void *src, size_t n) {
    size_t done = 0;

    while (done < n) {
        size_t step = n - done < PLATFORM_FLASH_READ_STEP
                          ? n - done
                          : PLATFORM_FLASH_READ_STEP;

        PlatformFlashReadBegin();
        memcpy((uint8_t *)dst + done, (const uint8_t *)src + done, step);
        PlatformFlashReadEnd();
        done += step;
    }
}
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

/* Longest serial number a platform may report, excluding the terminator. */
#define PLATFORM_SERIAL_MAX_LEN 64

/* The hardware's own serial number, NUL-terminated, into `buf`. Stable across
 * reflashes and reboots, so it identifies the unit rather than the image.
 * Returns the length written, -ENOSYS where the platform has no such source,
 * -ENOSPC if too small. A source that is broadcast over the air is not one:
 * a caller may derive a secret from this value. */
int PlatformSerialNumber(char *buf, size_t bufLen);

/* System control: the only paths that end the engine, since PlatformWappLoop
 * otherwise respawns a vanished supervisor forever. The request sets a flag the
 * loop acts on after the current iteration, so the worker unwinds first. */

/* main()'s argv, so a host reboot can re-exec the same image. */
void PlatformSetProcessArgs(int argc, char **argv);
void PlatformRequestShutdown(void);
void PlatformRequestReboot(void);

/* Open a host directory to be exposed as a WASI preopen; returns a native
 * openat(2)-class fd owned by the VFS layer. A read-write mount creates it, a
 * read-only one requires it to exist and answers -ENOENT when it does not. */
int PlatformOpenStateDir(const char *path, bool readonly);

/* Host root for engine-managed volumes; a named store lives at
 * <root>/<wapp>/<volname>, created on first use and never seen by the wapp.
 * Returns a stable, non-NULL absolute path. */
const char *PlatformVolumeRoot(void);

/* Read a whole host file into `buf`, NUL-terminated. For small documents the
 * engine reads outside any wapp's namespace — the launch-config overlay is the
 * only one today. Returns the length written, -ENOENT when absent, -ENOSPC
 * when the file does not fit, or another negative errno. */
int PlatformReadSmallFile(const char *path, char *buf, size_t bufLen);

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

/* LED backing for the core /dev/led driver. `address` is the grant's middle
 * field, read only here. Open returns -ENOSYS, -EINVAL, -EBUSY or -ENOSPC, each
 * of which fails the launch. A level is 0..PlatformLedMax. */
typedef struct platform_led_t platform_led_t;

#define PLAT_LED_MODE_ONOFF 0
#define PLAT_LED_MODE_PWM 1

typedef struct plat_led_cfg_t {
    const char *address;
    uint8_t mode; /* PLAT_LED_MODE_* */
} plat_led_cfg_t;

int PlatformLedOpen(const plat_led_cfg_t *cfg, platform_led_t **out);
unsigned PlatformLedMax(const platform_led_t *l);
/* True when a fade runs in hardware, with no further calls. */
bool PlatformLedHwFade(const platform_led_t *l);
int PlatformLedSet(platform_led_t *l, unsigned level);
/* Ramp to `level` over `ms` and return at once. An on/off LED switches at the
 * end of the fade. */
int PlatformLedFade(platform_led_t *l, unsigned level, unsigned ms);
/* The level the LED shows now. */
unsigned PlatformLedGet(const platform_led_t *l);
void PlatformLedClose(platform_led_t *l);

/* Motor backing for the core /dev/haptic driver: a motor on one pin.
 * `address` is the grant's middle field, read only here. Open returns
 * -ENOSYS, -EINVAL, -EBUSY or -ENOSPC, each of which fails the launch. */
typedef struct platform_motor_t platform_motor_t;

int PlatformMotorOpen(const char *address, platform_motor_t **out);
/* Run `n` alternating on and off durations in ms, on first, and return at
 * once. Replaces a running pattern; the motor ends off. */
int PlatformMotorPattern(platform_motor_t *m, const uint16_t *ms, size_t n);
/* Switch the motor off and drop any running pattern. */
void PlatformMotorStop(platform_motor_t *m);
/* True while a pattern runs. */
bool PlatformMotorBusy(const platform_motor_t *m);
void PlatformMotorClose(platform_motor_t *m);

/* Power backing for the core /dev/power driver. A light sleep halts the CPU
 * and returns where it stopped; a deep sleep resets it. `timerUs` is 0 for no
 * timer. The state block survives a deep sleep; a load reports the time since
 * the store. */
typedef enum plat_power_wake_t {
    PLAT_POWER_WAKE_TIMER = 0,
    PLAT_POWER_WAKE_EXTERNAL
} plat_power_wake_t;

typedef enum plat_power_boot_t {
    PLAT_POWER_BOOT_COLD = 0,
    PLAT_POWER_BOOT_TIMER,
    PLAT_POWER_BOOT_EXTERNAL
} plat_power_boot_t;

plat_power_wake_t PlatformPowerLightSleep(uint64_t timerUs);
/* Returns only on a platform that cannot sleep deeply. */
void PlatformPowerDeepSleep(uint64_t timerUs);
/* Why this boot happened: a cold start, or a wake from deep sleep. */
plat_power_boot_t PlatformPowerBootCause(void);
bool PlatformPowerStateStore(const void *buf, size_t len);
bool PlatformPowerStateLoad(void *buf, size_t len, uint64_t *elapsedUs);

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
/* Bounded: a destination that never completes the handshake fails with
 * -ETIMEDOUT rather than blocking past whatever the TCP stack's own retransmit
 * schedule happens to be. */
int PlatformNetConnect(struct netCtx *ctx, const char *hostname, uint16_t port);
int PlatformNetClose(struct netCtx *ctx);
int PlatformNetRecv(struct netCtx *ctx, void *buf, size_t nbyte, int flags);
int PlatformNetSend(struct netCtx *ctx, const void *buf, size_t nbyte,
                    int flags);
int PlatformNetShutdown(struct netCtx *ctx, int how);
int PlatformNetFree(struct netCtx *ctx);

/* Local endpoint of an established connection, formatted "<addr>:<port>" into
 * `buf`. Returns the byte count (excluding NUL) or a negative errno —
 * -ENOTCONN when `ctx` has no live connection to report, -ENOTSUP for a
 * transport with no comparable address (serial, AF_UNIX). */
int PlatformNetLocalAddr(struct netCtx *ctx, char *buf, size_t buflen);

/* Bind `ctx` to <bindAddr>:<port> and, on a stream transport, listen `backlog`
 * deep. A datagram transport is left bound: Recv reads it and Send answers the
 * peer the last datagram came from. */
int PlatformNetListen(struct netCtx *ctx, const char *bindAddr, uint16_t port,
                      int backlog);

/* Take one connection off a listening context's queue into *out, a context of
 * its own that the caller closes and frees. */
int PlatformNetAccept(struct netCtx *ctx, struct netCtx **out);

/* Wait until `ctx` is readable or `wakeFd` is raised. Answers 0 once the
 * call below would not block, -EINTR on a raised wake, -EAGAIN past the
 * deadline. A negative `timeout_ns` waits without one; 0 only tests. */
int PlatformNetWaitReadable(struct netCtx *ctx, int wakeFd, int64_t timeout_ns);

/* Readiness of `ctx` now, without blocking: readable counts data buffered by
 * TLS and a pending accept; hangup is a stream peer that closed. */
int PlatformNetPoll(struct netCtx *ctx, bool *readable, bool *writable,
                    bool *hangup);

/* Readiness of a host descriptor now, without blocking. A descriptor the host
 * cannot watch reads as both readable and writable. */
void PlatformFdReady(int fd, bool *readable, bool *writable);

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
    /* Build-time digest of the staged image, lowercase hex; empty when
     * nothing is staged. Matches PlatformFirmwareDigest's report once
     * that image boots, confirming to a control plane that it took. */
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
