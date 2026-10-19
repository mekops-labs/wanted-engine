/* SPDX-License-Identifier: Apache-2.0 */

/* The registry store on raw flash, in allocation units a blob takes as it needs
 * and stays mapped from. A blob's bytes are written before its record, a small
 * file, so a reset leaves a whole blob or none. */

#ifndef REGISTRY_PAGE_H
#define REGISTRY_PAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <registry-store.h>

#define REGISTRY_PAGE_MAX_UNITS 4096
#define REGISTRY_PAGE_MAX_EXTENTS 32

typedef struct {
    uint32_t offset;
    uint32_t length;
} registry_extent_t;

/* What the platform provides over the partition. Offsets are bytes from its
 * start. A write targets erased bytes only. */
typedef struct {
    int (*erase)(void *ctx, uint32_t unit);
    int (*write)(void *ctx, uint32_t offset, const uint8_t *buf, size_t n);
    /* Maps the extents, in order, as one range of `len` bytes. A store that
     * cannot join non-adjacent extents is given one. */
    int (*map)(void *ctx, const registry_extent_t *ext, size_t nExt, size_t len,
               const uint8_t **ptr, void **handle);
    void (*unmap)(void *ctx, void *handle);
    /* Called before each record write; non-zero refuses it. May be NULL. */
    int (*barrier)(void *ctx);
} registry_flash_ops_t;

typedef struct {
    const char *name;
    const char *entryDir; /* entry records */
    const char *blobDir;  /* blob records */
    uint32_t unit;        /* allocation unit in bytes */
    uint32_t units;       /* units in the partition */
    bool scattered;       /* `map` joins non-adjacent extents */
    const registry_flash_ops_t *flash;
    void *flashCtx;
} registry_page_config_t;

/* The store for `cfg`, which must outlive it. One at a time. */
registry_store_t *RegistryPageStore(const registry_page_config_t *cfg);

/* A blob that is resident in mapped flash and holds no unit. */
typedef struct {
    uint8_t digest[REGISTRY_DIGEST_LEN];
    const uint8_t *data;
    size_t len;
} registry_resident_t;

typedef struct {
    uint32_t units;
    uint32_t freeUnits;
    uint32_t shared;     /* units named by more than one blob record */
    uint32_t outOfRange; /* blob records naming bytes past the partition */
    uint32_t mismatch;   /* units in use that the free count does not match */
} registry_page_report_t;

/* Walks every blob record and the allocation map, for tests. */
int RegistryPageCheck(registry_store_t *s, registry_page_report_t *out);

#endif /* REGISTRY_PAGE_H */
