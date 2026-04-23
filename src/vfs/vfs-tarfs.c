#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <debug_trace.h>
#include <vfs-tarfs.h>
#include <wanted-api.h>
#include <wanted_malloc.h>

#define TAR_BLOCK_SIZE 512
#define TARFS_WHITEOUT 0xFFFFU

#define TAR_NAME_OFFSET 0
#define TAR_NAME_LEN 100
#define TAR_SIZE_OFFSET 124
#define TAR_SIZE_LEN 12
#define TAR_TYPEFLAG_OFFSET 156
#define TAR_PREFIX_OFFSET 345

#define INDEX_INIT_CAP 16
#define OWNED_INIT_CAP 4

typedef struct __attribute__((packed)) tar_index_entry_t {
    const char *path_ptr;
    uint16_t layer_idx;
    uint32_t hdr_offset;
    uint32_t size;
} tar_index_entry_t;

struct vfs_tarfs_ctx_t {
    const uint8_t *layers[TARFS_MAX_LAYERS];
    size_t layer_lens[TARFS_MAX_LAYERS];
    uint8_t layer_cnt;

    /* Deduplicated sorted index. Target: PSRAM on ESP.
     * TODO(psram): swap WantedMalloc -> heap_caps_malloc(MALLOC_CAP_SPIRAM)
     *              behind a platform hook once the ESP port lands. */
    tar_index_entry_t *index;
    uint16_t index_len;
    uint16_t index_cap;

    /* Heap-owned path strings (whiteout targets — non-zero-copy). */
    char **owned;
    uint16_t owned_len;
    uint16_t owned_cap;

    /* Boot optimisations: direct pointers into flash-mapped layer data. */
    const uint8_t *entrypoint_wasm;
    size_t entrypoint_wasm_len;
    const uint8_t *entrypoint_manifest;
    size_t entrypoint_manifest_len;
};

/* POSIX ustar stores numeric fields as NUL/space-terminated octal ASCII. */
static uint32_t ParseOctal(const char *s, size_t n) {
    uint32_t v = 0;
    for (size_t i = 0; i < n && s[i] >= '0' && s[i] <= '7'; i++) {
        v = (v << 3) | (uint32_t)(s[i] - '0');
    }
    return v;
}

static inline size_t AlignUp512(size_t n) {
    return (n + (TAR_BLOCK_SIZE - 1)) & ~(size_t)(TAR_BLOCK_SIZE - 1);
}

static bool IsZeroBlock(const uint8_t *b) {
    for (size_t i = 0; i < TAR_BLOCK_SIZE; i++) {
        if (b[i])
            return false;
    }
    return true;
}

static bool IndexContains(const tar_index_entry_t *idx, uint16_t n,
                          const char *path) {
    for (uint16_t i = 0; i < n; i++) {
        if (strcmp(idx[i].path_ptr, path) == 0)
            return true;
    }
    return false;
}

static int GrowIndex(vfs_tarfs_ctx_t *ctx) {
    uint16_t new_cap = ctx->index_cap ? (uint16_t)(ctx->index_cap * 2)
                                      : INDEX_INIT_CAP;
    tar_index_entry_t *next =
        WantedMalloc((size_t)new_cap * sizeof(tar_index_entry_t));
    if (!next)
        return -ENOMEM;
    if (ctx->index) {
        memcpy(next, ctx->index,
               (size_t)ctx->index_len * sizeof(tar_index_entry_t));
        WantedFree(ctx->index);
    }
    ctx->index = next;
    ctx->index_cap = new_cap;
    return 0;
}

static int RememberOwned(vfs_tarfs_ctx_t *ctx, char *s) {
    if (ctx->owned_len == ctx->owned_cap) {
        uint16_t nc = ctx->owned_cap ? (uint16_t)(ctx->owned_cap * 2)
                                     : OWNED_INIT_CAP;
        char **next = WantedMalloc((size_t)nc * sizeof(char *));
        if (!next)
            return -ENOMEM;
        if (ctx->owned) {
            memcpy(next, ctx->owned, (size_t)ctx->owned_len * sizeof(char *));
            WantedFree(ctx->owned);
        }
        ctx->owned = next;
        ctx->owned_cap = nc;
    }
    ctx->owned[ctx->owned_len++] = s;
    return 0;
}

static const char *Basename(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

/* "foo/.wh.bar" -> heap-allocated "foo/bar". Caller owns the result. */
static char *MakeWhiteoutTarget(const char *path) {
    const char *base = Basename(path);
    size_t dir_len = (size_t)(base - path);
    size_t tail_len = strlen(base + 4); /* skip ".wh." */
    char *out = WantedMalloc(dir_len + tail_len + 1);
    if (!out)
        return NULL;
    memcpy(out, path, dir_len);
    memcpy(out + dir_len, base + 4, tail_len);
    out[dir_len + tail_len] = '\0';
    return out;
}

static int IndexLayer(vfs_tarfs_ctx_t *ctx, uint8_t layer_idx) {
    const uint8_t *buf = ctx->layers[layer_idx];
    size_t len = ctx->layer_lens[layer_idx];
    size_t off = 0;
    int empty_run = 0;

    while (off + TAR_BLOCK_SIZE <= len) {
        const uint8_t *hdr = buf + off;

        /* Two consecutive zero blocks mark end of archive. */
        if (IsZeroBlock(hdr)) {
            if (++empty_run >= 2)
                break;
            off += TAR_BLOCK_SIZE;
            continue;
        }
        empty_run = 0;

        const char *name = (const char *)(hdr + TAR_NAME_OFFSET);
        size_t name_field_len = strnlen(name, TAR_NAME_LEN);
        uint32_t size =
            ParseOctal((const char *)(hdr + TAR_SIZE_OFFSET), TAR_SIZE_LEN);
        size_t next_off = off + TAR_BLOCK_SIZE + AlignUp512(size);

        /* Skip entries we can't safely zero-copy: 100-char name with no NUL,
         * or a ustar prefix field in use (would require concatenation). */
        if (name_field_len == TAR_NAME_LEN || hdr[TAR_PREFIX_OFFSET] != '\0') {
            DEBUG_TRACE(
                "tarfs: skipping long/prefixed entry in layer %u at %zu",
                layer_idx, off);
            off = next_off;
            continue;
        }

        if (name[0] == '\0') {
            off = next_off;
            continue;
        }

        uint8_t typeflag = hdr[TAR_TYPEFLAG_OFFSET];
        bool is_regular = (typeflag == '0' || typeflag == '\0');
        bool is_dir = (typeflag == '5');

        /* Links, fifos, device nodes: unsupported. Directories are implicit
         * (ReadDir uses path prefixes), so we don't index them either. */
        if (!is_regular || is_dir) {
            off = next_off;
            continue;
        }

        /* Normalise leading "./" or "/" produced by some TAR writers. */
        const char *eff_path = name;
        if (eff_path[0] == '.' && eff_path[1] == '/')
            eff_path += 2;
        else if (eff_path[0] == '/')
            eff_path += 1;

        const char *base = Basename(eff_path);
        bool is_whiteout =
            strncmp(base, ".wh.", 4) == 0 && base[4] != '\0';

        const char *entry_path;
        uint16_t entry_layer;

        if (is_whiteout) {
            char *target = MakeWhiteoutTarget(eff_path);
            if (!target)
                return -ENOMEM;
            int ret = RememberOwned(ctx, target);
            if (ret < 0) {
                WantedFree(target);
                return ret;
            }
            entry_path = target;
            entry_layer = TARFS_WHITEOUT;
        } else {
            entry_path = eff_path;
            entry_layer = layer_idx;
        }

        /* Newer-layer shadowing: first occurrence wins. */
        if (IndexContains(ctx->index, ctx->index_len, entry_path)) {
            off = next_off;
            continue;
        }

        /* Pre-fetch boot entrypoints from the topmost non-whiteout hit. */
        if (!is_whiteout) {
            size_t data_off = off + TAR_BLOCK_SIZE;
            if (strcmp(entry_path, "app.wasm") == 0 &&
                ctx->entrypoint_wasm == NULL) {
                ctx->entrypoint_wasm = buf + data_off;
                ctx->entrypoint_wasm_len = size;
            } else if (strcmp(entry_path, "manifest.json") == 0 &&
                       ctx->entrypoint_manifest == NULL) {
                ctx->entrypoint_manifest = buf + data_off;
                ctx->entrypoint_manifest_len = size;
            }
        }

        if (ctx->index_len == ctx->index_cap) {
            int ret = GrowIndex(ctx);
            if (ret < 0)
                return ret;
        }
        tar_index_entry_t *e = &ctx->index[ctx->index_len++];
        e->path_ptr = entry_path;
        e->layer_idx = entry_layer;
        e->hdr_offset = (uint32_t)off;
        e->size = size;

        off = next_off;
    }

    return 0;
}

static int IndexCmp(const void *a, const void *b) {
    const tar_index_entry_t *ea = a;
    const tar_index_entry_t *eb = b;
    return strcmp(ea->path_ptr, eb->path_ptr);
}

vfs_tarfs_ctx_t *TarFsInit(uint8_t *const layers[], const size_t layer_lens[],
                           uint8_t layer_cnt) {
    if (layers == NULL || layer_lens == NULL || layer_cnt == 0 ||
        layer_cnt > TARFS_MAX_LAYERS) {
        DEBUG_TRACE("tarfs: invalid args (cnt=%u)", layer_cnt);
        return NULL;
    }

    vfs_tarfs_ctx_t *ctx = WantedMalloc(sizeof(*ctx));
    if (!ctx) {
        DEBUG_TRACE("tarfs: alloc ctx failed");
        return NULL;
    }
    memset(ctx, 0, sizeof(*ctx));

    for (uint8_t i = 0; i < layer_cnt; i++) {
        if (layers[i] == NULL || layer_lens[i] == 0) {
            DEBUG_TRACE("tarfs: empty layer %u", i);
            TarFsDestroy(ctx);
            return NULL;
        }
        ctx->layers[i] = layers[i];
        ctx->layer_lens[i] = layer_lens[i];
    }
    ctx->layer_cnt = layer_cnt;

    for (uint8_t i = 0; i < layer_cnt; i++) {
        if (IndexLayer(ctx, i) < 0) {
            DEBUG_TRACE("tarfs: indexing failed on layer %u", i);
            TarFsDestroy(ctx);
            return NULL;
        }
    }

    if (ctx->index_len > 0) {
        qsort(ctx->index, ctx->index_len, sizeof(tar_index_entry_t), IndexCmp);
    }

    DEBUG_TRACE("tarfs: indexed %u entries across %u layers", ctx->index_len,
                ctx->layer_cnt);

    return ctx;
}

void TarFsDestroy(vfs_tarfs_ctx_t *ctx) {
    if (!ctx)
        return;
    if (ctx->owned) {
        for (uint16_t i = 0; i < ctx->owned_len; i++)
            WantedFree(ctx->owned[i]);
        WantedFree(ctx->owned);
    }
    if (ctx->index)
        WantedFree(ctx->index);
    WantedFree(ctx);
}

const uint8_t *TarFsEntrypointWasm(const vfs_tarfs_ctx_t *ctx, size_t *len) {
    if (!ctx || !len)
        return NULL;
    *len = ctx->entrypoint_wasm_len;
    return ctx->entrypoint_wasm;
}

const uint8_t *TarFsEntrypointManifest(const vfs_tarfs_ctx_t *ctx,
                                       size_t *len) {
    if (!ctx || !len)
        return NULL;
    *len = ctx->entrypoint_manifest_len;
    return ctx->entrypoint_manifest;
}

uint16_t TarFsIndexLen(const vfs_tarfs_ctx_t *ctx) {
    return ctx ? ctx->index_len : 0;
}
