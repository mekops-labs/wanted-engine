/*
 * ESP-IDF entry point for the WANTED engine.
 *
 * Exercises the ESP-IDF platform core primitives (name, memory stats, RNG,
 * monotonic clock + sleep, mutex) and, once LittleFS is mounted, the platform
 * VFS driver (open/write/read/seek/fstat/mkdir/readdir/rename) on-target and
 * logs a pass/fail line each. Driving the full engine (WantedStart) waits on
 * the registry and wapp layers.
 */

#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_netif.h"

#include <platform.h>
#include <vfs-drivers.h>
#include <vfs.h>

#define TAG "wanted"
#define LITTLEFS_PARTITION_LABEL "registry"
#define SELFTEST_DIR "/data/selftest"

static void selftest(void) {
    ESP_LOGI(TAG, "platform: %s", PlatformName());

    size_t used = 0, total = 0;
    PlatformMemoryStats(&used, &total);
    ESP_LOGI(TAG, "memory: used=%u total=%u bytes", (unsigned)used,
             (unsigned)total);

    uint8_t a[16] = {0}, b[16] = {0};
    int64_t r1 = PlatfromGetRandom(a, sizeof(a));
    int64_t r2 = PlatfromGetRandom(b, sizeof(b));
    bool rng_ok = (r1 == 0 && r2 == 0 && memcmp(a, b, sizeof(a)) != 0);
    ESP_LOGI(TAG, "rng: %s (rc=%" PRId64 ")", rng_ok ? "OK" : "FAIL", r1);

    plat_timestamp_t t1 = 0, t2 = 0;
    PlatformClockGetTime(PLAT_CLOCKID_MONOTONIC, &t1);
    PlatformClockNanoSleep(PLAT_CLOCKID_MONOTONIC, 10000000ULL, 0); /* 10 ms */
    PlatformClockGetTime(PLAT_CLOCKID_MONOTONIC, &t2);
    ESP_LOGI(TAG, "clock: %s (dt=%" PRIu64 " ns)", (t2 > t1) ? "OK" : "FAIL",
             (uint64_t)(t2 - t1));

    platform_mutex_t *m = PlatformMutexNew();
    if (m) {
        PlatformMutexLock(m);
        PlatformMutexUnlock(m);
        PlatformMutexFree(m);
    }
    ESP_LOGI(TAG, "mutex: %s", m ? "OK" : "FAIL");
}

static bool mountLittleFs(void) {
    esp_vfs_littlefs_conf_t conf = {
        .base_path = PlatformVolumeRoot(),
        .partition_label = LITTLEFS_PARTITION_LABEL,
        .format_if_mount_failed = true,
        .dont_mount = false,
    };
    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "littlefs mount failed: %s", esp_err_to_name(err));
        return false;
    }
    size_t total = 0, used = 0;
    esp_littlefs_info(LITTLEFS_PARTITION_LABEL, &total, &used);
    ESP_LOGI(TAG, "littlefs mounted at %s: used=%u total=%u bytes",
             PlatformVolumeRoot(), (unsigned)used, (unsigned)total);
    return true;
}

/* Exercises the platform VFS driver (vfs-esp-idf.c) directly against the
 * mounted LittleFS partition: open/write/read/seek/fstat, mkdir, readdir, and
 * rename, plus PlatformOpenStateDir as the preopen root the driver's OpenAt
 * resolves against — the same path a wapp's WASI preopen takes. */
static void fsSelftest(void) {
    bool ok = true;

    int rootFd = PlatformOpenStateDir(SELFTEST_DIR, false);
    if (rootFd < 0) {
        ESP_LOGE(TAG, "fs: PlatformOpenStateDir failed: %d", rootFd);
        ESP_LOGI(TAG, "fs: FAIL");
        return;
    }

    vfs_driver_t *drv = VfsPlatformFsInit(NULL, SELFTEST_DIR, false);
    if (drv == NULL) {
        ESP_LOGE(TAG, "fs: VfsPlatformFsInit failed");
        ESP_LOGI(TAG, "fs: FAIL");
        return;
    }

    static const char payload[] = "hello esp-idf vfs\n";
    int fileFd = drv->OpenAt(drv->ctx, rootFd, "hello.txt",
                             VFS_O_CREAT | VFS_O_RDWR | VFS_O_TRUNC);
    ESP_LOGI(TAG, "fs: open(hello.txt) -> %d", fileFd);
    if (fileFd < 0) {
        ok = false;
    } else {
        int wrote = drv->Write(drv->ctx, fileFd, payload, sizeof(payload) - 1);
        ESP_LOGI(TAG, "fs: write -> %d (want %d)", wrote,
                 (int)sizeof(payload) - 1);
        ok = ok && (wrote == (int)sizeof(payload) - 1);

        long pos = -1;
        int rc = drv->Seek(drv->ctx, fileFd, 0, VFS_SEEK_SET, &pos);
        ESP_LOGI(TAG, "fs: seek -> rc=%d pos=%ld", rc, pos);
        ok = ok && (rc == 0) && (pos == 0);

        char readBuf[64] = {0};
        int red = drv->Read(drv->ctx, fileFd, readBuf, sizeof(readBuf));
        ESP_LOGI(TAG, "fs: read -> %d (want %d) content=\"%.*s\"", red,
                 (int)sizeof(payload) - 1, red > 0 ? red : 0, readBuf);
        ok = ok && (red == (int)sizeof(payload) - 1) &&
             (memcmp(readBuf, payload, (size_t)red) == 0);

        drv->Close(drv->ctx, fileFd);

        /* esp-idf's LittleFS fstat reads the on-disk directory entry, not the
         * live handle's pending size (see vfs-esp-idf.c's _Stat comment), so
         * a size check against fileFd here would see stale/zero data. Reopen
         * to get a synced-on-close, on-disk view. */
        int checkFd = drv->OpenAt(drv->ctx, rootFd, "hello.txt", VFS_O_RDONLY);
        vfs_stat_t st = {0};
        rc = (checkFd >= 0) ? drv->Stat(drv->ctx, checkFd, &st) : checkFd;
        ESP_LOGI(TAG, "fs: stat -> rc=%d filetype=%u size=%u", rc,
                 (unsigned)st.filetype, (unsigned)st.size);
        ok = ok && (rc == 0) && (st.filetype == VFS_FILETYPE_REGULAR_FILE) &&
             (st.size == sizeof(payload) - 1);
        if (checkFd >= 0)
            drv->Close(drv->ctx, checkFd);

        ESP_LOGI(TAG, "fs: open/write/read/seek/fstat: %s",
                 ok ? "OK" : "FAIL");
    }

    int rc = drv->Mkdir(drv->ctx, rootFd, "subdir");
    ESP_LOGI(TAG, "fs: mkdir(subdir) -> %d", rc);
    ok = ok && (rc == 0);
    ESP_LOGI(TAG, "fs: mkdir: %s", (rc == 0) ? "OK" : "FAIL");

    char dirbuf[256];
    uint64_t cookie = 0;
    size_t bufUsed = 0;
    rc = drv->ReadDir(drv->ctx, rootFd, dirbuf, sizeof(dirbuf), &cookie,
                      &bufUsed);
    bool sawFile = false, sawDir = false;
    if (rc == 0) {
        size_t off = 0;
        while (off < bufUsed) {
            vfs_dirent_t *e = (vfs_dirent_t *)(dirbuf + off);
            const char *name = dirbuf + off + sizeof(*e);
            if (e->d_namlen == 9 && memcmp(name, "hello.txt", 9) == 0)
                sawFile = true;
            if (e->d_namlen == 6 && memcmp(name, "subdir", 6) == 0)
                sawDir = true;
            off += sizeof(*e) + e->d_namlen;
        }
    }
    ok = ok && (rc == 0) && sawFile && sawDir;
    ESP_LOGI(TAG, "fs: readdir: %s (saw hello.txt=%d subdir=%d)",
             (rc == 0 && sawFile && sawDir) ? "OK" : "FAIL", sawFile, sawDir);

    rc = drv->Rename(drv->ctx, rootFd, "hello.txt", rootFd, "renamed.txt");
    ok = ok && (rc == 0);
    if (rc == 0) {
        int reopened = drv->OpenAt(drv->ctx, rootFd, "renamed.txt", VFS_O_RDONLY);
        ok = ok && (reopened >= 0);
        if (reopened >= 0)
            drv->Close(drv->ctx, reopened);
        int stale = drv->OpenAt(drv->ctx, rootFd, "hello.txt", VFS_O_RDONLY);
        ok = ok && (stale == -ENOENT);
    }
    ESP_LOGI(TAG, "fs: rename: %s", (rc == 0) ? "OK" : "FAIL");

    drv->Close(drv->ctx, rootFd);
    drv->Destroy(drv);

    ESP_LOGI(TAG, "fs: %s", ok ? "OK" : "FAIL");
}

/* Exercises the flash-partition registry (registry.c + registry_flash.c)
 * directly: install a synthetic image via the PlatformRegistryWrite state
 * machine, confirm it enumerates, load it back through
 * PlatformRegistryWappLoad (esp_partition_mmap zero-copy XIP) and byte-verify
 * the mapped bytes, unload, then remove and confirm it is gone. Handing the
 * mapped bytes to WAMR for real instantiation needs a loaded wapp — M7.
 */
static void registrySelftest(void) {
    static const uint8_t payload[] =
        "esp-idf registry selftest payload -- flash-mapped XIP\n";
    bool ok = true;

    int w1 = PlatformRegistryWrite(START_WRITE, "selftest:v1", payload, 32);
    int w2 = PlatformRegistryWrite(CONTINUE_WRITE, NULL, payload + 32,
                                   sizeof(payload) - 32);
    int fin = PlatformRegistryWrite(FINISH_WRITE, NULL, NULL, 0);
    ESP_LOGI(TAG, "registry: install(selftest:v1) -> w1=%d w2=%d finish=%d", w1,
             w2, fin);
    ok = ok && (w1 == 32) && (w2 == (int)sizeof(payload) - 32) && (fin == 0);

    reg_entry_t list[8];
    int n = PlatformRegistryRead(list, 8);
    bool found = false;
    for (int i = 0; i < n && !found; i++) {
        if (strcmp(list[i].name, "selftest") == 0 &&
            strcmp(list[i].version, "v1") == 0 &&
            list[i].size == sizeof(payload)) {
            found = true;
        }
    }
    ESP_LOGI(TAG, "registry: list -> n=%d found=%d", n, found);
    ok = ok && found;

    /* wapp_t (~10 KB with this profile's launch-config arrays) is heap-, not
     * stack-, allocated — the main task's default stack does not have room
     * for it (embedded-c standards, prefer heap over large stack locals). */
    wapp_t *w = calloc(1, sizeof(*w));
    bool loaded = false;
    int loadRc = -ENOMEM;
    if (w != NULL) {
        reg_entry_t query = {.name = "selftest", .version = ""};
        loadRc = PlatformRegistryWappLoad(&query, w);
        loaded = (loadRc == 0) && (w->layer_cnt == 1) &&
                 (w->layer_lens[0] == sizeof(payload)) &&
                 (memcmp(w->layers[0], payload, sizeof(payload)) == 0);
    }
    ESP_LOGI(TAG, "registry: load(selftest) -> rc=%d len=%u match=%d", loadRc,
             w ? (unsigned)w->layer_lens[0] : 0, loaded);
    ok = ok && loaded;

    if (w != NULL) {
        if (loadRc == 0) {
            int unloadRc = PlatformWappUnload(w);
            ESP_LOGI(TAG, "registry: unload -> rc=%d", unloadRc);
            ok = ok && (unloadRc == 0);
        }
        free(w);
    }

    reg_entry_t rmEntry = {.name = "selftest", .version = "v1"};
    int rmRc = PlatformRegistryRemove(&rmEntry);
    n = PlatformRegistryRead(list, 8);
    bool gone = true;
    for (int i = 0; i < n; i++) {
        if (strcmp(list[i].name, "selftest") == 0)
            gone = false;
    }
    ESP_LOGI(TAG, "registry: remove -> rc=%d gone=%d", rmRc, gone);
    ok = ok && (rmRc == 0) && gone;

    ESP_LOGI(TAG, "registry: %s", ok ? "OK" : "FAIL");
}

/* Exercises platform/posix/socket.c (reused unmodified from the NuttX/Linux
 * platforms) to the extent possible without a live network interface: opening
 * and freeing a plain socket, and confirming a secure socket type is rejected
 * (SECURE_SOCKETS=0 here, so PlatformNetOpen's TLS branch returns NULL).
 * connect/send/recv need a routable interface — WiFi bring-up is M8.
 *
 * Requires esp_netif_init() (called once from app_main below) to have started
 * lwIP's internal tcpip thread first — plain socket() asserts
 * ("Invalid mbox") without it, even with no interface configured; the assert
 * fires inside lwIP's own socket layer before it ever needs a route.
 */
static void socketSelftest(void) {
    bool ok = true;

    void *tcp = PlatformNetOpen(VFS_SKT_TCP);
    ESP_LOGI(TAG, "socket: open(tcp) -> %s", tcp ? "OK" : "FAIL");
    ok = ok && (tcp != NULL);
    if (tcp != NULL) {
        int freeRc = PlatformNetFree(tcp);
        ESP_LOGI(TAG, "socket: free(tcp) -> rc=%d", freeRc);
        ok = ok && (freeRc == 0);
    }

    void *stcp = PlatformNetOpen(VFS_SKT_STCP);
    ESP_LOGI(TAG, "socket: open(stcp, TLS disabled) -> %s (want NULL)",
             stcp ? "non-NULL" : "NULL");
    ok = ok && (stcp == NULL);

    ESP_LOGI(TAG, "socket: %s (connect/send/recv need a live interface — M8)",
             ok ? "OK" : "FAIL");
}

void app_main(void) {
    ESP_LOGI(TAG, "WANTED engine — ESP-IDF platform bring-up");
    selftest();
    if (mountLittleFs()) {
        fsSelftest();
        registrySelftest();
    }

    /* Starts lwIP's tcpip thread; required before any socket() call. Brings
     * up no interface by itself — WiFi bring-up (esp_wifi_init() and a
     * default station netif) is M8. */
    esp_err_t netifErr = esp_netif_init();
    ESP_LOGI(TAG, "netif: init -> %s", netifErr == ESP_OK ? "OK" : "FAIL");
    if (netifErr == ESP_OK) {
        socketSelftest();
    }
    ESP_LOGI(TAG, "selftest done");
}
