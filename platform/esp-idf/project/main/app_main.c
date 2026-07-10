/* ESP-IDF entry point for the WANTED engine. */

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_netif.h"

#include <platform.h>
#include <vfs-drivers.h>
#include <vfs.h>
#include <wanted.h>

#define TAG "wanted"
#define LITTLEFS_PARTITION_LABEL "persist"
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

/* Exercises the platform VFS driver. */
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

        /* esp-idf's LittleFS fstat reads on-disk directory entry. */
        int checkFd = drv->OpenAt(drv->ctx, rootFd, "hello.txt", VFS_O_RDONLY);
        vfs_stat_t st = {0};
        rc = (checkFd >= 0) ? drv->Stat(drv->ctx, checkFd, &st) : checkFd;
        ESP_LOGI(TAG, "fs: stat -> rc=%d filetype=%u size=%u", rc,
                 (unsigned)st.filetype, (unsigned)st.size);
        ok = ok && (rc == 0) && (st.filetype == VFS_FILETYPE_REGULAR_FILE) &&
             (st.size == sizeof(payload) - 1);
        if (checkFd >= 0)
            drv->Close(drv->ctx, checkFd);

        ESP_LOGI(TAG, "fs: open/write/read/seek/fstat: %s", ok ? "OK" : "FAIL");
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
            const vfs_dirent_t *e = (const vfs_dirent_t *)(dirbuf + off);
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
        int reopened =
            drv->OpenAt(drv->ctx, rootFd, "renamed.txt", VFS_O_RDONLY);
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

/* Exercises the flash-partition registry. */
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

    /* wapp_t is heap-allocated. */
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

/* Exercises socket layer without a live network interface.
 * Requires esp_netif_init() to have started lwIP's tcpip thread. */
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

    const void *stcp = PlatformNetOpen(VFS_SKT_STCP);
    ESP_LOGI(TAG, "socket: open(stcp, TLS disabled) -> %s (want NULL)",
             stcp ? "non-NULL" : "NULL");
    ok = ok && (stcp == NULL);

    ESP_LOGI(TAG, "socket: %s (connect/send/recv need a live interface)",
             ok ? "OK" : "FAIL");
}

/* Proves PlatformRegistryWrite is safe to call while a wapp runs from PSRAM. */
#define CONCURRENT_INSTALL_ROUNDS 40
#define CONCURRENT_INSTALL_DELAY_US 500000

static void *concurrentInstallSelftest(void *arg) {
    (void)arg;
    static const uint8_t payload[] =
        "concurrent-install payload -- proves erase+program is safe "
        "while a wapp runs from PSRAM on the other core\n";
    int pass = 0, fail = 0;

    for (int i = 0; i < CONCURRENT_INSTALL_ROUNDS; i++) {
        bool ok = true;

        int w1 = PlatformRegistryWrite(START_WRITE, "testconcurrent:v1",
                                       payload, 32);
        int w2 = PlatformRegistryWrite(CONTINUE_WRITE, NULL, payload + 32,
                                       sizeof(payload) - 32);
        int fin = PlatformRegistryWrite(FINISH_WRITE, NULL, NULL, 0);
        ok =
            ok && (w1 == 32) && (w2 == (int)sizeof(payload) - 32) && (fin == 0);

        wapp_t *w = calloc(1, sizeof(*w));
        int loadRc = -ENOMEM;
        if (w != NULL) {
            reg_entry_t query = {.name = "testconcurrent", .version = ""};
            loadRc = PlatformRegistryWappLoad(&query, w);
            ok = ok && (loadRc == 0) && (w->layer_cnt == 1) &&
                 (w->layer_lens[0] == sizeof(payload)) &&
                 (memcmp(w->layers[0], payload, sizeof(payload)) == 0);
            if (loadRc == 0)
                PlatformWappUnload(w);
            free(w);
        } else {
            ok = false;
        }

        reg_entry_t rmEntry = {.name = "testconcurrent", .version = "v1"};
        int rmRc = PlatformRegistryRemove(&rmEntry);
        ok = ok && (rmRc == 0);

        if (ok)
            pass++;
        else
            fail++;
        ESP_LOGI(TAG,
                 "ci: round %d/%d -> %s (w1=%d w2=%d fin=%d load=%d rm=%d)",
                 i + 1, CONCURRENT_INSTALL_ROUNDS, ok ? "OK" : "FAIL", w1, w2,
                 fin, loadRc, rmRc);

        usleep(CONCURRENT_INSTALL_DELAY_US);
    }

    ESP_LOGI(TAG, "ci: concurrent-install selftest done: pass=%d fail=%d", pass,
             fail);
    return NULL;
}

/* Smoke-test fixtures linked via EMBED_FILES. */
extern const uint8_t _binary_looper_wapp_start[];
extern const uint8_t _binary_looper_wapp_end[];
extern const uint8_t _binary_wifi_connect_wapp_start[];
extern const uint8_t _binary_wifi_connect_wapp_end[];
extern const uint8_t _binary_devcheck_wapp_start[];
extern const uint8_t _binary_devcheck_wapp_end[];

/* Factory-seeds a wapp into the flash registry. Safe to repeat every boot. */
static void seedWapp(const char *name, const uint8_t *start,
                     const uint8_t *end) {
    size_t len = (size_t)(end - start);
    int w = PlatformRegistryWrite(START_WRITE, name, start, len);
    int fin = PlatformRegistryWrite(FINISH_WRITE, NULL, NULL, 0);
    ESP_LOGI(TAG, "seed: %s (%u bytes) -> write=%d finish=%d", name,
             (unsigned)len, w, fin);
}

/* Supervisor config matches configs/example_config_wsh.json minus imagePath
 * (defaults to SUPERVISOR_IMAGE_PATH, the embedded wsh tar).
 * Sets console to blocking USB-Serial/JTAG mode. */
static void consoleUseBlockingDriver(void) {
    usb_serial_jtag_driver_config_t cfg =
        USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    esp_err_t err = usb_serial_jtag_driver_install(&cfg);
    ESP_LOGI(TAG, "console: usb_serial_jtag_driver_install -> %s",
             err == ESP_OK ? "OK" : esp_err_to_name(err));
    usb_serial_jtag_vfs_use_driver();
}

/* Sockets entries grant /net/s (TCP) and /net/st (TLS) to Cloudflare
 * (1.1.1.1:80/443) — a DNS-independent round-trip target. Log mount at
 * /logs for reading wapp stdout/stderr. */
#define WANTED_DEFAULT_CFG                                                     \
    "{\"system\":{\"privileged\":true},"                                       \
    "\"supervisor\":{\"params\":{"                                             \
    "\"console\":{\"in\":{\"name\":\"platform\"},"                             \
    "\"out\":{\"name\":\"platform\"},"                                         \
    "\"err\":{\"name\":\"platform\"}},"                                        \
    "\"drivers\":[{\"name\":\"wanted\"},{\"name\":\"ota\"}],"                  \
    "\"mounts\":[{\"name\":\"log\",\"path\":\"/logs\"}],"                      \
    "\"sockets\":[{\"name\":\"s\",\"address\":\"tcp://1.1.1.1:80\"},"          \
    "{\"name\":\"st\",\"address\":\"tcps://1.1.1.1:443\"}]}}}"

void app_main(void) {
    ESP_LOGI(TAG, "WANTED engine — ESP-IDF platform bring-up");
    selftest();
    if (mountLittleFs()) {
        fsSelftest();
        registrySelftest();
        seedWapp("looper", _binary_looper_wapp_start, _binary_looper_wapp_end);
        seedWapp("wifi-connect", _binary_wifi_connect_wapp_start,
                 _binary_wifi_connect_wapp_end);
        seedWapp("devcheck", _binary_devcheck_wapp_start,
                 _binary_devcheck_wapp_end);
    }

    /* Starts lwIP's tcpip thread; required before any socket() call. */
    esp_err_t netifErr = esp_netif_init();
    ESP_LOGI(TAG, "netif: init -> %s", netifErr == ESP_OK ? "OK" : "FAIL");
    if (netifErr == ESP_OK) {
        socketSelftest();
    }
    ESP_LOGI(TAG, "selftest done");

    pthread_t ciThread;
    if (pthread_create(&ciThread, NULL, concurrentInstallSelftest, NULL) == 0) {
        pthread_detach(ciThread);
        ESP_LOGI(TAG, "ci: concurrent-install selftest thread started");
    } else {
        ESP_LOGE(TAG, "ci: concurrent-install selftest thread failed to start");
    }

    int otaRc = PlatformOtaInit();
    ESP_LOGI(TAG, "ota: init -> rc=%d", otaRc);

    PlatformSetProcessArgs(0, NULL);
    consoleUseBlockingDriver();
    ESP_LOGI(TAG, "starting WANTED engine (supervisor: wsh, privileged)");
    int ret = WantedStart(WANTED_DEFAULT_CFG, strlen(WANTED_DEFAULT_CFG));
    ESP_LOGI(TAG, "WantedStart returned %d", ret);
}
