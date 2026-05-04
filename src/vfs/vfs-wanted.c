#include <debug_trace.h>
#include <vfs-drivers.h>
#include <vfs.h>

extern const vfs_driver_t WantedConfigDriver;
extern const vfs_driver_t WantedControlDriver;
extern const vfs_driver_t WantedRegistryDriver;
extern const vfs_driver_t WantedWappsDriver;

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
    ret = drv->Register(drv->ctx, "ctrl", &WantedControlDriver);
    if (ret < 0) {
        DEBUG_TRACE("can't register ctrl (%d)", ret);
        return NULL;
    }
    ret = drv->Register(drv->ctx, "reg", &WantedRegistryDriver);
    if (ret < 0) {
        DEBUG_TRACE("can't register reg (%d)", ret);
        return NULL;
    }
    ret = drv->Register(drv->ctx, "wapps", &WantedWappsDriver);
    if (ret < 0) {
        DEBUG_TRACE("can't register wapps (%d)", ret);
        return NULL;
    }

    /* "w" subdirectory: backward-compat for pre-overhaul paths "w/ctrl" and
     * "w/reg" used by the sheriff supervisor binary. */
    vfs_driver_t *w_compat = VfsVirtualInit(wapp, NULL);
    if (NULL == w_compat) {
        DEBUG_TRACE("can't load w compat virtual driver");
        return NULL;
    }
    ret = w_compat->Register(w_compat->ctx, "ctrl", &WantedControlDriver);
    if (ret < 0) {
        DEBUG_TRACE("can't register w/ctrl (%d)", ret);
        return NULL;
    }
    ret = w_compat->Register(w_compat->ctx, "reg", &WantedRegistryDriver);
    if (ret < 0) {
        DEBUG_TRACE("can't register w/reg (%d)", ret);
        return NULL;
    }
    ret = drv->Register(drv->ctx, "w", w_compat);
    if (ret < 0) {
        DEBUG_TRACE("can't register w compat dir (%d)", ret);
        return NULL;
    }

    return drv;
}
