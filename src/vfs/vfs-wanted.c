/* SPDX-License-Identifier: Apache-2.0 */

#include <debug_trace.h>
#include <vfs-drivers.h>
#include <vfs.h>

extern const vfs_driver_t WantedConfigDriver;
extern const vfs_driver_t WantedRegistryDriver;
extern const vfs_driver_t WantedWappsDriver;
extern const vfs_driver_t WantedCtlDriver;

vfs_driver_t *VfsWantedInit(const wapp_t *wapp, const char *opt) {
    int ret = 0;
    vfs_driver_t *drv;

    drv = VfsVirtualInit(wapp, opt);
    if (NULL == drv) {
        DEBUG_TRACE("can't load virtual driver (%d)", ret);
        return NULL;
    }
    ret = drv->Register(drv->ctx, "config", &WantedConfigDriver);
    if (ret < 0) {
        DEBUG_TRACE("can't register config (%d)", ret);
        return NULL;
    }
    ret = drv->Register(drv->ctx, "reg", &WantedRegistryDriver);
    if (ret < 0) {
        DEBUG_TRACE("can't register reg (%d)", ret);
        return NULL;
    }
    /* Control plane: a path-addressed per-wapp namespace (wapps/<name>/...)
     * plus a root create-and-launch node (ctl). */
    ret = drv->Register(drv->ctx, "ctl", &WantedCtlDriver);
    if (ret < 0) {
        DEBUG_TRACE("can't register ctl (%d)", ret);
        return NULL;
    }
    ret = drv->Register(drv->ctx, "wapps", &WantedWappsDriver);
    if (ret < 0) {
        DEBUG_TRACE("can't register wapps (%d)", ret);
        return NULL;
    }

    return drv;
}
