#include <platform.h>
#include "../include/vfs-linux.h"

#define DEFAULT_ROOT "./"

int VfsPlatformInit(vfs_driver_t *driver) {
    return VfsLinuxInit(driver, DEFAULT_ROOT);
}

void VfsPlatformDestroy(vfs_driver_t *driver) {
    return VfsLinuxDestroy(driver);
}
