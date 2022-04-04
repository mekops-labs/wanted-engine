#include <stdint.h>
#include <stddef.h>

#include <wanted.h>
#include <vfs.h>
#include <wanted-api.h>

typedef uint32_t plat_clk_id_t;

#define PLAT_CLOCKID_REALTIME           0U
#define PLAT_CLOCKID_MONOTONIC          1U
#define PLAT_CLOCKID_PROCESS_CPUTIME_ID 2U
#define PLAT_CLOCKID_THREAD_CPUTIME_ID  3U

typedef uint64_t plat_timestamp_t;
typedef uint16_t plat_clk_flags_t;

#define PLAT_CLOCK_FLAGS_ABSTIME 1U

typedef struct {
    char    name[WAPP_MAX_NAME_LEN];
    size_t  size;
} reg_entry_t;

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

int VfsPlatformFsInit(vfs_driver_t *driver);
void VfsPlatformFsDestroy(vfs_driver_t *driver);

int PlatformWappLoad(const char *name, wapp_t * wapp);
int PlatformWappUnload(const wapp_t *wapp);
int PlatformWappStart(wapp_t app);
void PlatformWappLoop();
int PlatformWappGetState(wapp_state_t *apps, size_t appsLen);

int PlatformRegistryRead(reg_entry_t *registryList, size_t len);
int PlatformRegistryWrite(write_state_t s, const uint8_t *buf, size_t nbytes);
