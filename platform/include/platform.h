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

int PlatformWappLoad(const char *name, wapp_t *wapp);
int PlatformWappUnload(const wapp_t *wapp);
int PlatformWappStart(wapp_t *wapp);
int PlatformWappStop(const char *name);
void PlatformWappLoop();
int PlatformWappGetState(wapp_state_t *apps, size_t appsLen);
void PlatformMemoryStats(size_t *heap_used, size_t *heap_total);

int PlatformRegistryRead(reg_entry_t *registryList, size_t len);
int PlatformRegistryWrite(write_state_t s, const uint8_t *buf, size_t nbytes);
int PlatformRegistryRemove(const reg_entry_t *entry);
int PlatformRegistryWappLoad(const reg_entry_t *entry, wapp_t *w);

/* Open (and create-if-absent) a host-side directory that will be exposed to a
 * wapp as a WASI preopen. Returns a native fd usable with openat(2)-class
 * APIs, or a negative errno on failure. The returned fd's lifetime is owned
 * by the VFS layer (closed at VfsDestroy). */
int PlatformOpenStateDir(const char *path);

/* Thin wrappers over native fs primitives, used by VFS path_rename and
 * path_create_directory to operate on preopen-rooted directories. Both fds
 * are native (openat-class) directory descriptors. */
int PlatformFsRename(int old_fd, const char *old_path,
                     int new_fd, const char *new_path);
int PlatformFsMkdir(int fd, const char *path);

void *PlatformNetOpen(int socket_type);
int PlatformNetConnect(void *ctx, const char *hostname, uint16_t port);
int PlatformNetClose(void *ctx);
int PlatformNetRecv(void *ctx, void *buf, size_t nbyte, int flags);
int PlatformNetSend(void *ctx, const void *buf, size_t nbyte, int flags);
int PlatformNetAccept(void *ctx);
int PlatformNetShutdown(void *ctx, int how);
int PlatformNetFree(void *ctx);
