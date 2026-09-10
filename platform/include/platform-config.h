/* SPDX-License-Identifier: Apache-2.0 */

/* Platform-independent engine paths and registry constants, shared by every
 * platform's config header. */

#ifndef PLATFORM_CONFIG_H
#define PLATFORM_CONFIG_H

#define DEFAULT_ROOT "./"

#define REGISTRY_ROOT "./registry"
#define VOLUME_ROOT "./data"
#define REGISTRY_EXT ".wapp"
/* Sidecar holding one entry's metadata record, beside its image. Enumeration
 * lists REGISTRY_EXT only, so a sidecar is never an entry of its own. */
#define REGISTRY_META_EXT ".meta"
/* Separates name and version in a registry image ref and on-disk filename
 * ("<name>@<version>.wapp"). Must be valid in every backing filesystem. */
#define REGISTRY_VERSION_SEPARATOR '@'

#endif /* PLATFORM_CONFIG_H */
