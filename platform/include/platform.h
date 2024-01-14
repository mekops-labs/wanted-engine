#include <stdint.h>
#include <stddef.h>

#include <vfs.h>
#include <wanted-api.h>

typedef uint32_t plat_clk_id_t;

#define THREAD_STACK_SIZE 8196

#define PLAT_CLOCKID_REALTIME           0U
#define PLAT_CLOCKID_MONOTONIC          1U
#define PLAT_CLOCKID_PROCESS_CPUTIME_ID 2U
#define PLAT_CLOCKID_THREAD_CPUTIME_ID  3U

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
int PlatformClockNanoSleep(plat_clk_id_t clk_id, plat_timestamp_t timeout, plat_clk_flags_t flags);
int64_t PlatfromGetRandom(uint8_t *buf, size_t buf_len);

int PlatformWappLoad(const char *name, wapp_t * wapp);
int PlatformWappUnload(const wapp_t *wapp);
int PlatformWappStart(wapp_t *wapp);
int PlatformWappStop(const char* name);
void PlatformWappLoop();
int PlatformWappGetState(wapp_state_t *apps, size_t appsLen);

int PlatformRegistryRead(reg_entry_t *registryList, size_t len);
int PlatformRegistryWrite(write_state_t s, const uint8_t *buf, size_t nbytes);
int PlatformRegistryRemove(const reg_entry_t *entry);
int PlatformRegistryWappLoad(const reg_entry_t *entry, wapp_t *w);

void *PlatformNetOpen(int socket_type);
int PlatformNetConnect(void *ctx, const char *hostname, uint16_t port);
int PlatformNetClose(void *ctx);
int PlatformNetRecv(void *ctx, void *buf, size_t nbyte, int flags);
int PlatformNetSend(void *ctx, const void *buf, size_t nbyte, int flags);
int PlatformNetAccept(void *ctx);
int PlatformNetShutdown(void *ctx, int how);
int PlatformNetFree(void *ctx);
