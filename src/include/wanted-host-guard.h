/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <wanted-autoconf.h>

/* Consistency checks between the engine's configuration and the host it is
 * compiled into. Hosts that compile engine sources into their own Kconfig tree
 * (NuttX, ESP-IDF) get the edges enforced at configure time; hosts that build
 * the engine as an external package (OpenWrt) share no symbol namespace, so
 * the preprocessor is what they get.
 *
 * A preprocessor check reads defines, not the host image being linked. It
 * catches a self-contradictory configuration or a build path that never
 * generated the header; it cannot tell that libmbedtls is missing — that
 * surfaces at link time, and no #error can pull it earlier.
 */

/* The generated header must have reached this TU. A path that skipped the
 * Kconfig step would otherwise compile every limit as 0 or fail later with
 * a confusing error. */
#ifndef CONFIG_WANTED_MAX_WAPPS
#error                                                                         \
    "wanted-autoconf.h is absent or empty: this build did not run the Kconfig step. Every build path must generate the header (see cmake/Kconfig.cmake, or the NuttX app Makefile for a non-CMake example)."
#endif

/* TLS is a property of the socket driver. Selecting secure sockets without
 * the driver means a link dependency that nothing can reach. */
#if defined(SECURE_SOCKETS) && SECURE_SOCKETS != 0
#ifndef CONFIG_WANTED_VFS_SOCKET
#error                                                                         \
    "SECURE_SOCKETS is on but the socket driver is deselected: TLS has nothing to secure. Enable CONFIG_WANTED_VFS_SOCKET or build without secure sockets."
#endif
#endif

/* The supervisor occupies a slot. With one slot the engine can run the
 * supervisor and nothing else — legitimate but easy to do by accident. */
#if CONFIG_WANTED_MAX_WAPPS < 1
#error                                                                         \
    "CONFIG_WANTED_MAX_WAPPS must be at least 1: the supervisor itself occupies a slot."
#endif
