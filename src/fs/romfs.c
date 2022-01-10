#include <stdio.h>
#include <stdint.h>
#include <errno.h>

#include "romfs-internal.h"

/* TODO
1. get file mode - m3_wasi_generic_fd_fdstat_get
2. open - m3_wasi_generic_path_open
3. read dir - m3_wasi_generic_fd_readdir
4. read file - m3_wasi_generic_fd_read
...
*/

static romfs_t romfs;

#define MAX_OPEN 10

#define YES 1
#define NO 0

static struct {
    uint8_t opened;
    nodehdr_t node;
} fildes[MAX_OPEN];

/* PUBLIC functions */

int RomfsLoad(uint8_t * img, size_t imgSize) {
    int ret = 0;

    romfs.img = img;
    romfs.size = imgSize;

    ret = RomfsVolumeConfigure(romfs.img, &romfs.vol);

    ROMFS_TRACE("Loaded volume \"%s\". Size is %ld bytes. First entry offset = 0x%x",
        romfs.vol.name,
        romfs.vol.size,
        romfs.vol.rootOff);

    // preopen root dir as first file descriptor
    fildes[0].opened = YES;
    RomfsGetNodeHdr(&romfs, romfs.vol.rootOff, &fildes[0].node);

    return ret;
}

int RomfsFdStat(int fd)
{
    int file = fd - 3;

    if (file < 0 || file > MAX_OPEN) {
        return -EBADF;
    }

    if (!fildes[file].opened) {
        return -EBADF;
    }

    return fildes[file].node.mode;
}

int RomfsOpenAt(int fd, const char *path, int flags, int mode) {
    return -EACCES;
}
