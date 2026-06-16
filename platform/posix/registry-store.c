/* SPDX-License-Identifier: Apache-2.0 */

/* Shared registry install/remove. The writer stages incoming bytes to a temp
 * file then renames it into place under the install ref ("<name>:<ver>") once
 * the stream completes; remove deletes a stored image by ref. Registry
 * enumeration (PlatformRegistryRead) is platform-specific. */

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#include <platform-config.h>
#include <platform.h>

int PlatformRegistryWrite(write_state_t s, const char *ref, const uint8_t *buf,
                          size_t nbytes) {
    static FILE *f;
    static const char tempName[] = REGISTRY_ROOT "/_temp";
    static char targetRef[PATH_MAX];
    static char targetName[PATH_MAX];

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

        /* write first chunk */
        written = fwrite(buf, 1, nbytes, f);
        break;
    case CONTINUE_WRITE:
        if (buf == NULL || nbytes == 0)
            return -EINVAL;
        if (f == NULL)
            return -EBADF;
        written = fwrite(buf, 1, nbytes, f);
        break;
    case FINISH_WRITE:
        if (f == NULL)
            return -EBADF;
        fclose(f);
        f = NULL;
        if (targetRef[0] == '\0') {
            remove(tempName);
            return -EINVAL;
        }

        int n = snprintf(targetName, sizeof(targetName), "%s/%s%s",
                         REGISTRY_ROOT, targetRef, REGISTRY_EXT);
        targetRef[0] = '\0';
        if (n < 0 || (size_t)n >= sizeof(targetName)) {
            /* A truncated path would name the wrong file — reject it. */
            remove(tempName);
            return -ENAMETOOLONG;
        }
        if (rename(tempName, targetName) < 0) {
            remove(tempName);
            return -errno;
        }
        break;
    case ABORT_WRITE:
        if (f == NULL)
            return -EBADF;
        fclose(f);
        f = NULL;
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

    snprintf(targetName, sizeof(targetName), "%s/%s%c%s%s", REGISTRY_ROOT,
             entry->name, REGISTRY_VERSION_SEPARATOR, entry->version,
             REGISTRY_EXT);
    if (remove(targetName) != 0) {
        return -errno;
    }

    return 0;
}
