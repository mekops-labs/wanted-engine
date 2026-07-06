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

# Platform layer. Core primitives, the wapp lifecycle, the registry, and
# sockets are the real ESP-IDF implementations plus the shared POSIX layer;
# subsystems not yet ported (crypto, extram) remain dummy stubs until their
# milestones.
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
    # Shared POSIX primitives (also compiled by the Linux/NuttX builds).
    # platform/posix/fs.c is NOT reused here: its PlatformFsRename/Mkdir call
    # renameat/mkdirat, which esp-idf's newlib declares but never links (no
    # *at() family in esp_vfs) — vfs-esp-idf.c provides esp-idf-native
    # PlatformOpenStateDir/FsRename/FsMkdir instead.
    ${ENGINE}/platform/posix/clock.c
    ${ENGINE}/platform/posix/mutex.c
    # lwIP exposes the same BSD socket API this file already targets (it is
    # also the NuttX platform's socket.c, unmodified) — SECURE_SOCKETS=0 below
    # compiles out its TLS branches, same as on NuttX.
    ${ENGINE}/platform/posix/socket.c
    # Not-yet-ported subsystems
    ${ENGINE}/platform/dummy/dummy-crypto.c
    ${ENGINE}/platform/dummy/dummy-extram.c
)

# Engine resource limits. The idf.py build bypasses the top-level CMakeLists
# profile loop, so resolve a profile here and turn its limits into compile
# definitions (each is #ifndef-guarded in wanted-config.h). Defaults to the S3 +
# PSRAM profile; override with -DWANTED_PROFILE=<name> from cmake/profiles/.
if(NOT WANTED_PROFILE)
    set(WANTED_PROFILE "psram-s3")
endif()
include(${ENGINE}/cmake/profiles/${WANTED_PROFILE}.cmake)
set(_limit_defs "")
foreach(_limit MAX_WAPPS WASM_STACK_SIZE WASM_HEAP_SIZE WASM_MAX_MEMORY_PAGES
               WASM_WORKER_STACK_SIZE MAX_PATH_LEN MAX_DRIVERS_CNT MAX_OPTIONS_SIZE)
    if(DEFINED ${_limit})
        list(APPEND _limit_defs ${_limit}=${${_limit}})
    endif()
endforeach()

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
    INCLUDE_DIRS ${ENGINE}/include
    PRIV_INCLUDE_DIRS
        ${ENGINE}/src/include
        ${ENGINE}/platform/include
        ${ENGINE}/platform/esp-idf/include
        ${ENGINE}/vendor/tiny-json
        ${ENGINE}/vendor/c9
        ${ENGINE}/vendor/cwalk/include
        ${ENGINE}/vendor/uzlib/src
    REQUIRES wamr esp_hw_support pthread esp_partition lwip esp_wifi esp_event nvs_flash
    EMBED_FILES ${ENGINE}/wasm/supervisor/wsh/supervisor.tar
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    PLATFORM_ESP_IDF=1
    SECURE_SOCKETS=0
    SUPERVISOR_IMAGE_PATH="embedded:supervisor.tar"
    ${_limit_defs}
)
