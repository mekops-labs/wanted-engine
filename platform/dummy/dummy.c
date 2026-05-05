#include <platform.h>

int PlatformClockGetRes(plat_clk_id_t clk_id, uint64_t *resolution) {
    return 0;
}
int PlatformClockGetTime(plat_clk_id_t clk_id, plat_timestamp_t *time) {
    return 0;
}
int PlatformClockNanoSleep(plat_clk_id_t clk_id, plat_timestamp_t timeout,
                           plat_clk_flags_t flags) {
    return 0;
}
int64_t PlatfromGetRandom(uint8_t *buf, size_t buf_len) { return 0; }

int PlatformWappLoad(const char *name, wapp_t *wapp) { return 0; }
int PlatformWappUnload(const wapp_t *wapp) { return 0; }
int PlatformWappStart(wapp_t *app) { return 0; }
int PlatformWappStop(const char *name) { return 0; }
void PlatformWappLoop() {}
int PlatformWappGetState(wapp_state_t *apps, size_t appsLen) { return 0; }

int PlatformRegistryRead(reg_entry_t *registryList, size_t len) { return 0; }
int PlatformRegistryWrite(write_state_t s, const uint8_t *buf, size_t nbytes) {
    return 0;
}
int PlatformRegistryRemove(const reg_entry_t *entry) { return 0; }
int PlatformRegistryWappLoad(const reg_entry_t *entry, wapp_t *w) { return 0; }

void *PlatformNetOpen(int socket_type) { return NULL; }
int PlatformNetConnect(void *ctx, const char *hostname, uint16_t port) {
    return 0;
}
int PlatformNetClose(void *ctx) { return 0; }
int PlatformNetRecv(void *ctx, void *buf, size_t nbyte, int flags) { return 0; }
int PlatformNetSend(void *ctx, const void *buf, size_t nbyte, int flags) {
    return 0;
}
int PlatformNetAccept(void *ctx) { return 0; }
int PlatformNetShutdown(void *ctx, int how) { return 0; }
int PlatformNetFree(void *ctx) { return 0; }

vfs_driver_t *VfsPlatformFsInit(const wapp_t *wapp, const char *opt) {
    return NULL;
}
void PlatformMemoryStats(size_t *heap_used, size_t *heap_total) { if (heap_used) *heap_used = 0; if (heap_total) *heap_total = 0; }
