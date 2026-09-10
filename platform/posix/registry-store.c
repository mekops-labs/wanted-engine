/* SPDX-License-Identifier: Apache-2.0 */

/* Shared registry install and remove. The writer stages incoming bytes to a
 * temp file and renames it into place once the stream completes, named from the
 * install ref. Enumeration stays platform-specific. */

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

/* The target's own config header, not the generic one: a platform that puts
 * the registry on a dedicated filesystem redefines REGISTRY_ROOT there, and
 * including only the base would silently write to the relative default. A
 * board has no working directory for that default to resolve against. */
#ifdef __NuttX__
#include <config-nuttx.h>
#else
#include <platform-config.h>
#endif

#include <platform.h>

/* Sidecar path for an entry, beside its stored image. */
static int metaPath(char *out, size_t outLen, const char *name,
                    const char *version) {
    int n = snprintf(out, outLen, "%s/%s%c%s%s", REGISTRY_ROOT, name,
                     REGISTRY_VERSION_SEPARATOR, version, REGISTRY_META_EXT);
    if (n < 0 || (size_t)n >= outLen)
        return -ENAMETOOLONG;
    return 0;
}

static int metaLoad(const char *path, registry_meta_t *out) {
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return -ENOENT;
    size_t r = fread(out, 1, sizeof(*out), f);
    fclose(f);
    if (!RegistryMetaValid(out, r))
        return -ENOENT;
    return 0;
}

static int metaStore(const char *path, const registry_meta_t *meta) {
    FILE *f = fopen(path, "wb");
    if (f == NULL)
        return -errno;
    size_t w = fwrite(meta, 1, sizeof(*meta), f);
    fclose(f);
    if (w != sizeof(*meta)) {
        remove(path);
        return -EIO;
    }
    return 0;
}

int PlatformRegistryWrite(write_state_t s, const char *ref, const uint8_t *buf,
                          size_t nbytes) {
    static FILE *f;
    static const char tempName[] = REGISTRY_ROOT "/_temp";
    static char targetRef[PATH_MAX];
    static char targetName[PATH_MAX];
    /* Hashing rides the install stream, so covering every stored byte costs
     * no extra read. */
    static void *sha;
    static size_t stored;

    int written = 0;

    switch (s) {
    case START_WRITE:
        if (buf == NULL || nbytes == 0)
            return -EINVAL;
        /* The install target is named by the ref ("<name>:<version>"), captured
         * here and used to name the stored file at FINISH_WRITE. */
        if (ref == NULL || ref[0] == '\0')
            return -EINVAL;
        strncpy(targetRef, ref, sizeof(targetRef) - 1);
        targetRef[sizeof(targetRef) - 1] = '\0';
        f = fopen(tempName, "w");
        if (f == NULL)
            return -errno;

        if (sha != NULL)
            PlatformSha256Free(sha);
        sha = PlatformSha256New();
        stored = 0;

        /* write first chunk */
        written = fwrite(buf, 1, nbytes, f);
        if (written > 0) {
            if (sha != NULL)
                PlatformSha256Update(sha, buf, (size_t)written);
            stored += (size_t)written;
        }
        break;
    case CONTINUE_WRITE:
        if (buf == NULL || nbytes == 0)
            return -EINVAL;
        if (f == NULL)
            return -EBADF;
        written = fwrite(buf, 1, nbytes, f);
        if (written > 0) {
            if (sha != NULL)
                PlatformSha256Update(sha, buf, (size_t)written);
            stored += (size_t)written;
        }
        break;
    case FINISH_WRITE: {
        if (f == NULL)
            return -EBADF;
        fclose(f);
        f = NULL;
        if (targetRef[0] == '\0') {
            remove(tempName);
            return -EINVAL;
        }

        /* The ref uses ':'; stored files use REGISTRY_VERSION_SEPARATOR, which
         * is what the loader and enumerator look for. */
        char *sep = strchr(targetRef, ':');
        if (sep != NULL)
            *sep = REGISTRY_VERSION_SEPARATOR;

        int n = snprintf(targetName, sizeof(targetName), "%s/%s%s",
                         REGISTRY_ROOT, targetRef, REGISTRY_EXT);
        if (n < 0 || (size_t)n >= sizeof(targetName)) {
            /* A truncated path would name the wrong file — reject it. */
            targetRef[0] = '\0';
            remove(tempName);
            return -ENAMETOOLONG;
        }

        registry_meta_t meta = {
            .magic = REGISTRY_META_MAGIC,
            .size = (uint32_t)stored,
            .layerCount = 1,
        };
        if (sha != NULL) {
            PlatformSha256Final(sha, meta.layerDigest[0]);
            PlatformSha256Free(sha);
            sha = NULL;
        }

        char metaName[PATH_MAX];
        int rc = snprintf(metaName, sizeof(metaName), "%s/%s%s", REGISTRY_ROOT,
                          targetRef, REGISTRY_META_EXT);
        targetRef[0] = '\0';
        if (rc < 0 || (size_t)rc >= sizeof(metaName)) {
            remove(tempName);
            return -ENAMETOOLONG;
        }
        if (rename(tempName, targetName) < 0) {
            remove(tempName);
            return -errno;
        }
        /* An image whose record did not land is unverifiable, so it does not
         * stay installed. */
        rc = metaStore(metaName, &meta);
        if (rc < 0) {
            remove(targetName);
            return rc;
        }
        break;
    }
    case ABORT_WRITE:
        if (f == NULL)
            return -EBADF;
        fclose(f);
        f = NULL;
        if (sha != NULL) {
            PlatformSha256Free(sha);
            sha = NULL;
        }
        targetRef[0] = '\0';
        remove(tempName);
        break;
    default:
        return -EINVAL;
        break;
    }

    return written;
}

int PlatformRegistryRemove(const reg_entry_t *entry) {
    char targetName[PATH_MAX];
    char metaName[PATH_MAX];

    snprintf(targetName, sizeof(targetName), "%s/%s%c%s%s", REGISTRY_ROOT,
             entry->name, REGISTRY_VERSION_SEPARATOR, entry->version,
             REGISTRY_EXT);
    if (remove(targetName) != 0) {
        return -errno;
    }
    if (metaPath(metaName, sizeof(metaName), entry->name, entry->version) == 0)
        remove(metaName);

    return 0;
}

int PlatformRegistryMetaRead(const reg_entry_t *entry, registry_meta_t *out) {
    char path[PATH_MAX];
    int rc;

    if (entry == NULL || out == NULL)
        return -EINVAL;
    rc = metaPath(path, sizeof(path), entry->name, entry->version);
    if (rc < 0)
        return rc;
    return metaLoad(path, out);
}

int PlatformRegistryMetaSetSignature(const reg_entry_t *entry, uint32_t keyId,
                                     const uint8_t sig[REGISTRY_META_SIG_LEN]) {
    char path[PATH_MAX];
    registry_meta_t meta;
    int rc;

    if (entry == NULL || sig == NULL)
        return -EINVAL;
    rc = metaPath(path, sizeof(path), entry->name, entry->version);
    if (rc < 0)
        return rc;
    rc = metaLoad(path, &meta);
    if (rc < 0)
        return rc;

    meta.keyId = keyId;
    memcpy(meta.signature, sig, REGISTRY_META_SIG_LEN);
    meta.flags |= REGISTRY_META_SIGNED;
    return metaStore(path, &meta);
}

/* This backing seeds nothing, so nothing is protected from removal. */
void PlatformRegistryMarkSeeded(const char *ref) { (void)ref; }
