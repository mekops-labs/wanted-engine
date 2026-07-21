/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <vfs.h>
#include <wanted-api.h>
#include <wanted-autoconf.h>

#ifndef SECURE_SOCKETS
#define SECURE_SOCKETS 0
#endif

enum vfs_socket_type_t {
    VFS_SKT_TCP,
    VFS_SKT_UDP,
    VFS_SKT_STCP,   /* secure tcp */
    VFS_SKT_SUDP,   /* secure udp */
    VFS_SKT_SERIAL, /* any point-to-point byte-stream device reachable as a
                     * plain character device - UART, USB-CDC, or an ISM/LoRa
                     * module's AT-command UART bridge - e.g.
                     * "serial:///dev/ttyACM0" - no port. Assumes a reliable,
                     * ordered byte stream: a raw packet-radio link with no
                     * UART bridge (lossy, size-limited, no ordering
                     * guarantee) needs a framing/retry layer on top of this,
                     * not provided here. */
};

typedef vfs_driver_t *(*VfsInitFunction_t)(const wapp_t *wapp,
                                           const char *options);

typedef struct vfs_driver_table_t {
    char *name;
    VfsInitFunction_t init;
} vfs_driver_table_t;

/* Driver table contributed by an out-of-tree source tree, NULL-terminated; may
 * return NULL or an empty table. Lets a use-case-specific driver — one not
 * worth carrying in the engine — be statically linked into a target without
 * living in this repo. The coupling is source-level, not a binary ABI: the
 * tree is compiled against these headers as part of the same build.
 *
 * Searched last, after the core and platform tables, so an extra driver can
 * never shadow a core name. It is nonetheless engine-privilege code — a fault
 * in it faults the engine — so it belongs to the same trust boundary as any
 * compiled-in driver. Wire a tree in with -DWANTED_EXTRA_DRIVERS_DIR=<path>;
 * builds without one link a default returning NULL. */
const vfs_driver_table_t *ExtraDriverTable(void);

/* Always compiled in. */
vfs_driver_t *VfsNullInit(const wapp_t *wapp, const char *options);
vfs_driver_t *VfsLogInit(const wapp_t *wapp, const char *options);
vfs_driver_t *VfsPlatformFsInit(const wapp_t *wapp, const char *options,
                                bool readonly);
vfs_driver_t *VfsVirtualInit(const wapp_t *wapp, const char *options);

/* Configurable. Each declaration is gated with its source so a call from a
 * build that deselected the driver fails at compile time, naming the symbol,
 * rather than at link time. */
#ifdef CONFIG_WANTED_VFS_LOGMOUNT
vfs_driver_t *VfsLogMountInit(const wapp_t *wapp, const char *options);
#endif
#ifdef CONFIG_WANTED_VFS_9P
vfs_driver_t *Vfs9PInit(const wapp_t *wapp, const char *options);
#endif
#ifdef CONFIG_WANTED_VFS_CONFIG
vfs_driver_t *VfsConfigInit(const wapp_t *wapp, const char *options);
#endif
#ifdef CONFIG_WANTED_VFS_SOCKET
vfs_driver_t *VfsSocketInit(const wapp_t *wapp, const char *options);
#endif
#ifdef CONFIG_WANTED_VFS_SHA256
vfs_driver_t *VfsSha256Init(const wapp_t *wapp, const char *options);
#endif
#ifdef CONFIG_WANTED_VFS_ED25519
vfs_driver_t *VfsEd25519Init(const wapp_t *wapp, const char *options);
#endif
#ifdef CONFIG_WANTED_VFS_INFLATE
vfs_driver_t *VfsInflateInit(const wapp_t *wapp, const char *options);
#endif
vfs_driver_t *VfsGpioInit(const wapp_t *wapp, const char *options);
vfs_driver_t *VfsWifiInit(const wapp_t *wapp, const char *options);
vfs_driver_t *VfsOtaInit(const wapp_t *wapp, const char *options);
vfs_driver_t *VfsWantedInit(const wapp_t *wapp, const char *options);
