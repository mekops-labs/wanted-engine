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
 * Returns 0 when the signature is valid, -EBADMSG when it is not, and
 * -ENOSYS when this platform build carries no Ed25519 backend. Platforms
 * with a crypto peripheral or library back this with hardware acceleration;
 * the caller only ever sees the verdict. */
int PlatformEd25519Verify(const uint8_t pubkey[PLATFORM_ED25519_KEY_LEN],
                          const uint8_t sig[PLATFORM_ED25519_SIG_LEN],
                          const uint8_t *msg, size_t msgLen);

/* Streaming SHA-256 (FIPS 180-4), offloaded to hardware where available.
 * Unlike PlatformEd25519Verify there is no -ENOSYS path: /dev/sha256
 * (src/vfs/vfs-sha256.c) has no other digest source, so a platform with no
 * crypto peripheral runs a portable software implementation
 * (platform/posix/sha256.c) instead of declining the request — every
 * platform must provide a real backend. New returns NULL only on
 * allocation failure. */
#define PLATFORM_SHA256_DIGEST_LEN 32

void *PlatformSha256New(void);
void PlatformSha256Update(void *ctx, const uint8_t *data, size_t len);
void PlatformSha256Final(void *ctx, uint8_t out[PLATFORM_SHA256_DIGEST_LEN]);
void PlatformSha256Free(void *ctx);

/* Opaque cross-platform mutex. src/ must not call native threading directly,
 * so shared state (e.g. the inter-wapp pipe store) guards itself through these.
 * Lock/Unlock/Free tolerate a NULL handle so callers need not special-case an
 * allocation failure from PlatformMutexNew. */
typedef struct platform_mutex_t platform_mutex_t;
platform_mutex_t *PlatformMutexNew(void);
void PlatformMutexLock(platform_mutex_t *m);
void PlatformMutexUnlock(platform_mutex_t *m);
void PlatformMutexFree(platform_mutex_t *m);

/* This platform's driver table (config name -> VfsInitFunction_t), NULL-
 * terminated; may return NULL or an empty table. Holds only the drivers this
 * target actually implements (e.g. gpio/wifi on NuttX) — a platform omits a
 * driver it cannot honour, so a launch config naming it fails cleanly rather
 * than resolving to a no-op stub. WantedInstallDriver searches the core table
 * first, then this one, so a platform cannot shadow a core driver. Ownership
 * stays with the platform (typically a static const array). */
const vfs_driver_table_t *PlatformDriverTable(void);

int PlatformWappLoad(const char *name, wapp_t *wapp);
int PlatformWappUnload(const wapp_t *wapp);
int PlatformWappStart(wapp_t *wapp);
/* Effective native C-stack size a worker thread is created with — the
 * configured CONFIG_WANTED_WASM_WORKER_STACK_SIZE after the platform's own
 * flooring (e.g. PTHREAD_STACK_MIN). Reported via /proc/wanted so the value
 * reflects what the threads actually get, not just the compile-time request. */
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

/* Free/total bytes of the store backing the registry and volumes — flash on a
 * board, a filesystem on a host. 0 when the platform cannot report it. */
void PlatformStorageStats(size_t *free_b, size_t *total_b);

/* External-RAM (PSRAM) allocator for large engine buffers — wapp image cache
 * and WAMR linear memory — so internal RAM is left for task stacks, which on
 * the ESP32 can only live in internal RAM. On targets without external RAM
 * these fall back to the ordinary heap. malloc/realloc/free-compatible. */
void *PlatformExtramMalloc(size_t size);
void *PlatformExtramRealloc(void *ptr, size_t size);
void PlatformExtramFree(void *ptr);

/* Force the extram pool to be carved out now, before any other allocation can
 * fragment the heap region it needs a large contiguous block from. A no-op
 * after the first call (idempotent, same as the lazy path). Call as early as
 * possible in boot on targets where PSRAM shares a merged heap with internal
 * RAM (see platform/nuttx/api/extram.c); harmless (and unnecessary) elsewhere
 * since PlatformExtramMalloc/Realloc already lazy-init on first use. */
void PlatformExtramEarlyInit(void);

/* Short identifier for the target the engine was built against ("linux",
 * "nuttx", "dummy"). Static storage; the caller must not free it. Exposed at
 * /proc/wanted so a wapp can read which platform hosts it. */
const char *PlatformName(void);

/* Hex digits in a firmware digest (SHA-256). A buffer holding one needs a
 * further byte for the terminator. */
#define FIRMWARE_DIGEST_HEX_LEN 64

/* Lowercase-hex digest of the running firmware image, NUL-terminated, written
 * into `buf` and reported at /proc/wanted. The digest is stamped into the image
 * at build time, so it distinguishes two builds of one source tree where a
 * version string cannot: a control plane confirming a firmware update compares
 * this. Returns the length written, -ENOSYS where the platform stamps no
 * digest, or -ENOSPC when `bufLen` is too small. */
int PlatformFirmwareDigest(char *buf, size_t bufLen);

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
/* Read up to maxLen leading bytes of `entry`'s stored image (the .wapp archive)
 * into buf, without loading/mapping the layers. Returns the byte count, or a
 * negative errno. Used for a cheap pre-flight peek of the image header (e.g.
 * the wasm memory section) when rendering a registry descriptor. */
int PlatformRegistryReadImage(const reg_entry_t *entry, uint8_t *buf,
                              size_t maxLen);

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

/* Thin wrappers over native fs primitives, used by VFS path_rename,
 * path_create_directory and path_remove_directory to operate on preopen-rooted
 * directories. Both fds are native (openat-class) directory descriptors. */
int PlatformFsRename(int old_fd, const char *old_path, int new_fd,
                     const char *new_path);
int PlatformFsMkdir(int fd, const char *path);
int PlatformFsRmdir(int fd, const char *path);

/* GPIO backing for the core /dev/gpio driver (src/vfs/vfs-gpio.c).
 *
 * The driver owns the wapp-facing tree, the grant grammar, and the name-to-line
 * mapping; the platform owns only the line itself. `address` is the grant's
 * middle field and is interpreted here and nowhere else: a decimal pin number
 * on ESP-IDF, a host character-device path such as "/dev/gpio0" on NuttX. A
 * wapp never sees it.
 *
 * PlatformGpioOpen configures the line and returns a handle. It returns
 * -ENOSYS where the target drives no GPIO, -EINVAL on an address this backing
 * cannot parse, and -ENOTSUP for a pull or drive mode it cannot honour. The
 * driver turns any of these into a failed launch, so a wapp never sees a
 * configuration the board did not apply. */
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
} plat_gpio_cfg_t;

int PlatformGpioOpen(const plat_gpio_cfg_t *cfg, platform_gpio_t **out);
int PlatformGpioRead(const platform_gpio_t *g, bool *level);
int PlatformGpioWrite(platform_gpio_t *g, bool level);
void PlatformGpioClose(platform_gpio_t *g);

/* UART backing for the core /dev/uart driver (src/vfs/vfs-uart.c).
 *
 * The driver owns the wapp-facing tree, the grant grammar, the blocking
 * policy, and the line-setting text format. The platform owns the port.
 *
 * `port` is the grant's port= value and names the port to the backing: a UART
 * number on ESP-IDF. `options` carries every grant key the driver did not
 * consume, comma separated and in the order written — "tx=1,rx=2" on ESP-IDF,
 * "dev=/dev/ttyUSB0" on Linux. Reject an unknown key rather than ignoring it,
 * so a grant meaningless on this target fails the launch.
 *
 * The backing must make the port exclusive. ESP-IDF gets this for free, since
 * uart_driver_install refuses a port already installed. Linux does not: two
 * opens of one tty both succeed, so the backing takes TIOCEXCL itself. */
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

/* Apply a new baud rate and frame format to a live port. Drain the transmit
 * buffer first, so a reconfiguration cannot truncate a byte already on the
 * wire, then discard the receive buffer, because bytes received under the old
 * settings cannot be decoded under the new ones. Returns -EINVAL when the
 * backing cannot produce the requested rate or format. Never select the
 * nearest achievable rate — that produces a link that looks configured and
 * corrupts data. */
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

/* Bind `ctx` to <bindAddr>:<port> and, on a stream transport, start listening
 * `backlog` deep. A datagram transport is left bound: it is read with
 * PlatformNetRecv and answered with PlatformNetSend, which addresses the peer
 * the last datagram came from. */
int PlatformNetListen(struct netCtx *ctx, const char *bindAddr, uint16_t port,
                      int backlog);

/* Take one connection off a listening context's queue into *out, a context of
 * its own that the caller closes and frees. */
int PlatformNetAccept(struct netCtx *ctx, struct netCtx **out);

/* A/B firmware OTA (dual-slot bootloader-level update + rollback). Shared
 * seam across every platform that can back it with a real bootloader
 * (ESP-IDF's native esp_ota_ops A/B slots, NuttX/MCUboot's image trailer) --
 * a platform without one (Linux, dummy) provides stubs that report a single
 * always-confirmed slot and reject write attempts.
 *
 * Slots are always named 'a' (the first physical app slot -- ESP-IDF's
 * ota_0) and 'b' (the second -- ota_1), so the wapp-facing /dev/ota wire
 * text is identical across platforms regardless of which bootloader backs
 * it. */
typedef struct {
    char active_slot;      /* 'a' or 'b': the slot currently running */
    bool confirmed;        /* active_slot is marked good; no rollback armed */
    bool pending_swap;     /* the inactive slot is scheduled to run on the
                            * next boot (commit issued, reboot not yet
                            * observed) */
    char last_failed_slot; /* 'a', 'b', or '\0' if no slot has ever failed */
    int boot_attempts;     /* boot attempts recorded for active_slot */
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
/* Finalise the write: validate the image, schedule the inactive slot to run
 * on the next boot (still unconfirmed until PlatformOtaConfirm). A
 * malformed image is rejected with -EBADMSG and the boot partition is left
 * unchanged. */
int PlatformOtaCommit(void);
/* Discard a streaming write and release the session, leaving the boot
 * partition unchanged. Ends a write that must not become bootable: a session
 * begun and never committed holds the slot, and every later BeginWrite answers
 * -EBUSY until the board reboots. Idempotent -- 0 when no write is in flight.
 */
int PlatformOtaAbort(void);
/* Revert to the other slot. May reboot the board during the call rather than
 * scheduling the revert for next boot, so the caller must not assume control
 * returns. Reverts a booted image; it does not end a streaming write. */
int PlatformOtaRollback(void);

#endif /* PLATFORM_H */
