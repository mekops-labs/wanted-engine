# WANTED engine as an ESP-IDF component.
#
# Mirrors the source set the NuttX built-in app compiles
# (third_party/nuttx-apps/system/wanted/Makefile): the platform-independent
# engine core + vendored libraries, plus a platform layer. The platform layer
# is the dummy (-ENOSYS) stub set for now; the real esp-idf layer replaces it
# incrementally. WAMR comes from the vendored esp-idf component (REQUIRES wamr).

set(ENGINE ${CMAKE_CURRENT_LIST_DIR}/../../../..)

set(core_srcs
    ${ENGINE}/src/wanted_malloc.c
    ${ENGINE}/src/wanted.c
    ${ENGINE}/src/wanted_wasm_api.c
    ${ENGINE}/src/wanted-vfs-api.c
    ${ENGINE}/src/log-store.c
    ${ENGINE}/src/extra-drivers-none.c
)

set(vfs_srcs
    vfs.c vfs-9p.c vfs-config.c vfs-devfs.c vfs-netfs.c vfs-null.c
    vfs-log.c vfs-logmount.c vfs-pipe.c vfs-procfs.c vfs-procfs-wapps.c
    vfs-stdio.c vfs-tarfs.c vfs-socket.c
    vfs-wanted.c vfs-wanted-config.c vfs-wanted-wapps.c
    vfs-wanted-registry.c vfs-virtual.c
    vfs-sha256.c vfs-ed25519.c vfs-inflate.c
)
list(TRANSFORM vfs_srcs PREPEND ${ENGINE}/src/vfs/)

set(wasi_srcs ${ENGINE}/src/wasi/wasi-vfs.c)

set(vendor_srcs
    ${ENGINE}/vendor/tiny-json/tiny-json.c
    ${ENGINE}/vendor/c9/c9.c
    ${ENGINE}/vendor/cwalk/src/cwalk.c
    ${ENGINE}/vendor/uzlib/src/tinflate.c
    ${ENGINE}/vendor/uzlib/src/crc32.c
    ${ENGINE}/vendor/uzlib/src/adler32.c
)

# Platform layer. Core primitives, the wapp lifecycle, the registry, sockets,
# and the extram (PSRAM) allocator are the real ESP-IDF implementations plus
# the shared POSIX layer; subsystems not yet ported (crypto) remain dummy
# stubs.
set(platform_srcs
    # ESP-IDF core primitives + wapp lifecycle
    ${ENGINE}/platform/esp-idf/memory.c
    ${ENGINE}/platform/esp-idf/random.c
    ${ENGINE}/platform/esp-idf/clock-sleep.c
    ${ENGINE}/platform/esp-idf/fs-volume.c
    ${ENGINE}/platform/esp-idf/wapps.c
    ${ENGINE}/platform/esp-idf/platform.c
    ${ENGINE}/platform/esp-idf/registry.c
    ${ENGINE}/platform/esp-idf/registry_flash.c
    ${ENGINE}/platform/esp-idf/vfs/vfs-esp-idf.c
    ${ENGINE}/platform/esp-idf/vfs/vfs-wifi.c
    # PlatformOta* on esp_ota_ops + the /dev/ota VFS driver.
    ${ENGINE}/platform/esp-idf/ota.c
    ${ENGINE}/platform/esp-idf/vfs/vfs-ota.c
    # Shared POSIX primitives (also compiled by the Linux/NuttX builds).
    # platform/posix/fs.c is NOT reused here: its PlatformFsRename/Mkdir call
    # renameat/mkdirat, which esp-idf's newlib declares but never links (no
    # *at() family in esp_vfs) — vfs-esp-idf.c provides esp-idf-native
    # PlatformOpenStateDir/FsRename/FsMkdir instead.
    ${ENGINE}/platform/posix/clock.c
    ${ENGINE}/platform/posix/mutex.c
    # lwIP exposes the same BSD socket API this file already targets (it is
    # also the NuttX platform's socket.c, unmodified); its TLS branches turn on
    # with SECURE_SOCKETS, set below from the engine's configuration.
    ${ENGINE}/platform/posix/socket.c
    # PlatformSha256* backed by mbedtls_sha256_context, which the ESP32-S3
    # SHA hardware peripheral transparently accelerates (MBEDTLS_HARDWARE_SHA
    # is on by default for this SoC) -- unlike ECC/ECDSA, this chip has no
    # peripheral gap to work around.
    ${ENGINE}/platform/esp-idf/sha256.c
    # PlatformExtram* routes WantedMalloc into PSRAM (heap_caps_malloc).
    ${ENGINE}/platform/esp-idf/extram.c
    # Not-yet-ported subsystems
    ${ENGINE}/platform/dummy/dummy-crypto.c
)

# Engine build configuration. The idf.py build is not the engine's top-level
# project, so point the shared Kconfig step at the engine root and let it
# generate wanted-autoconf.h here. The OTA profile below selects the envelope,
# so MAX_WAPPS and the flash layout cannot disagree.
set(WANTED_ENGINE_ROOT ${ENGINE})

# ESP-IDF OTA flash-layout profile: couples MAX_WAPPS (overriding the
# WANTED_PROFILE value above), WAPP_IMAGE_MAX_SLOTS (config-esp-idf.h
# aliases it directly to MAX_WAPPS), and the generated partitions.csv from
# one source. Two named profiles: s3-wapps (default, MAX_WAPPS=20, max
# concurrency) and s3-storage (MAX_WAPPS=10, roomier persist partition).
# Only MAX_WAPPS varies; slot size is fixed at 160 KiB across both.
if(NOT OTA_PROFILE)
    set(OTA_PROFILE "s3-wapps")
endif()
include(${ENGINE}/platform/esp-idf/ota-profiles/${OTA_PROFILE}.cmake)

# The OTA profile above names the engine defconfig; generating it here is what
# makes CONFIG_WANTED_MAX_WAPPS below the same number the header carries.
include(${ENGINE}/cmake/Kconfig.cmake)

# Fixed flash geometry (8 MB XIAO ESP32-S3): reserved 64 KiB (bootloader +
# partition table + nvs 24 KiB + phy_init 4 KiB) + otadata 8 KiB + two OTA
# app slots. 1600 KiB slots hold >=35% headroom over the measured,
# OTA-code-included image (1 171 072 B, 2026-07-07) -- 1536 KiB (the
# pre-OTA-code estimate) no longer clears that margin once this driver's own
# code is counted, before the pending gpio driver or a real Ed25519 backend
# land (floor-measured gpio stub: +832 B; Ed25519 unmeasured, budgeted
# generously). Must stay a multiple of 64 KiB so ota_0/ota_1 auto-align with
# zero padding waste -- see partitions.csv.in's layout comment.
set(OTA_SLOT_KB 1600)
math(EXPR OTA_POOL_KB "8192 - 64 - 8 - (2 * ${OTA_SLOT_KB})")

set(WAPP_IMAGE_SLOT_SIZE_KB 160)
math(EXPR OTA_WAPPS_KB "${CONFIG_WANTED_MAX_WAPPS} * ${WAPP_IMAGE_SLOT_SIZE_KB}")
math(EXPR OTA_PERSIST_KB "${OTA_POOL_KB} - ${OTA_WAPPS_KB}")

# Build-time assert on the coupled quad: this is the derivation itself (not
# an independent re-check), but it still catches a future OTA_SLOT_KB/
# OTA_PROFILE edit that pushes wapps past the pool.
if(OTA_WAPPS_KB GREATER OTA_POOL_KB)
    message(FATAL_ERROR
        "OTA_PROFILE=${OTA_PROFILE}: CONFIG_WANTED_MAX_WAPPS=${CONFIG_WANTED_MAX_WAPPS} needs "
        "${OTA_WAPPS_KB} KiB of wapps, exceeding the ${OTA_POOL_KB} KiB "
        "persist+wapps pool (OTA_SLOT_KB=${OTA_SLOT_KB})")
endif()

# Regenerated on every configure -- partitions.csv is a build artifact (like
# sdkconfig from sdkconfig.defaults), not a hand-maintained file. Runs during
# the cmake configure phase, strictly before the ninja build step that reads
# CONFIG_PARTITION_TABLE_CUSTOM_FILENAME.
configure_file(
    ${ENGINE}/platform/esp-idf/project/partitions.csv.in
    ${ENGINE}/platform/esp-idf/project/partitions.csv
    @ONLY
)

# EMBED_FILES links the compiled-in default supervisor (wsh) straight into the
# app binary as a linker section (`_binary_supervisor_tar_{start,end}`) —
# platform.c's PlatformWappLoad reads it directly, zero-copy. The ESP-IDF
# equivalent of the NuttX port's boot ROMFS (platform/nuttx/app/
# wanted_esp32_main.c), since ESP-IDF has no host filesystem to stage a
# supervisor image on. Prerequisite: `just supervisor`
# (wasm/supervisor/wsh/supervisor.tar). Embedded here rather than by the
# `main` component so platform.c and the embed live in the same static
# library — a symbol EMBED_FILES defines in one component's archive is not
# visible to a different component's archive without
# --start-group/--end-group, which is exactly the link failure that surfaced
# this constraint.
idf_component_register(
    SRCS ${core_srcs} ${vfs_srcs} ${wasi_srcs} ${vendor_srcs} ${platform_srcs}
    INCLUDE_DIRS ${ENGINE}/include ${WANTED_AUTOCONF_DIR}
    PRIV_INCLUDE_DIRS
        ${ENGINE}/src/include
        ${ENGINE}/platform/include
        ${ENGINE}/platform/esp-idf/include
        ${ENGINE}/vendor/tiny-json
        ${ENGINE}/vendor/c9
        ${ENGINE}/vendor/cwalk/include
        ${ENGINE}/vendor/uzlib/src
    REQUIRES wamr esp_hw_support pthread esp_partition lwip esp_wifi esp_event nvs_flash mbedtls app_update esp_timer joltwallet__littlefs
    EMBED_FILES ${ENGINE}/wasm/supervisor/wsh/supervisor.tar
)

# TLS backend for this platform: the engine asks through its own Kconfig, and
# ESP-IDF answers with the mbedTLS it already ships. Previously pinned on here,
# which meant a configuration that deselected secure sockets still built and
# linked them.
if(CONFIG_WANTED_VFS_SOCKET_TLS)
    set(_secure_sockets 1)
    target_sources(${COMPONENT_LIB} PRIVATE ${ENGINE}/platform/posix/ssocket-mbedtls.c)
else()
    set(_secure_sockets 0)
endif()

# Build-wide, not component-private: the host guard is reached from any source
# including wanted-api.h — app_main.c among them — and a component that saw the
# configuration symbol but not this define would fail the guard for no reason.
idf_build_set_property(COMPILE_DEFINITIONS "SECURE_SOCKETS=${_secure_sockets}" APPEND)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    PLATFORM_ESP_IDF=1
    SUPERVISOR_IMAGE_PATH="embedded:supervisor.tar"
)
