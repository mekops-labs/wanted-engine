/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <limits.h>
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
#define PATH_POOL_INIT_CAP 256

typedef struct __attribute__((packed)) tar_index_entry_t {
    const char *path_ptr;
    uint16_t layer_idx;
    uint64_t hdr_offset;
    uint32_t size;
} tar_index_entry_t;

/* Heap-allocated handle returned by TarFs_Open. Files carry layer/offset/size
 * + a read cursor; directories carry the synthesised "<dir>/" prefix used to
 * walk the sorted index. */
typedef struct tarfs_file_ctx_t {
    bool is_dir;
    uint16_t layer_idx;
    uint64_t hdr_offset;
    uint32_t size;
    uint32_t pos;
    char *dir_prefix;
    size_t dir_prefix_len;
} tarfs_file_ctx_t;

struct vfs_tarfs_ctx_t {
    const uint8_t *layers[TARFS_MAX_LAYERS];
    size_t layer_lens[TARFS_MAX_LAYERS];
    uint8_t layer_cnt;

    tar_index_entry_t *index;
    uint16_t index_len;
    uint16_t index_cap;

    /* Heap-owned path strings (whiteout targets — non-zero-copy). */
    char **owned;
    uint16_t owned_len;
    uint16_t owned_cap;

    /* String pool for paths that cannot be zero-copied from the layer:
     * PAX path= values (end with '\n', not '\0') and ustar prefix+name
     * concatenations. Allocated lazily on first use. */
    char *path_pool;
    size_t path_pool_used;
    size_t path_pool_cap;

    /* Boot optimisation: direct pointer into layer data for the entrypoint. */
    const uint8_t *entrypoint_wasm;
    size_t entrypoint_wasm_len;
};

/* POSIX ustar stores numeric fields as NUL/space-terminated octal ASCII. */
static uint32_t parseOctal(const char *s, size_t n) {
    uint32_t v = 0;
    for (size_t i = 0; i < n && s[i] >= '0' && s[i] <= '7'; i++) {
        v = (v << 3) | (uint32_t)(s[i] - '0');
    }
    return v;
}

static inline size_t alignUp512(size_t n) {
    return (n + (TAR_BLOCK_SIZE - 1)) & ~(size_t)(TAR_BLOCK_SIZE - 1);
}

static bool isZeroBlock(const uint8_t *b) {
    for (size_t i = 0; i < TAR_BLOCK_SIZE; i++) {
        if (b[i])
            return false;
    }
    return true;
}

/* Binary search over the sorted prefix [0, n). Only valid when n entries have
 * already been sorted — i.e. called with the length from a prior layer, not
 * including entries currently being appended for the layer under construction.
 */
static bool indexContains(const tar_index_entry_t *idx, uint16_t n,
                          const char *path) {
    uint16_t lo = 0, hi = n;
    while (lo < hi) {
        uint16_t mid = lo + (uint16_t)((hi - lo) / 2);
        int cmp = strcmp(idx[mid].path_ptr, path);
        if (cmp < 0)
            lo = (uint16_t)(mid + 1);
        else if (cmp > 0)
            hi = mid;
        else
            return true;
    }
    return false;
}

static int growIndex(vfs_tarfs_ctx_t *ctx) {
    uint16_t new_cap =
        ctx->index_cap ? (uint16_t)(ctx->index_cap * 2) : INDEX_INIT_CAP;
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

static int rememberOwned(vfs_tarfs_ctx_t *ctx, char *s) {
    if (ctx->owned_len == ctx->owned_cap) {
        uint16_t nc =
            ctx->owned_cap ? (uint16_t)(ctx->owned_cap * 2) : OWNED_INIT_CAP;
        char **next = (char **)WantedMalloc((size_t)nc * sizeof(char *));
        if (!next)
            return -ENOMEM;
        if (ctx->owned) {
            memcpy((void *)next, (const void *)ctx->owned,
                   (size_t)ctx->owned_len * sizeof(char *));
            WantedFree((void *)ctx->owned);
        }
        ctx->owned = next;
        ctx->owned_cap = nc;
    }
    ctx->owned[ctx->owned_len++] = s;
    return 0;
}

/* Copy s[0..len) into the path pool (null-terminated) and return a pointer
 * into the pool. Returns NULL on allocation failure. */
static const char *internPath(vfs_tarfs_ctx_t *ctx, const char *s, size_t len) {
    if (ctx->path_pool_used + len + 1 > ctx->path_pool_cap) {
        size_t new_cap =
            ctx->path_pool_cap ? ctx->path_pool_cap * 2 : PATH_POOL_INIT_CAP;
        while (new_cap < ctx->path_pool_used + len + 1)
            new_cap *= 2;
        char *next = WantedMalloc(new_cap);
        if (!next)
            return NULL;
        if (ctx->path_pool) {
            memcpy(next, ctx->path_pool, ctx->path_pool_used);
            WantedFree(ctx->path_pool);
        }
        ctx->path_pool = next;
        ctx->path_pool_cap = new_cap;
    }
    char *dest = ctx->path_pool + ctx->path_pool_used;
    memcpy(dest, s, len);
    dest[len] = '\0';
    ctx->path_pool_used += len + 1;
    return dest;
}

static const char *pathBasename(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

/* "foo/.wh.bar" -> heap-allocated "foo/bar". Caller owns the result. */
static char *makeWhiteoutTarget(const char *path) {
    const char *base = pathBasename(path);
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

/* Walk a PAX extended header data block and extract path= and size= overrides.
 * Both out-params are left unchanged if the corresponding key is absent.
 * path_out points into data[] — not null-terminated; caller must intern it. */
static void parsePaxData(const uint8_t *data, size_t data_len,
                         const char **path_out, size_t *path_len_out,
                         uint32_t *size_out) {
    size_t pos = 0;
    while (pos < data_len) {
        size_t rec_start = pos;
        size_t rec_len = 0;
        while (pos < data_len && data[pos] >= '0' && data[pos] <= '9')
            rec_len = rec_len * 10 + (size_t)(data[pos++] - '0');
        if (rec_len == 0 || pos >= data_len || data[pos] != ' ')
            break;
        pos++; /* skip space */

        size_t rec_end = rec_start + rec_len;
        if (rec_end > data_len)
            break;

        /* Locate '=' separating key from value. */
        size_t key_start = pos;
        while (pos < rec_end && data[pos] != '=' && data[pos] != '\n')
            pos++;
        if (pos >= rec_end || data[pos] != '=')
            goto next_record;

        {
            size_t key_len = pos - key_start;
            const char *key = (const char *)data + key_start;
            const char *val = (const char *)data + pos + 1;
            /* Value ends at rec_end - 1 (the '\n'). Guard against underflow. */
            size_t val_len = (rec_end > 0 && rec_end - 1 > (size_t)(pos + 1))
                                 ? rec_end - 1 - (pos + 1)
                                 : 0;

            if (key_len == 4 && memcmp(key, "path", 4) == 0) {
                *path_out = val;
                *path_len_out = val_len;
            } else if (key_len == 4 && memcmp(key, "size", 4) == 0) {
                uint32_t s = 0;
                for (size_t i = 0; i < val_len; i++) {
                    if (val[i] < '0' || val[i] > '9')
                        break;
                    s = s * 10 + (uint32_t)(val[i] - '0');
                }
                *size_out = s;
            }
        }

    next_record:
        pos = rec_end;
    }
}

/* sorted_len: number of entries in ctx->index that are already sorted (i.e.
 * entries from all previously-indexed layers). Used for O(log N) dedup. */
static int indexLayer(vfs_tarfs_ctx_t *ctx, uint8_t layer_idx,
                      uint16_t sorted_len) {
    const uint8_t *buf = ctx->layers[layer_idx];
    size_t len = ctx->layer_lens[layer_idx];
    size_t off = 0;
    int empty_run = 0;

    /* Overrides set by extended header blocks ('x', 'L') for the next entry. */
    const char *override_path = NULL;
    uint32_t override_size = 0;

    while (off + TAR_BLOCK_SIZE <= len) {
        const uint8_t *hdr = buf + off;

        /* Two consecutive zero blocks mark end of archive. */
        if (isZeroBlock(hdr)) {
            if (++empty_run >= 2)
                break;
            off += TAR_BLOCK_SIZE;
            continue;
        }
        empty_run = 0;

        uint32_t size =
            parseOctal((const char *)(hdr + TAR_SIZE_OFFSET), TAR_SIZE_LEN);

        /* Guard against crafted entries whose claimed size overflows or exceeds
         * the layer buffer. alignUp512 can wrap on 32-bit if size is near
         * SIZE_MAX, so validate before the arithmetic. */
        if (len - off < TAR_BLOCK_SIZE ||
            (size_t)size > len - off - TAR_BLOCK_SIZE) {
            DEBUG_TRACE("tarfs: entry at %zu size %u exceeds layer bound %zu",
                        off, size, len);
            break;
        }
        size_t aligned_size = alignUp512((size_t)size);
        if (aligned_size < (size_t)size) {
            break;
        }
        size_t next_off = off + TAR_BLOCK_SIZE + aligned_size;
        if (next_off < off) {
            break;
        }

        uint8_t typeflag = hdr[TAR_TYPEFLAG_OFFSET];

        /* Extended header blocks: extract overrides for the immediately
         * following entry, then continue to the next loop iteration. */
        if (typeflag == 'x') {
            /* PAX local extended header: parse path= and size= keys. */
            const char *pax_path = NULL;
            size_t pax_path_len = 0;
            parsePaxData(buf + off + TAR_BLOCK_SIZE, size, &pax_path,
                         &pax_path_len, &override_size);
            if (pax_path && pax_path_len > 0) {
                /* Normalise leading "./" or "/" before interning. */
                if (pax_path_len >= 2 && pax_path[0] == '.' &&
                    pax_path[1] == '/') {
                    pax_path += 2;
                    pax_path_len -= 2;
                } else if (pax_path[0] == '/') {
                    pax_path += 1;
                    pax_path_len -= 1;
                }
                if (pax_path_len > 0) {
                    const char *p = internPath(ctx, pax_path, pax_path_len);
                    if (!p)
                        return -ENOMEM;
                    override_path = p;
                }
            }
            off = next_off;
            continue;
        }
        if (typeflag == 'g' || typeflag == 'K') {
            /* PAX global header or GNU long linkname: nothing we use. */
            off = next_off;
            continue;
        }
        if (typeflag == 'L') {
            /* GNU long filename: data block is null-terminated. Zero-copy. */
            if (size > 0 && off + TAR_BLOCK_SIZE < len) {
                const char *p = (const char *)(buf + off + TAR_BLOCK_SIZE);
                size_t pl = strnlen(p, size);
                if (pl >= 2 && p[0] == '.' && p[1] == '/') {
                    p += 2;
                    pl -= 2;
                } else if (pl >= 1 && p[0] == '/') {
                    p += 1;
                    pl -= 1;
                }
                if (pl > 0)
                    override_path = p;
            }
            off = next_off;
            continue;
        }

        /* Resolve the effective path and file size for this entry. */
        const char *eff_path;
        uint32_t eff_size = override_size ? override_size : size;

        if (override_path) {
            eff_path = override_path;
            override_path = NULL;
            override_size = 0;
        } else {
            override_size = 0;

            const char *name = (const char *)(hdr + TAR_NAME_OFFSET);
            size_t name_field_len = strnlen(name, TAR_NAME_LEN);

            if (name_field_len == TAR_NAME_LEN) {
                /* Name field has no NUL — malformed entry, skip. */
                off = next_off;
                continue;
            }
            if (name[0] == '\0') {
                off = next_off;
                continue;
            }

            if (hdr[TAR_PREFIX_OFFSET] != '\0') {
                /* ustar prefix+name: intern as "prefix/name". */
                const char *prefix = (const char *)(hdr + TAR_PREFIX_OFFSET);
                size_t prefix_len = strnlen(prefix, 155);
                char concat[257];
                size_t total = prefix_len + 1 + name_field_len;
                if (total >= sizeof(concat)) {
                    off = next_off;
                    continue;
                }
                memcpy(concat, prefix, prefix_len);
                concat[prefix_len] = '/';
                memcpy(concat + prefix_len + 1, name, name_field_len);
                concat[total] = '\0';
                const char *p = internPath(ctx, concat, total);
                if (!p)
                    return -ENOMEM;
                eff_path = p;
            } else {
                eff_path = name;
            }

            /* Normalise leading "./" or "/" produced by some TAR writers. */
            if (eff_path[0] == '.' && eff_path[1] == '/')
                eff_path += 2;
            else if (eff_path[0] == '/')
                eff_path += 1;
        }

        bool is_regular = (typeflag == '0' || typeflag == '\0');
        bool is_dir = (typeflag == '5');

        /* Links, fifos, device nodes: unsupported. Directories are implicit
         * (ReadDir uses path prefixes), so we don't index them either. */
        if (!is_regular || is_dir) {
            off = next_off;
            continue;
        }

        const char *base = pathBasename(eff_path);
        bool is_whiteout = strncmp(base, ".wh.", 4) == 0 && base[4] != '\0';

        const char *entry_path;
        uint16_t entry_layer;

        if (is_whiteout) {
            char *target = makeWhiteoutTarget(eff_path);
            if (!target)
                return -ENOMEM;
            int ret = rememberOwned(ctx, target);
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

        /* Newer-layer shadowing: first occurrence wins.
         * Search only the sorted prefix from previous layers. Within-layer
         * duplicates are the TAR writer's problem; well-formed archives don't
         * have them. */
        if (indexContains(ctx->index, sorted_len, entry_path)) {
            off = next_off;
            continue;
        }

        /* Pre-fetch the boot entrypoint from the topmost non-whiteout hit. */
        if (!is_whiteout) {
            if (strcmp(entry_path, "app.wasm") == 0 &&
                ctx->entrypoint_wasm == NULL) {
                ctx->entrypoint_wasm = buf + off + TAR_BLOCK_SIZE;
                ctx->entrypoint_wasm_len = eff_size;
            }
        }

        if (ctx->index_len == ctx->index_cap) {
            int ret = growIndex(ctx);
            if (ret < 0)
                return ret;
        }
        tar_index_entry_t *e = &ctx->index[ctx->index_len++];
        e->path_ptr = entry_path;
        e->layer_idx = entry_layer;
        e->hdr_offset = off;
        e->size = eff_size;

        off = next_off;
    }

    return 0;
}

static int indexCmp(const void *a, const void *b) {
    const tar_index_entry_t *ea = a;
    const tar_index_entry_t *eb = b;
    return strcmp(ea->path_ptr, eb->path_ptr);
}

/* Callers pass uint8_t *[]; const-qualifying the array elements would break
 * them under GCC's incompatible-pointer-types. */
/* cppcheck-suppress constParameter */
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
        /* snapshot of the sorted boundary before this layer's entries land */
        uint16_t sorted_len = ctx->index_len;
        if (indexLayer(ctx, i, sorted_len) < 0) {
            DEBUG_TRACE("tarfs: indexing failed on layer %u", i);
            TarFsDestroy(ctx);
            return NULL;
        }
        /* Sort the full index so subsequent layers can binary-search it. */
        if (ctx->index_len > 0)
            qsort(ctx->index, ctx->index_len, sizeof(tar_index_entry_t),
                  indexCmp);
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
        WantedFree((void *)ctx->owned);
    }
    if (ctx->index)
        WantedFree(ctx->index);
    if (ctx->path_pool)
        WantedFree(ctx->path_pool);
    WantedFree(ctx);
}

const uint8_t *TarFsEntrypointWasm(const vfs_tarfs_ctx_t *ctx, size_t *len) {
    if (!ctx || !len)
        return NULL;
    *len = ctx->entrypoint_wasm_len;
    return ctx->entrypoint_wasm;
}

uint16_t TarFsIndexLen(const vfs_tarfs_ctx_t *ctx) {
    return ctx ? ctx->index_len : 0;
}

/* File/directory operations.
 *
 * Lookups run on the qsorted index — exact-match for files, "<path>/" prefix
 * scan for implicit directories. Reads are zero-copy via the layer pointer;
 * the only heap allocation per open is the handle plus (for directories) a
 * copy of the prefix string. */

static uint16_t lowerBound(const tar_index_entry_t *idx, uint16_t n,
                           const char *key) {
    uint16_t lo = 0, hi = n;
    while (lo < hi) {
        uint16_t mid = lo + (uint16_t)((hi - lo) / 2);
        if (strcmp(idx[mid].path_ptr, key) < 0)
            lo = (uint16_t)(mid + 1);
        else
            hi = mid;
    }
    return lo;
}

static const tar_index_entry_t *findExact(const vfs_tarfs_ctx_t *ctx,
                                          const char *path) {
    uint16_t pos = lowerBound(ctx->index, ctx->index_len, path);
    if (pos < ctx->index_len && strcmp(ctx->index[pos].path_ptr, path) == 0) {
        return &ctx->index[pos];
    }
    return NULL;
}

static bool directoryExists(const vfs_tarfs_ctx_t *ctx, const char *prefix,
                            size_t prefix_len) {
    if (prefix_len == 0)
        return ctx->index_len > 0;
    uint16_t pos = lowerBound(ctx->index, ctx->index_len, prefix);
    return pos < ctx->index_len &&
           strncmp(ctx->index[pos].path_ptr, prefix, prefix_len) == 0;
}

static tarfs_file_ctx_t *allocFileHandle(const tar_index_entry_t *e) {
    tarfs_file_ctx_t *h = WantedMalloc(sizeof(*h));
    if (!h)
        return NULL;
    memset(h, 0, sizeof(*h));
    h->is_dir = false;
    h->layer_idx = e->layer_idx;
    h->hdr_offset = e->hdr_offset;
    h->size = e->size;
    h->pos = 0;
    return h;
}

static tarfs_file_ctx_t *allocDirHandle(const char *prefix, size_t prefix_len) {
    tarfs_file_ctx_t *h = WantedMalloc(sizeof(*h));
    if (!h)
        return NULL;
    memset(h, 0, sizeof(*h));
    h->is_dir = true;
    h->dir_prefix = WantedMalloc(prefix_len + 1);
    if (!h->dir_prefix) {
        WantedFree(h);
        return NULL;
    }
    if (prefix_len > 0)
        memcpy(h->dir_prefix, prefix, prefix_len);
    h->dir_prefix[prefix_len] = '\0';
    h->dir_prefix_len = prefix_len;
    return h;
}

void *TarFs_Open(const vfs_tarfs_ctx_t *ctx, const char *path,
                 vfs_oflags_t flags) {
    if (!ctx || !path)
        return NULL;

    /* Read-only filesystem. Anything other than O_RDONLY without create/trunc
     * is rejected before we touch the index. */
    if ((flags & 03) != VFS_O_RDONLY)
        return NULL;
    if (flags & (VFS_O_CREAT | VFS_O_TRUNC))
        return NULL;

    while (*path == '/')
        path++;

    size_t plen = strlen(path);
    bool trailing_slash = (plen > 0 && path[plen - 1] == '/');
    if (trailing_slash)
        plen--;

    if (plen > 512)
        return NULL;

    char work[514]; /* path + trailing '/' + NUL */
    if (plen > 0)
        memcpy(work, path, plen);
    work[plen] = '\0';

    if (plen == 0) {
        /* Root directory always exists; empty prefix matches every entry. */
        return allocDirHandle("", 0);
    }

    if (!trailing_slash) {
        const tar_index_entry_t *e = findExact(ctx, work);
        if (e != NULL) {
            if (e->layer_idx == TARFS_WHITEOUT)
                return NULL;
            if (flags & VFS_O_DIRECTORY)
                return NULL;
            return allocFileHandle(e);
        }
    }

    /* Treat as directory: synthesise "<work>/" and look for any indexed
     * entry sharing that prefix. */
    work[plen] = '/';
    work[plen + 1] = '\0';
    size_t prefix_len = plen + 1;

    if (!directoryExists(ctx, work, prefix_len))
        return NULL;

    return allocDirHandle(work, prefix_len);
}

int TarFs_Close(vfs_tarfs_ctx_t *ctx, void *handle) {
    (void)ctx;
    if (!handle)
        return -EBADF;
    tarfs_file_ctx_t *h = handle;
    if (h->is_dir && h->dir_prefix)
        WantedFree(h->dir_prefix);
    WantedFree(h);
    return 0;
}

int TarFs_Read(vfs_tarfs_ctx_t *ctx, void *handle, void *buf, size_t nbyte) {
    if (!ctx || !handle || !buf)
        return -EINVAL;
    tarfs_file_ctx_t *h = handle;
    if (h->is_dir)
        return -EISDIR;
    if (h->pos >= h->size)
        return 0;

    size_t remaining = h->size - h->pos;
    size_t n = nbyte < remaining ? nbyte : remaining;
    /* Clamp to INT_MAX so the cast to int is always non-negative. */
    if (n > (size_t)INT_MAX)
        n = (size_t)INT_MAX;
    const uint8_t *data =
        ctx->layers[h->layer_idx] + h->hdr_offset + TAR_BLOCK_SIZE + h->pos;
    memcpy(buf, data, n);
    h->pos += (uint32_t)n;
    return (int)n;
}

int TarFs_Stat(vfs_tarfs_ctx_t *ctx, void *handle, vfs_stat_t *stat) {
    (void)ctx;
    if (!handle || !stat)
        return -EINVAL;
    const tarfs_file_ctx_t *h = handle;
    memset(stat, 0, sizeof(*stat));
    if (h->is_dir) {
        stat->filetype = VFS_FILETYPE_DIRECTORY;
        stat->size = 0;
    } else {
        stat->filetype = VFS_FILETYPE_REGULAR_FILE;
        stat->size = h->size;
    }
    stat->nlink = 1;
    return 0;
}

int TarFs_Seek(vfs_tarfs_ctx_t *ctx, void *handle, long off,
               vfs_whence_t whence, long *pos) {
    (void)ctx;
    if (!handle || !pos)
        return -EINVAL;
    tarfs_file_ctx_t *h = handle;
    if (h->is_dir)
        return -EISDIR;

    long new_pos;
    switch (whence) {
    case VFS_SEEK_SET:
        new_pos = off;
        break;
    case VFS_SEEK_CUR:
        new_pos = (long)h->pos + off;
        break;
    case VFS_SEEK_END:
        new_pos = (long)h->size + off;
        break;
    default:
        return -EINVAL;
    }
    if (new_pos < 0)
        return -EINVAL;
    if (new_pos > (long)h->size)
        new_pos = (long)h->size;
    h->pos = (uint32_t)new_pos;
    *pos = new_pos;
    return 0;
}

int TarFs_ReadDir(vfs_tarfs_ctx_t *ctx, void *handle, void *buf, size_t bufLen,
                  uint64_t *cookie, size_t *bufUsed) {
    if (!ctx || !handle || !buf || !cookie || !bufUsed)
        return -EINVAL;
    const tarfs_file_ctx_t *h = handle;
    if (!h->is_dir)
        return -ENOTDIR;

    size_t used = 0;
    uint16_t i;
    if (*cookie == 0) {
        i = lowerBound(ctx->index, ctx->index_len, h->dir_prefix);
    } else {
        /* WASI dircookie is uint64_t. The index is uint16_t-bounded, so any
         * cookie beyond the index length means iteration is complete. */
        if (*cookie > ctx->index_len)
            return -EINVAL;
        i = (uint16_t)*cookie;
    }

    while (i < ctx->index_len) {
        const tar_index_entry_t *e = &ctx->index[i];

        if (h->dir_prefix_len > 0 &&
            strncmp(e->path_ptr, h->dir_prefix, h->dir_prefix_len) != 0) {
            break;
        }

        const char *suffix = e->path_ptr + h->dir_prefix_len;
        if (*suffix == '\0') {
            i++;
            continue;
        }

        const char *slash = strchr(suffix, '/');
        bool is_subdir = (slash != NULL);
        size_t namlen = is_subdir ? (size_t)(slash - suffix) : strlen(suffix);

        if (!is_subdir && e->layer_idx == TARFS_WHITEOUT) {
            i++;
            continue;
        }

        if (used + sizeof(vfs_dirent_t) + namlen > bufLen)
            break;

        vfs_dirent_t dir = {0};
        dir.d_ino = i;
        dir.d_namlen = (uint32_t)namlen;
        dir.d_type =
            is_subdir ? VFS_FILETYPE_DIRECTORY : VFS_FILETYPE_REGULAR_FILE;

        uint16_t next_i;
        if (is_subdir) {
            /* Sorted index keeps siblings under "<prefix><name>/" contiguous,
             * so dedup is a forward scan rather than a hash. */
            next_i = (uint16_t)(i + 1);
            while (next_i < ctx->index_len) {
                const char *p = ctx->index[next_i].path_ptr;
                if (h->dir_prefix_len > 0 &&
                    strncmp(p, h->dir_prefix, h->dir_prefix_len) != 0)
                    break;
                const char *s = p + h->dir_prefix_len;
                if (strncmp(s, suffix, namlen) != 0)
                    break;
                if (s[namlen] != '/')
                    break;
                next_i++;
            }
        } else {
            next_i = (uint16_t)(i + 1);
        }
        dir.d_next = next_i;

        memcpy((uint8_t *)buf + used, &dir, sizeof(dir));
        memcpy((uint8_t *)buf + used + sizeof(dir), suffix, namlen);
        used += sizeof(dir) + namlen;

        i = next_i;
    }

    *cookie = i;
    *bufUsed = used;
    return 0;
}
