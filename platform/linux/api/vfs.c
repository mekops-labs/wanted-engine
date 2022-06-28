#include <platform.h>
#include <vfs-linux.h>
#include <config-linux.h>
#include <vfs-drivers.h>

vfs_driver_t *VfsPlatformFsInit(const wapp_t *wapp, const char *opt)
{
    return VfsLinuxInit(wapp, opt);
}
