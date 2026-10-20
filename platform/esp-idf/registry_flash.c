/* SPDX-License-Identifier: Apache-2.0 */

/* ESP-IDF registry store: the page store over the raw "wapps" partition, in
 * units of one MMU page, with its records in LittleFS. An image that the old
 * fixed-slot layout placed is adopted where it lies. */

#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_heap_caps.h"
#include "esp_partition.h"
#include "esp_pthread.h"
#include "spi_flash_mmap.h"

#include <config-esp-idf.h>
#include <platform.h>
#include <registry-page.h>
#include <registry.h>
#include <wanted_log.h>

#define UNIT_SIZE SPI_FLASH_MMU_PAGE_SIZE
#define ENTRY_DIR REGISTRY_ROOT "/entries"
#define BLOB_DIR REGISTRY_ROOT "/pages"
#define OLD_STAGE REGISTRY_ROOT "/_meta"
#define OLD_PATH_MAX 96
#define OLD_NAME_MAX 48
#define OLD_BATCH 8
#define OLD_SLOT_TAIL 8
#define MAP_MAX_PAGES 128
/* Every converted image shares one sequence: the old layout kept no install
 * order, and a bare name took the first ref in sort order. */
#define OLD_SEQ 1
#define HELPER_STACK_BYTES 4096

typedef enum {
    JOB_MAP_RANGE,
    JOB_MAP_PAGES,
    JOB_UNMAP,
} job_kind_t;

/* Lives in internal RAM: spi_flash_mmap_pages needs its page list there, and a
 * worker stack may be in PSRAM. */
typedef struct {
    job_kind_t kind;
    const esp_partition_t *part;
    size_t offset;
    size_t size;
    registry_extent_t ext[REGISTRY_PAGE_MAX_EXTENTS];
    size_t nExt;
    int pages[MAP_MAX_PAGES];
    const void *ptr;
    spi_flash_mmap_handle_t handle;
    esp_err_t err;
} flash_job_t;

static pthread_mutex_t g_flashHelperLock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_flashHelperThread;
static bool g_flashHelperStarted;
static sem_t g_flashJobReady;
static sem_t g_flashJobDone;
static flash_job_t g_flashJob;

static esp_err_t mapPages(flash_job_t *job) {
    size_t n = 0;

    for (size_t i = 0; i < job->nExt; i++) {
        size_t first = (job->part->address + job->ext[i].offset) / UNIT_SIZE;
        size_t count = (job->ext[i].length + UNIT_SIZE - 1) / UNIT_SIZE;

        for (size_t p = 0; p < count; p++) {
            if (n >= MAP_MAX_PAGES)
                return ESP_ERR_NO_MEM;
            job->pages[n++] = (int)(first + p);
        }
    }
    return spi_flash_mmap_pages(job->pages, n, SPI_FLASH_MMAP_DATA, &job->ptr,
                                &job->handle);
}

static void *flashHelperMain(void *arg) {
    (void)arg;
    for (;;) {
        sem_wait(&g_flashJobReady);
        switch (g_flashJob.kind) {
        case JOB_MAP_RANGE:
            g_flashJob.err = esp_partition_mmap(
                g_flashJob.part, g_flashJob.offset, g_flashJob.size,
                ESP_PARTITION_MMAP_DATA, &g_flashJob.ptr, &g_flashJob.handle);
            break;
        case JOB_MAP_PAGES:
            g_flashJob.err = mapPages(&g_flashJob);
            break;
        case JOB_UNMAP:
            spi_flash_munmap(g_flashJob.handle);
            g_flashJob.err = ESP_OK;
            break;
        }
        sem_post(&g_flashJobDone);
    }
    return NULL;
}

/* Starts the helper thread on first use. Its stack must be internal RAM: the
 * mapping runs with the flash cache frozen. A creating thread's own pthread
 * config would otherwise pass on, so the config is set here and put back. */
static bool flashHelperEnsureStarted(void) {
    esp_pthread_cfg_t prev;
    esp_pthread_cfg_t cfg = esp_pthread_get_default_config();
    bool hadPrev = esp_pthread_get_cfg(&prev) == ESP_OK;
    int rc;

    if (g_flashHelperStarted)
        return true;
    if (sem_init(&g_flashJobReady, 0, 0) != 0)
        return false;
    if (sem_init(&g_flashJobDone, 0, 0) != 0)
        return false;
    cfg.thread_name = "regflash";
    cfg.stack_size = HELPER_STACK_BYTES;
    cfg.stack_alloc_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    esp_pthread_set_cfg(&cfg);
    rc = pthread_create(&g_flashHelperThread, NULL, flashHelperMain, NULL);
    if (hadPrev)
        esp_pthread_set_cfg(&prev);
    if (rc != 0)
        return false;
    g_flashHelperStarted = true;
    return true;
}

static const esp_partition_t *wappPartition(void) {
    static const esp_partition_t *part;

    if (part == NULL) {
        part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                        ESP_PARTITION_SUBTYPE_ANY,
                                        WAPP_IMAGE_PARTITION_LABEL);
    }
    return part;
}

static int flashErase(void *ctx, uint32_t unit) {
    (void)ctx;
    return esp_partition_erase_range(wappPartition(), (size_t)unit * UNIT_SIZE,
                                     UNIT_SIZE) == ESP_OK
               ? 0
               : -EIO;
}

static int flashWrite(void *ctx, uint32_t offset, const uint8_t *buf,
                      size_t n) {
    (void)ctx;
    return esp_partition_write(wappPartition(), offset, buf, n) == ESP_OK
               ? 0
               : -EIO;
}

/* One extent maps as a range, at any offset; several map as the pages they
 * cover, joined in order into one range. */
static int flashMap(void *ctx, const registry_extent_t *ext, size_t nExt,
                    size_t len, const uint8_t **ptr, void **handle) {
    esp_err_t err;

    (void)ctx;
    pthread_mutex_lock(&g_flashHelperLock);
    if (!flashHelperEnsureStarted()) {
        pthread_mutex_unlock(&g_flashHelperLock);
        return -ENOMEM;
    }
    g_flashJob.part = wappPartition();
    if (nExt == 1) {
        g_flashJob.kind = JOB_MAP_RANGE;
        g_flashJob.offset = ext[0].offset;
        g_flashJob.size = len;
    } else {
        g_flashJob.kind = JOB_MAP_PAGES;
        g_flashJob.nExt = nExt;
        memcpy(g_flashJob.ext, ext, nExt * sizeof(ext[0]));
    }
    sem_post(&g_flashJobReady);
    sem_wait(&g_flashJobDone);
    err = g_flashJob.err;
    *ptr = g_flashJob.ptr;
    *handle = (void *)(uintptr_t)g_flashJob.handle;
    pthread_mutex_unlock(&g_flashHelperLock);
    if (err == ESP_ERR_NO_MEM)
        return -ENOMEM;
    return err == ESP_OK ? 0 : -EIO;
}

static void flashUnmap(void *ctx, void *handle) {
    (void)ctx;
    pthread_mutex_lock(&g_flashHelperLock);
    if (flashHelperEnsureStarted()) {
        g_flashJob.kind = JOB_UNMAP;
        g_flashJob.handle = (spi_flash_mmap_handle_t)(uintptr_t)handle;
        sem_post(&g_flashJobReady);
        sem_wait(&g_flashJobDone);
    }
    pthread_mutex_unlock(&g_flashHelperLock);
}

static const registry_flash_ops_t flashOps = {
    .erase = flashErase,
    .write = flashWrite,
    .map = flashMap,
    .unmap = flashUnmap,
};

static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

/* "<name>@<version>.wapp" into a key. */
static bool oldKey(const char *file, registry_key_t *key) {
    size_t ext = sizeof(REGISTRY_EXT) - 1;
    size_t len = strlen(file);
    const char *sep = strchr(file, REGISTRY_VERSION_SEPARATOR);
    size_t nameLen;
    size_t verLen;

    if (len <= ext || strcmp(file + len - ext, REGISTRY_EXT) != 0 ||
        sep == NULL)
        return false;
    nameLen = (size_t)(sep - file);
    verLen = len - ext - nameLen - 1;
    if (nameLen == 0 || nameLen >= REGISTRY_KEY_NAME_LEN ||
        verLen >= REGISTRY_KEY_NAME_LEN)
        return false;
    memset(key, 0, sizeof(*key));
    memcpy(key->name, file, nameLen);
    memcpy(key->version, sep + 1, verLen);
    return true;
}

/* Converts one index file of the fixed-slot layout: a record, then the slot
 * and the stride it was placed under. Returns true once the file is settled,
 * so it can go: converted, dropped as a seed, or not a usable record. */
static bool convertOld(registry_store_t *s, const char *file, uint32_t seq) {
    char path[OLD_PATH_MAX];
    uint8_t buf[REGISTRY_RECORD_MAX];
    uint8_t rec[REGISTRY_RECORD_MAX];
    registry_meta_t meta;
    registry_key_t key;
    const esp_partition_t *part = wappPartition();
    FILE *f;
    size_t n;
    uint64_t offset;
    size_t recLen;

    if (!oldKey(file, &key))
        return true;
    snprintf(path, sizeof(path), "%s/%s", REGISTRY_ROOT, file);
    f = fopen(path, "rb");
    if (f == NULL)
        return false;
    n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    if (n <= OLD_SLOT_TAIL ||
        !RegistryRecordDecode(buf, n - OLD_SLOT_TAIL, &meta))
        return true;
    if ((meta.flags & REGISTRY_META_SEEDED) || meta.size == 0)
        return true;

    offset = (uint64_t)get32(buf + n - OLD_SLOT_TAIL) *
             get32(buf + n - OLD_SLOT_TAIL + 4);
    if (part == NULL || offset + meta.size > part->size)
        return true;

    if (RegistryPageAdopt(s, meta.layerDigest[0], (uint32_t)offset, meta.size) <
        0)
        return false;
    meta.installSeq = seq;
    recLen = RegistryRecordEncode(&meta, rec, sizeof(rec));
    return recLen > 0 && s->ops->recPut(s, &key, rec, recLen) == 0;
}

/* Converts every old index file and removes it once its records are written,
 * so a reset restarts the conversion where it stopped. */
static void convertAll(registry_store_t *s) {
    size_t settled;

    remove(OLD_STAGE);
    do {
        DIR *dir = opendir(REGISTRY_ROOT);
        const struct dirent *de;
        char names[OLD_BATCH][OLD_NAME_MAX];
        size_t n = 0;

        settled = 0;
        if (dir == NULL)
            return;
        while ((de = readdir(dir)) != NULL && n < OLD_BATCH) {
            registry_key_t key;

            if (oldKey(de->d_name, &key) &&
                strlen(de->d_name) < sizeof(names[0]))
                strcpy(names[n++], de->d_name);
        }
        closedir(dir);
        for (size_t i = 0; i < n; i++) {
            char path[OLD_PATH_MAX];

            if (!convertOld(s, names[i], OLD_SEQ))
                continue;
            snprintf(path, sizeof(path), "%s/%s", REGISTRY_ROOT, names[i]);
            if (remove(path) == 0)
                settled++;
        }
    } while (settled > 0);
}

static registry_page_config_t g_cfg = {
    .name = "wapps",
    .entryDir = ENTRY_DIR,
    .blobDir = BLOB_DIR,
    .unit = UNIT_SIZE,
    .scattered = true,
    .flash = &flashOps,
};
static registry_store_ops_t g_ops;
static registry_store_t g_store;
static int (*g_pageInit)(registry_store_t *s);

static int storeInit(registry_store_t *s) {
    int rc = g_pageInit(s);

    convertAll(s);
    return rc;
}

size_t PlatformRegistryStores(registry_store_t **out, size_t max) {
    const esp_partition_t *part = wappPartition();
    registry_store_t *page;

    if (max == 0 || part == NULL)
        return 0;
    g_cfg.units = part->size / UNIT_SIZE;
    if (g_cfg.units > REGISTRY_PAGE_MAX_UNITS)
        g_cfg.units = REGISTRY_PAGE_MAX_UNITS;
    /* Pages map by absolute page number, so a partition off a page boundary
     * keeps each blob in one run, which maps at any offset. */
    g_cfg.scattered = part->address % UNIT_SIZE == 0;
    if (!g_cfg.scattered)
        LOG_ERROR("wapps partition at 0x%x is off a page; blobs stay whole",
                  (unsigned)part->address);
    page = RegistryPageStore(&g_cfg);
    g_ops = *page->ops;
    g_pageInit = page->ops->init;
    g_ops.init = storeInit;
    g_store.ops = &g_ops;
    out[0] = &g_store;
    return 1;
}

int PlatformWappUnload(const wapp_t *wapp) {
    if (wapp == NULL)
        return -EINVAL;
    RegistryUnload(wapp);
    return 0;
}
