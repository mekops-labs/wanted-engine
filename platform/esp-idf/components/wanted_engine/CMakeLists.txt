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

# Platform layer: dummy (-ENOSYS) stubs. Replaced by the real esp-idf layer in
# later milestones.
set(platform_srcs
    ${ENGINE}/platform/dummy/dummy.c
    ${ENGINE}/platform/dummy/dummy-crypto.c
    ${ENGINE}/platform/dummy/dummy-fs.c
    ${ENGINE}/platform/dummy/dummy-extram.c
    ${ENGINE}/platform/dummy/dummy-registry.c
    ${ENGINE}/platform/dummy/dummy-wapps.c
    ${ENGINE}/platform/dummy/dummy-net.c
)

idf_component_register(
    SRCS ${core_srcs} ${vfs_srcs} ${wasi_srcs} ${vendor_srcs} ${platform_srcs}
    INCLUDE_DIRS ${ENGINE}/include
    PRIV_INCLUDE_DIRS
        ${ENGINE}/src/include
        ${ENGINE}/platform/include
        ${ENGINE}/vendor/tiny-json
        ${ENGINE}/vendor/c9
        ${ENGINE}/vendor/cwalk/include
        ${ENGINE}/vendor/uzlib/src
    REQUIRES wamr
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    PLATFORM_ESP_IDF=1
    SECURE_SOCKETS=0
    SUPERVISOR_IMAGE_PATH="/rom/wanted/supervisor.tar"
)
