/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <wanted-autoconf.h>

/* Consistency checks for a host that builds the engine as an external package
 * (OpenWrt), where no shared Kconfig namespace cross-checks the two. Hosts that
 * compile engine sources into their own tree get this from Kconfig instead.
 *
 * A preprocessor sees defines, not the image around it: it catches a
 * self-contradictory configuration or a missing Kconfig step, never a missing
 * host library — that surfaces at link time.
 */

/* The generated header must have reached this TU; without it every limit would
 * compile as 0 or fail much later with a confusing error. */
#ifndef CONFIG_WANTED_MAX_WAPPS
#error                                                                         \
    "wanted-autoconf.h is absent or empty: this build did not run the Kconfig step. Every build path must generate the header (see cmake/Kconfig.cmake, or the NuttX app Makefile for a non-CMake example)."
#endif

/* TLS belongs to the socket driver; without it the build carries a TLS link
 * dependency nothing can reach. */
#if defined(SECURE_SOCKETS) && SECURE_SOCKETS != 0
#ifndef CONFIG_WANTED_VFS_SOCKET
#error                                                                         \
    "SECURE_SOCKETS is on but the socket driver is deselected: TLS has nothing to secure. Enable CONFIG_WANTED_VFS_SOCKET or build without secure sockets."
#endif
#endif

/* The other direction, and the one that fails quietly: the configuration asked
 * for secure sockets but this host supplied no backend, so tcps:// and udps://
 * would be rejected at wapp launch on a build that believes it has TLS. */
#ifdef CONFIG_WANTED_VFS_SOCKET_TLS
#if !defined(SECURE_SOCKETS) || SECURE_SOCKETS == 0
#error                                                                         \
    "CONFIG_WANTED_VFS_SOCKET_TLS is set but this build has no TLS backend. Linux needs OpenSSL; NuttX and ESP-IDF need their host's mbedTLS enabled. Deselect it to build without secure sockets."
#endif
#endif

/* The supervisor occupies a slot. */
#if CONFIG_WANTED_MAX_WAPPS < 1
#error                                                                         \
    "CONFIG_WANTED_MAX_WAPPS must be at least 1: the supervisor itself occupies a slot."
#endif
