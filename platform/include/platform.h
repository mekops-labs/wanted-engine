#include <stdint.h>
#include <stddef.h>

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

int PlatformClockGetRes(plat_clk_id_t clk_id, uint64_t *resolution);
int PlatformClockGetTime(plat_clk_id_t clk_id, plat_timestamp_t *time);
int PlatformClockNanoSleep(plat_clk_id_t clk_id, plat_timestamp_t timeout, plat_clk_flags_t flags);
int64_t PlatfromGetRandom(uint8_t *buf, size_t buf_len);

int VfsPlatformFsInit(vfs_driver_t *driver);
void VfsPlatformFsDestroy(vfs_driver_t *driver);

int VfsPlatformRegistryInit(vfs_driver_t *driver);
void VfsPlatformRegistryDestroy(vfs_driver_t *driver);

int LoadWapp(const char *name, wapp_t * wapp);
int StartWapp(wapp_t *app);
void WaitForWapps();
