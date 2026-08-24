# WANTED engine as an ESP-IDF component. Mirrors the source set the NuttX
# built-in app compiles (third_party/nuttx-apps/system/wanted/Makefile).
# WAMR comes from the vendored esp-idf component (REQUIRES wamr).

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
    # ed25519 (orlp/ed25519, verify-only): backs PlatformEd25519Verify
    # (crypto.c) -- no ECC peripheral / mbedTLS Ed25519 on this chip.
    ${ENGINE}/vendor/ed25519/src/verify.c
    ${ENGINE}/vendor/ed25519/src/sha512.c
    ${ENGINE}/vendor/ed25519/src/ge.c
    ${ENGINE}/vendor/ed25519/src/sc.c
    ${ENGINE}/vendor/ed25519/src/fe.c
)

# Platform layer. Core primitives, the wapp lifecycle, the registry, sockets,
# crypto, and the extram (PSRAM) allocator are the real ESP-IDF
# implementations plus the shared POSIX layer.
set(platform_srcs
    # ESP-IDF core primitives + wapp lifecycle
    ${ENGINE}/platform/esp-idf/memory.c
    ${ENGINE}/platform/esp-idf/random.c
    ${ENGINE}/platform/esp-idf/clock-sleep.c
    ${ENGINE}/platform/esp-idf/fs-volume.c
    ${ENGINE}/platform/esp-idf/wapps.c
    ${ENGINE}/platform/esp-idf/wake.c
    ${ENGINE}/platform/esp-idf/persist-mem.c
    ${ENGINE}/platform/esp-idf/reset-reason.c
    ${ENGINE}/platform/esp-idf/platform.c
    ${ENGINE}/platform/esp-idf/registry.c
    ${ENGINE}/platform/esp-idf/registry_flash.c
    ${ENGINE}/platform/esp-idf/vfs/vfs-esp-idf.c
    ${ENGINE}/platform/esp-idf/vfs/vfs-wifi.c
    # Shared POSIX primitives (also compiled by Linux/NuttX). vfs-esp-idf.c
    # provides PlatformOpenStateDir/FsRename/FsMkdir natively: esp-idf's
    # newlib declares renameat/mkdirat but never links them.
    ${ENGINE}/platform/posix/clock.c
    ${ENGINE}/platform/posix/mutex.c
    # lwIP exposes the same BSD socket API this file already targets (it is
    # also the NuttX platform's socket.c, unmodified); its TLS branches turn on
    # with SECURE_SOCKETS, set below from the engine's configuration.
    ${ENGINE}/platform/posix/socket.c
    # PlatformSha256* backed by mbedtls_sha256_context, transparently
    # accelerated by the ESP32-S3's SHA peripheral (on by default).
    ${ENGINE}/platform/esp-idf/sha256.c
    # PlatformEd25519Verify over the vendored ed25519 (in vendor_srcs).
    ${ENGINE}/platform/esp-idf/crypto.c
    # PlatformExtram* routes WantedMalloc into PSRAM (heap_caps_malloc).
    ${ENGINE}/platform/esp-idf/extram.c
)

# Engine version string (git-derived SemVer), surfaced at /proc/wanted.
# This component compiles the engine core itself, so nothing else
# supplies it — without it every board reports "unknown".
include(${ENGINE}/cmake/VersionFromGit.cmake)
version_from_git(TIMESTAMP "%Y%m%d%H%M%S")
if(NOT SEMVER)
    message(FATAL_ERROR
        "engine version unavailable: no git tag reachable from this checkout. "
        "A firmware that reports no version cannot be confirmed after an "
        "update -- tag the tree, or fetch its tags.")
endif()

# The idf.py build is not the engine's top-level project, so point the
# shared Kconfig step at the engine root and generate wanted-autoconf.h
# here — board-defconfig.cmake below then selects the same envelope.
set(WANTED_ENGINE_ROOT ${ENGINE})

# Board defconfig + OTA flash-layout: couples MAX_WAPPS, the registry image
# ceiling and the generated partitions.csv from one source.
include(${ENGINE}/platform/esp-idf/board-defconfig.cmake)

# The include above names the engine defconfig; generating it here is what
# makes CONFIG_WANTED_MAX_WAPPS below the same number the header carries.
include(${ENGINE}/cmake/Kconfig.cmake)

# Configurable drivers, gated like src/CMakeLists.txt. Their component
# REQUIRES stay unconditional: ESP-IDF resolves REQUIRES before .config
# exists, so one gated on CONFIG_WANTED_VFS_* never reaches the resolver.
set(driver_requires esp_driver_gpio esp_driver_uart app_update)
if(CONFIG_WANTED_VFS_GPIO)
    list(APPEND vfs_srcs ${ENGINE}/src/vfs/vfs-gpio.c)
    list(APPEND platform_srcs ${ENGINE}/platform/esp-idf/gpio.c)
endif()
if(CONFIG_WANTED_VFS_UART)
    list(APPEND vfs_srcs ${ENGINE}/src/vfs/vfs-uart.c)
    list(APPEND platform_srcs ${ENGINE}/platform/esp-idf/uart.c)
endif()
if(CONFIG_WANTED_VFS_OTA)
    list(APPEND vfs_srcs ${ENGINE}/src/vfs/vfs-ota.c)
    list(APPEND platform_srcs ${ENGINE}/platform/esp-idf/ota.c)
endif()

if(WANTED_OTA_LAYOUT STREQUAL "factory")
    # 4 MB flash (classic ESP32): 64 KiB reserved, one factory app slot,
    # no A/B; persist+wapps take the remainder. Must stay a multiple of
    # 64 KiB so persist/wapps auto-align with zero padding waste.
    set(FACTORY_APP_KB 1536)
    math(EXPR OTA_POOL_KB "4096 - 64 - ${FACTORY_APP_KB}")
    set(_partition_template ${ENGINE}/platform/esp-idf/project/partitions-factory.csv.in)
else()
    # 8 MB XIAO ESP32-S3: 64 KiB reserved + otadata 8 KiB + two app slots, and
    # persist takes what is left. A multiple of 64 KiB, or ota_1 pads up to the
    # next boundary and the table runs past the end of flash.
    set(OTA_SLOT_KB 1792)
    math(EXPR OTA_POOL_KB "8192 - 64 - 8 - (2 * ${OTA_SLOT_KB})")
    set(_partition_template ${ENGINE}/platform/esp-idf/project/partitions.csv.in)
endif()

# Slot size is the engine's image ceiling, so the compiled-in bound and the
# partition are the same number. Slots are erased in place, so a size that is
# not a whole number of 4 KiB sectors would put two slots in one sector.
set(WAPP_IMAGE_SLOT_SIZE_KB ${CONFIG_WANTED_MAX_WAPP_IMAGE_KB})
math(EXPR SLOT_SECTOR_REMAINDER "${WAPP_IMAGE_SLOT_SIZE_KB} % 4")
if(NOT SLOT_SECTOR_REMAINDER EQUAL 0)
    message(FATAL_ERROR
        "CONFIG_WANTED_MAX_WAPP_IMAGE_KB=${WAPP_IMAGE_SLOT_SIZE_KB} is not a "
        "multiple of the 4 KiB flash sector")
endif()

math(EXPR OTA_WAPPS_KB "${CONFIG_WANTED_MAX_WAPPS} * ${WAPP_IMAGE_SLOT_SIZE_KB}")
math(EXPR OTA_PERSIST_KB "${OTA_POOL_KB} - ${OTA_WAPPS_KB}")

# Build-time assert on the coupled quad: this is the derivation itself (not
# an independent re-check), but it still catches a future slot-size/
# defconfig edit that pushes wapps past the pool.
if(OTA_WAPPS_KB GREATER OTA_POOL_KB)
    message(FATAL_ERROR
        "WANTED_DEFCONFIG=${WANTED_DEFCONFIG}: CONFIG_WANTED_MAX_WAPPS=${CONFIG_WANTED_MAX_WAPPS} at "
        "${WAPP_IMAGE_SLOT_SIZE_KB} KiB per image needs ${OTA_WAPPS_KB} KiB of wapps, "
        "exceeding the ${OTA_POOL_KB} KiB persist+wapps pool")
endif()

# Regenerated on every configure — a build artifact, not hand-maintained.
# Runs during cmake configure, before the ninja step reads
# CONFIG_PARTITION_TABLE_CUSTOM_FILENAME.
configure_file(
    ${_partition_template}
    ${ENGINE}/platform/esp-idf/project/partitions.csv
    @ONLY
)

# EMBED_FILES links the supervisor in as a linker section that
# PlatformWappLoad reads zero-copy. Embedded here, not by `main`, so the
# symbol shares platform.c's archive — a different one can't see it.
string(REGEX REPLACE "^\\./" "" _supervisor_rel "${CONFIG_WANTED_SUPERVISOR_IMAGE}")
set(_supervisor_tar "${ENGINE}/${_supervisor_rel}")
if(NOT EXISTS "${_supervisor_tar}")
    message(FATAL_ERROR
        "supervisor image missing: ${_supervisor_tar}\n"
        "Build it with `make sheriff`, or `make supervisor` for wsh/selftest.")
endif()
message(STATUS "Supervisor image: ${_supervisor_tar}")
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
        ${ENGINE}/vendor/ed25519/src
    REQUIRES wamr esp_hw_support pthread esp_partition lwip esp_wifi esp_event nvs_flash mbedtls esp_timer joltwallet__littlefs ${driver_requires}
    EMBED_FILES ${_supervisor_tar}
)

# TLS backend for this platform: the engine asks through its own
# Kconfig, and ESP-IDF answers with the mbedTLS it already ships.
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
    WANTED_VERSION="${SEMVER}"
    PLATFORM_ESP_IDF=1
    SUPERVISOR_IMAGE_PATH="embedded:supervisor.tar"
)
