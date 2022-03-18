#include <platform.h>
#include <vfs-linux.h>
#include <config-linux.h>

int VfsPlatformFsInit(vfs_driver_t *driver) {
    return VfsLinuxInit(driver, DEFAULT_ROOT);
}

void VfsPlatformFsDestroy(vfs_driver_t *driver) {
    return VfsLinuxDestroy(driver);
}
