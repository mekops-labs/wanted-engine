#include <platform.h>

/* Clock, PRNG, VFS platform driver, and fs functions live in dummy-fs.c. */

int PlatformWappLoad(const char *name, wapp_t *wapp) { (void)name; (void)wapp; return 0; }
int PlatformWappUnload(const wapp_t *wapp) { (void)wapp; return 0; }
int PlatformWappStart(wapp_t *app) { (void)app; return 0; }
int PlatformWappStop(const char *name) { (void)name; return 0; }
void PlatformWappLoop() {}
int PlatformWappGetState(wapp_state_t *apps, size_t appsLen) { (void)apps; (void)appsLen; return 0; }

int PlatformRegistryRead(reg_entry_t *registryList, size_t len) { (void)registryList; (void)len; return 0; }
int PlatformRegistryWrite(write_state_t s, const uint8_t *buf, size_t nbytes) {
    (void)s; (void)buf; (void)nbytes; return 0;
}
int PlatformRegistryRemove(const reg_entry_t *entry) { (void)entry; return 0; }
int PlatformRegistryWappLoad(const reg_entry_t *entry, wapp_t *w) { (void)entry; (void)w; return 0; }

void *PlatformNetOpen(int socket_type) { (void)socket_type; return NULL; }
int PlatformNetConnect(void *ctx, const char *hostname, uint16_t port) {
    (void)ctx; (void)hostname; (void)port; return 0;
}
int PlatformNetClose(void *ctx) { (void)ctx; return 0; }
int PlatformNetRecv(void *ctx, void *buf, size_t nbyte, int flags) {
    (void)ctx; (void)buf; (void)nbyte; (void)flags; return 0;
}
int PlatformNetSend(void *ctx, const void *buf, size_t nbyte, int flags) {
    (void)ctx; (void)buf; (void)nbyte; (void)flags; return 0;
}
int PlatformNetAccept(void *ctx) { (void)ctx; return 0; }
int PlatformNetShutdown(void *ctx, int how) { (void)ctx; (void)how; return 0; }
int PlatformNetFree(void *ctx) { (void)ctx; return 0; }

void PlatformMemoryStats(size_t *heap_used, size_t *heap_total) {
    if (heap_used) *heap_used = 0;
    if (heap_total) *heap_total = 0;
}
