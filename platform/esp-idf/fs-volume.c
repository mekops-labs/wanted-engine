/* SPDX-License-Identifier: Apache-2.0 */

/* ESP-IDF writable volume root — the LittleFS mount point the registry and wapp
 * state directories live under. Mounting the partition is done at startup. */

#include <errno.h>
#include <stdio.h>

#include <platform.h>

#define VOLUME_ROOT "/data"

const char *PlatformVolumeRoot(void) { return VOLUME_ROOT; }

/* Read a whole small file. Sized by the caller's buffer rather than by stat,
 * so a file that grows between the two cannot overrun it. Standard stdio
 * against the LittleFS mount, same as the rest of this file's callers already
 * use (e.g. the Wi-Fi bring-up credential store). */
int PlatformReadSmallFile(const char *path, char *buf, size_t bufLen) {
    if (path == NULL || buf == NULL || bufLen < 2)
        return -EINVAL;

    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return (errno == ENOENT) ? -ENOENT : -EIO;

    size_t n = fread(buf, 1, bufLen - 1, f);
    int err = ferror(f);
    /* A full buffer with bytes still unread means the file does not fit; the
     * caller must not act on a truncated document. */
    int over = (n == bufLen - 1) && (fgetc(f) != EOF);
    fclose(f);

    if (err)
        return -EIO;
    if (over)
        return -ENOSPC;

    buf[n] = '\0';
    return (int)n;
}
