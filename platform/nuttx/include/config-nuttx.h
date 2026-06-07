#pragma once

#define DEFAULT_ROOT "./"

#define REGISTRY_ROOT "./wapps"
#define REGISTRY_EXT ".wapp"
#define REGISTRY_VERSION_SEPARATOR ':'

/* Upper bound on registry entries materialised when resolving a wapp whose
 * version is unspecified. Embedded targets avoid VLAs, so this caps the
 * on-stack scan buffer. */
#define REGISTRY_MAX_ENTRIES 50
