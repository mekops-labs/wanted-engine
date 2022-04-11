#include <platform.h>
#include <vfs-linux.h>
#include <config-linux.h>

vfs_driver_t *VfsPlatformFsInit(const wapp_t *wapp, uint8_t argc, const char *args[])
{
    return VfsLinuxInit(wapp, argc, args);
}
