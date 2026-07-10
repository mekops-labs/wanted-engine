/* SPDX-License-Identifier: Apache-2.0 */

/* ESP-IDF wapp image storage: a raw flash partition ("wapps", partitions.csv)
 * holding WAPP_IMAGE_MAX_SLOTS fixed-size, erase-sector-aligned slots. */

#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_partition.h"

#include <config-esp-idf.h>
#include <platform.h>
#include <registry-image.h>
#include <wanted-api.h>
#include <wanted.h>

static inline size_t min(size_t a, size_t b) { return (a) > (b) ? (b) : (a); }

typedef struct {
    bool isMmap;
    const esp_partition_t *part;
    size_t offset;
    size_t size;
    const void *ptr;
    esp_partition_mmap_handle_t handle;
    esp_err_t err;
} flash_map_job_t;

static pthread_mutex_t g_flashHelperLock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_flashHelperThread;
static bool g_flashHelperStarted;
static sem_t g_flashJobReady;
static sem_t g_flashJobDone;
static flash_map_job_t g_flashJob;

static void *flashHelperMain(void *arg) {
    (void)arg;
    for (;;) {
        sem_wait(&g_flashJobReady);
        if (g_flashJob.isMmap) {
            g_flashJob.err = esp_partition_mmap(
                g_flashJob.part, g_flashJob.offset, g_flashJob.size,
                ESP_PARTITION_MMAP_DATA, &g_flashJob.ptr, &g_flashJob.handle);
        } else {
            esp_partition_munmap(g_flashJob.handle);
            g_flashJob.err = ESP_OK;
        }
        sem_post(&g_flashJobDone);
    }
    return NULL;
}

/* Lazily starts the helper thread on first use. No esp_pthread_set_cfg call
 * here — that config only ever applies to the next pthread_create on the
 * calling thread, so this picks up esp-idf's plain default (internal-DRAM)
 * stack regardless of what the caller's own thread previously configured for
 * itself (e.g. a wapp worker's own PSRAM cfg, already consumed by its own
 * creation in platform/esp-idf/wapps.c). */
static bool flashHelperEnsureStarted(void) {
    if (g_flashHelperStarted)
        return true;
    if (sem_init(&g_flashJobReady, 0, 0) != 0)
        return false;
    if (sem_init(&g_flashJobDone, 0, 0) != 0)
        return false;
    if (pthread_create(&g_flashHelperThread, NULL, flashHelperMain, NULL) != 0)
        return false;
    g_flashHelperStarted = true;
    return true;
}

static esp_err_t flashHelperMmap(const esp_partition_t *part, size_t offset,
                                 size_t size, const void **ptr,
                                 esp_partition_mmap_handle_t *handle) {
    esp_err_t err;

    pthread_mutex_lock(&g_flashHelperLock);
    if (!flashHelperEnsureStarted()) {
        pthread_mutex_unlock(&g_flashHelperLock);
        return ESP_ERR_NO_MEM;
    }
    g_flashJob.isMmap = true;
    g_flashJob.part = part;
    g_flashJob.offset = offset;
    g_flashJob.size = size;
    sem_post(&g_flashJobReady);
    sem_wait(&g_flashJobDone);
    err = g_flashJob.err;
    *ptr = g_flashJob.ptr;
    *handle = g_flashJob.handle;
    pthread_mutex_unlock(&g_flashHelperLock);
    return err;
}

static void flashHelperMunmap(esp_partition_mmap_handle_t handle) {
    pthread_mutex_lock(&g_flashHelperLock);
    if (flashHelperEnsureStarted()) {
        g_flashJob.isMmap = false;
        g_flashJob.handle = handle;
        sem_post(&g_flashJobReady);
        sem_wait(&g_flashJobDone);
    }
    pthread_mutex_unlock(&g_flashHelperLock);
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

static void metaPath(char *out, size_t outLen, const char *name,
                     const char *version) {
    snprintf(out, outLen, "%s/%s%c%s%s", REGISTRY_ROOT, name,
             REGISTRY_VERSION_SEPARATOR, version, REGISTRY_EXT);
}

/* The install ref (vfs-wanted-registry.c's wapp-visible grammar) is
 * "<name>" or "<name>:<version>" — a plain ':', distinct from the on-disk
 * REGISTRY_VERSION_SEPARATOR ('@') metaPath uses for the metadata filename. */
#define INSTALL_REF_SEPARATOR ':'

static bool splitRef(const char *ref, char *name, size_t nameLen, char *version,
                     size_t versionLen) {
    const char *sep = strchr(ref, INSTALL_REF_SEPARATOR);
    size_t n = sep ? (size_t)(sep - ref) : strlen(ref);
    const char *ver = sep ? sep + 1 : "";

    if (n == 0 || n >= nameLen || strlen(ver) >= versionLen)
        return false;
    memcpy(name, ref, n);
    name[n] = '\0';
    strncpy(version, ver, versionLen - 1);
    version[versionLen - 1] = '\0';
    return true;
}

/* Mark every slot referenced by a valid registry index file as used. Creates
 * REGISTRY_ROOT if absent (fresh device), matching PlatformRegistryRead. */
static int scanUsedSlots(bool used[WAPP_IMAGE_MAX_SLOTS]) {
    DIR *dir;
    const struct dirent *de;
    char path[WAPP_REG_PATH_MAX];

    memset(used, 0, WAPP_IMAGE_MAX_SLOTS * sizeof(bool));

    dir = opendir(REGISTRY_ROOT);
    if (dir == NULL) {
        if (errno == ENOENT) {
            if (mkdir(REGISTRY_ROOT, 0755) < 0)
                return -errno;
            return 0;
        }
        return -errno;
    }

    while ((de = readdir(dir)) != NULL) {
        const char *ext = strrchr(de->d_name, '.');
        if (!ext || ext == de->d_name || strcmp(ext, REGISTRY_EXT) != 0)
            continue;

        /* de->d_name's declared width (255) exceeds WAPP_REG_PATH_MAX, but a
         * ".wapp"-suffixed name reaching here is always one of our own short
         * ones; a would-be-truncated entry is not — skip it defensively
         * rather than sizing the buffer to the dirent's theoretical worst
         * case. */
        int n =
            snprintf(path, sizeof(path), "%s/%s", REGISTRY_ROOT, de->d_name);
        if (n < 0 || (size_t)n >= sizeof(path))
            continue;
        wapp_image_meta_t meta;
        FILE *f = fopen(path, "rb");
        if (f == NULL)
            continue;
        size_t r = fread(&meta, 1, sizeof(meta), f);
        fclose(f);
        if (r == sizeof(meta) && meta.magic == WAPP_IMAGE_META_MAGIC &&
            meta.slot < WAPP_IMAGE_MAX_SLOTS)
            used[meta.slot] = true;
    }
    closedir(dir);
    return 0;
}

/* If "<name>@<version>" is already installed, its slot is reused (an
 * overwrite in place) instead of leaking the old one; -ENOENT if not found. */
static int findExistingSlot(const char *name, const char *version) {
    char path[WAPP_REG_PATH_MAX];
    wapp_image_meta_t meta;

    metaPath(path, sizeof(path), name, version);
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return -ENOENT;
    size_t r = fread(&meta, 1, sizeof(meta), f);
    fclose(f);
    if (r != sizeof(meta) || meta.magic != WAPP_IMAGE_META_MAGIC)
        return -ENOENT;
    return (int)meta.slot;
}

static int allocSlot(const bool used[WAPP_IMAGE_MAX_SLOTS]) {
    for (int i = 0; i < WAPP_IMAGE_MAX_SLOTS; i++) {
        if (!used[i])
            return i;
    }
    return -ENOSPC;
}

/* Single in-flight install (static, mirroring platform/posix/registry-
 * store.c's writer state). */
static struct {
    bool active;
    int slot;
    size_t
        partitionOffset; /* running write cursor, absolute in the partition */
    size_t written;
    char name[WAPP_MAX_NAME_LEN];
    char version[WAPP_MAX_VERSION_LEN];
} g_write;

int PlatformRegistryWrite(write_state_t s, const char *ref, const uint8_t *buf,
                          size_t nbytes) {
    const esp_partition_t *part = wappPartition();
    if (part == NULL)
        return -ENODEV;

    switch (s) {
    case START_WRITE: {
        if (buf == NULL || nbytes == 0 || ref == NULL || ref[0] == '\0')
            return -EINVAL;
        if (g_write.active)
            return -EBUSY;

        char name[WAPP_MAX_NAME_LEN], version[WAPP_MAX_VERSION_LEN];
        if (!splitRef(ref, name, sizeof(name), version, sizeof(version)))
            return -EINVAL;

        bool used[WAPP_IMAGE_MAX_SLOTS];
        int rc = scanUsedSlots(used);
        if (rc < 0)
            return rc;

        int slot = findExistingSlot(name, version);
        if (slot < 0) {
            slot = allocSlot(used);
            if (slot < 0)
                return slot;
        }

        esp_err_t err = esp_partition_erase_range(
            part, (size_t)slot * WAPP_IMAGE_SLOT_SIZE, WAPP_IMAGE_SLOT_SIZE);
        if (err != ESP_OK)
            return -EIO;

        g_write.active = true;
        g_write.slot = slot;
        g_write.partitionOffset = (size_t)slot * WAPP_IMAGE_SLOT_SIZE;
        g_write.written = 0;
        strncpy(g_write.name, name, sizeof(g_write.name) - 1);
        g_write.name[sizeof(g_write.name) - 1] = '\0';
        strncpy(g_write.version, version, sizeof(g_write.version) - 1);
        g_write.version[sizeof(g_write.version) - 1] = '\0';

        if (nbytes > WAPP_IMAGE_SLOT_SIZE) {
            g_write.active = false;
            return -ENOSPC;
        }
        err = esp_partition_write(part, g_write.partitionOffset, buf, nbytes);
        if (err != ESP_OK) {
            g_write.active = false;
            return -EIO;
        }
        g_write.partitionOffset += nbytes;
        g_write.written += nbytes;
        return (int)nbytes;
    }
    case CONTINUE_WRITE: {
        if (!g_write.active)
            return -EBADF;
        if (buf == NULL || nbytes == 0)
            return -EINVAL;
        if (g_write.written + nbytes > WAPP_IMAGE_SLOT_SIZE)
            return -ENOSPC;

        esp_err_t err =
            esp_partition_write(part, g_write.partitionOffset, buf, nbytes);
        if (err != ESP_OK)
            return -EIO;
        g_write.partitionOffset += nbytes;
        g_write.written += nbytes;
        return (int)nbytes;
    }
    case FINISH_WRITE: {
        if (!g_write.active)
            return -EBADF;

        wapp_image_meta_t meta = {
            .magic = WAPP_IMAGE_META_MAGIC,
            .slot = (uint32_t)g_write.slot,
            .size = (uint32_t)g_write.written,
        };
        char path[WAPP_REG_PATH_MAX];
        metaPath(path, sizeof(path), g_write.name, g_write.version);
        g_write.active = false;

        FILE *f = fopen(path, "wb");
        if (f == NULL)
            return -errno;
        size_t w = fwrite(&meta, 1, sizeof(meta), f);
        fclose(f);
        if (w != sizeof(meta))
            return -EIO;
        return 0;
    }
    case ABORT_WRITE:
        g_write.active = false;
        return 0;
    default:
        return -EINVAL;
    }
}

int PlatformRegistryRemove(const reg_entry_t *entry) {
    char path[WAPP_REG_PATH_MAX];

    if (entry == NULL)
        return -EINVAL;
    metaPath(path, sizeof(path), entry->name, entry->version);
    if (remove(path) != 0)
        return -errno;
    return 0;
}

/* Tracks live esp_partition_mmap()s by their returned pointer, so
 * PlatformWappUnload can find the matching handle to unmap. Sized to
 * WAPP_IMAGE_MAX_SLOTS — the same bound on concurrently loaded images as on
 * installed ones. */
static struct {
    bool used;
    const void *ptr;
    esp_partition_mmap_handle_t handle;
} g_mmapTable[WAPP_IMAGE_MAX_SLOTS];

static bool mmapTableAdd(const void *ptr, esp_partition_mmap_handle_t handle) {
    for (int i = 0; i < WAPP_IMAGE_MAX_SLOTS; i++) {
        if (!g_mmapTable[i].used) {
            g_mmapTable[i].used = true;
            g_mmapTable[i].ptr = ptr;
            g_mmapTable[i].handle = handle;
            return true;
        }
    }
    return false;
}

static bool mmapTableTake(const void *ptr, esp_partition_mmap_handle_t *out) {
    for (int i = 0; i < WAPP_IMAGE_MAX_SLOTS; i++) {
        if (g_mmapTable[i].used && g_mmapTable[i].ptr == ptr) {
            g_mmapTable[i].used = false;
            *out = g_mmapTable[i].handle;
            return true;
        }
    }
    return false;
}

int PlatformRegistryWappLoad(const reg_entry_t *entry, wapp_t *w) {
    reg_entry_t resolved;

    if (entry == NULL || w == NULL)
        return -EINVAL;

    if (!entry->version[0]) {
        reg_entry_t list[REGISTRY_MAX_ENTRIES];
        int num = PlatformRegistryRead(list, REGISTRY_MAX_ENTRIES);
        if (num < 0)
            return num;
        if ((size_t)num > REGISTRY_MAX_ENTRIES)
            num = REGISTRY_MAX_ENTRIES;

        const reg_entry_t *match = NULL;
        for (int i = 0; i < num; i++) {
            if (strncmp(list[i].name, entry->name, WAPP_MAX_NAME_LEN) == 0) {
                match = &list[i];
                break;
            }
        }
        if (match == NULL)
            return -ENOENT;
        resolved = *match;
    } else {
        resolved = *entry;
    }

    char path[WAPP_REG_PATH_MAX];
    metaPath(path, sizeof(path), resolved.name, resolved.version);
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return -ENOENT;
    wapp_image_meta_t meta;
    size_t r = fread(&meta, 1, sizeof(meta), f);
    fclose(f);
    if (r != sizeof(meta) || meta.magic != WAPP_IMAGE_META_MAGIC ||
        meta.slot >= WAPP_IMAGE_MAX_SLOTS || meta.size == 0)
        return -ENOENT;

    const esp_partition_t *part = wappPartition();
    if (part == NULL)
        return -ENODEV;

    const void *ptr;
    esp_partition_mmap_handle_t handle;
    esp_err_t err =
        flashHelperMmap(part, (size_t)meta.slot * WAPP_IMAGE_SLOT_SIZE,
                        meta.size, &ptr, &handle);
    if (err != ESP_OK)
        return -EIO;

    if (!mmapTableAdd(ptr, handle)) {
        flashHelperMunmap(handle);
        return -ENOMEM;
    }

    w->layers[0] = (uint8_t *)ptr;
    w->layer_lens[0] = meta.size;
    w->layer_cnt = 1;

    strncpy(w->image, resolved.name, WAPP_MAX_NAME_LEN - 1);
    w->image[WAPP_MAX_NAME_LEN - 1] = '\0';
    strncpy(w->version, resolved.version, WAPP_MAX_VERSION_LEN - 1);
    w->version[WAPP_MAX_VERSION_LEN - 1] = '\0';
    return 0;
}

/* Releases a PlatformRegistryWappLoad mapping. A wapp loaded via the
 * compiled-in image path (platform.c's PlatformWappLoad) has no mmap-table
 * entry, so this is a no-op for it. */
int PlatformWappUnload(const wapp_t *wapp) {
    esp_partition_mmap_handle_t handle;

    if (wapp == NULL)
        return -EINVAL;
    if (mmapTableTake(wapp->layers[0], &handle))
        flashHelperMunmap(handle);
    return 0;
}

int PlatformRegistryReadImage(const reg_entry_t *entry, uint8_t *buf,
                              size_t maxLen) {
    if (entry == NULL || buf == NULL || maxLen == 0)
        return -EINVAL;

    char path[WAPP_REG_PATH_MAX];
    metaPath(path, sizeof(path), entry->name, entry->version);
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return -errno;
    wapp_image_meta_t meta;
    size_t r = fread(&meta, 1, sizeof(meta), f);
    fclose(f);
    if (r != sizeof(meta) || meta.magic != WAPP_IMAGE_META_MAGIC)
        return -ENOENT;

    const esp_partition_t *part = wappPartition();
    if (part == NULL)
        return -ENODEV;

    size_t want = min(maxLen, (size_t)meta.size);
    esp_err_t err = esp_partition_read(
        part, (size_t)meta.slot * WAPP_IMAGE_SLOT_SIZE, buf, want);
    if (err != ESP_OK)
        return -EIO;
    return (int)want;
}
