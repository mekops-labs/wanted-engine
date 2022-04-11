#include <debug_trace.h>
#include <vfs.h>
#include <vfs-drivers.h>

extern const vfs_driver_t WantedConfigDriver;
extern const vfs_driver_t WantedControlDriver;
extern const vfs_driver_t WantedRegistryDriver;

vfs_driver_t *VfsWantedInit()
{
    int ret;
    vfs_driver_t *drv;

    drv = VfsVirtualInit();
    if (NULL == drv) {
        DEBUG_TRACE("VfsWantedInit: can't load virtual driver (%d)", ret);
        return NULL;
    }
    ret = drv->Register(drv->ctx, "config", &WantedConfigDriver);
    if (ret < 0) {
        DEBUG_TRACE("VfsWantedInit: can't register config (%d)", ret);
        return NULL;
    }
    ret = drv->Register(drv->ctx, "ctrl", &WantedControlDriver);
    if (ret < 0) {
        DEBUG_TRACE("VfsWantedInit: can't register ctrl (%d)", ret);
        return NULL;
    }
    ret = drv->Register(drv->ctx, "reg", &WantedRegistryDriver);
    if (ret < 0) {
        DEBUG_TRACE("VfsWantedInit: can't register reg (%d)", ret);
        return NULL;
    }

    return drv;
}
