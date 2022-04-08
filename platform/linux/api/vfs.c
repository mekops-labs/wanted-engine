#include <platform.h>
#include <vfs-linux.h>
#include <config-linux.h>

vfs_driver_t *VfsPlatformFsInit()
{
    return VfsLinuxInit(DEFAULT_ROOT);
}
