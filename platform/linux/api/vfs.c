#include <platform.h>
#include "../include/vfs-linux.h"

#define DEFAULT_ROOT "./"
#define REGISTRY_ROOT "./wapps"

int VfsPlatformFsInit(vfs_driver_t *driver) {
    return VfsLinuxInit(driver, DEFAULT_ROOT);
}

void VfsPlatformFsDestroy(vfs_driver_t *driver) {
    return VfsLinuxDestroy(driver);
}

int VfsPlatformRegistryInit(vfs_driver_t *driver) {
    return VfsLinuxInit(driver, REGISTRY_ROOT);
}

void VfsPlatformRegistryDestroy(vfs_driver_t *driver) {
    return VfsLinuxDestroy(driver);
}
