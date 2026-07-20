set(sources
    "wanted_malloc.c"
    "wanted.c"
    "wanted_wasm_api.c"
    "wanted-vfs-api.c"
    "log-store.c"
    "default_supervisor_cfg.json.h"
)

set(vfs-srcs
    "vfs/vfs.c"
    "vfs/vfs-9p.c"
    "vfs/vfs-config.c"
    "vfs/vfs-devfs.c"
    "vfs/vfs-netfs.c"
    "vfs/vfs-null.c"
    "vfs/vfs-log.c"
    "vfs/vfs-logmount.c"
    "vfs/vfs-pipe.c"
    "vfs/vfs-procfs.c"
    "vfs/vfs-procfs-wapps.c"
    "vfs/vfs-stdio.c"
    "vfs/vfs-tarfs.c"
    "vfs/vfs-socket.c"
    "vfs/vfs-sha256.c"
    "vfs/vfs-ed25519.c"
    "vfs/vfs-inflate.c"
    "vfs/vfs-wanted.c"
    "vfs/vfs-wanted-config.c"
    "vfs/vfs-wanted-wapps.c"
    "vfs/vfs-wanted-registry.c"
    "vfs/vfs-virtual.c"
    )

set(wasi-src
    "wasi/wasi-vfs.c"
)

# Add tiny-json to the build
set(tiny-json-src
    ${CMAKE_SOURCE_DIR}/vendor/tiny-json/tiny-json.c
)

# Add c9
set(c9-src
    ${CMAKE_SOURCE_DIR}/vendor/c9/c9.c
)

# Add uzlib (decompressor + checksums only; the compressor stays uncompiled)
set(uzlib-src
    ${CMAKE_SOURCE_DIR}/vendor/uzlib/src/tinflate.c
    ${CMAKE_SOURCE_DIR}/vendor/uzlib/src/crc32.c
    ${CMAKE_SOURCE_DIR}/vendor/uzlib/src/adler32.c
)

add_subdirectory(${CMAKE_SOURCE_DIR}/vendor/cwalk ${CMAKE_BINARY_DIR}/cwalk)

# Target platform selection. Picks the platform/<name> implementation linked by
# cmd/ and the WAMR platform layer. Linux is the default; nuttx is the embedded
# port.
set(WANTED_PLATFORM "linux" CACHE STRING "Target platform (linux, nuttx)")
set_property(CACHE WANTED_PLATFORM PROPERTY STRINGS linux nuttx)

# WAMR runtime
set(WAMR_ROOT_DIR ${CMAKE_SOURCE_DIR}/vendor/wamr)
set(WAMR_BUILD_PLATFORM "${WANTED_PLATFORM}")
set(WAMR_BUILD_INTERP 1)
set(WAMR_BUILD_FAST_INTERP 0)  # classic interpreter: smaller per-module footprint than fast-interp, which matters on MCU-class targets
set(WAMR_BUILD_AOT 0)
set(WAMR_BUILD_LIBC_WASI 0)
set(WAMR_BUILD_LIBC_BUILTIN 1)
set(WAMR_BUILD_MULTI_MODULE 0)
set(WAMR_BUILD_SHARED_MEMORY 0)
# Required by the cooperative wapp stop: only with the thread manager does
# setting the instance exception reach the interpreter's terminate check
# (WASM_SUSPEND_FLAG_TERMINATE, tested at every branch and loop back-edge).
# Without it the flag is a string nothing reads and a never-yielding wapp
# cannot be stopped.
set(WAMR_BUILD_THREAD_MGR 1)
include(${WAMR_ROOT_DIR}/build-scripts/runtime_lib.cmake)
add_library(vmlib STATIC ${WAMR_RUNTIME_LIB_SOURCE})
target_include_directories(vmlib INTERFACE
    ${WAMR_ROOT_DIR}/core/iwasm/include
)

set(WANTED_SUPERVISOR_IMAGE_PATH "./wasm/supervisor/sheriff/supervisor.tar" CACHE STRING
    "Supervisor TAR image path loaded at boot")

# Out-of-tree driver tree. Keeps a use-case-specific driver out of this repo
# while still linking it statically into a target: the tree is compiled as part
# of this build (source-level coupling, no binary ABI) and supplies
# ExtraDriverTable(), searched after the core and platform tables.
#
# The tree's CMakeLists.txt must define a library target named
# `wanted_extra_drivers`; the engine headers it needs are added to that target
# below. With the option unset, a default ExtraDriverTable() returning NULL is
# compiled in and the build is byte-identical to one built before this seam
# existed.
set(WANTED_EXTRA_DRIVERS_DIR "" CACHE PATH
    "Out-of-tree source dir supplying ExtraDriverTable()")

if (WANTED_EXTRA_DRIVERS_DIR)
    message("-- Extra drivers: ${WANTED_EXTRA_DRIVERS_DIR}")
    add_subdirectory(${WANTED_EXTRA_DRIVERS_DIR}
                     ${CMAKE_BINARY_DIR}/extra-drivers)
    target_include_directories(wanted_extra_drivers
        PRIVATE ${CMAKE_SOURCE_DIR}/include
        PRIVATE ${CMAKE_SOURCE_DIR}/src/include
        PRIVATE ${CMAKE_SOURCE_DIR}/platform/include
        PRIVATE ${WAMR_ROOT_DIR}/core/iwasm/include
        )
else()
    list(APPEND sources "extra-drivers-none.c")
endif()

add_library(${project} STATIC
    ${sources}
    ${vfs-srcs}
    ${wasi-src}
    ${tiny-json-src}
    ${c9-src}
    ${uzlib-src}
)

target_include_directories(${project}
    PUBLIC ${CMAKE_SOURCE_DIR}/include
    PRIVATE ${CMAKE_SOURCE_DIR}/platform/include
    PRIVATE ${CMAKE_SOURCE_DIR}/src/include
    PRIVATE ${CMAKE_SOURCE_DIR}/vendor/tiny-json
    PRIVATE ${CMAKE_SOURCE_DIR}/vendor/c9
    PRIVATE ${CMAKE_SOURCE_DIR}/vendor/uzlib/src
    PRIVATE ${WAMR_ROOT_DIR}/core/iwasm/include
    )



target_link_libraries(${project}
    PRIVATE cwalk
    PRIVATE vmlib
    PRIVATE m
    )

if (WANTED_EXTRA_DRIVERS_DIR)
    target_link_libraries(${project} PRIVATE wanted_extra_drivers)
endif()

message("-- Supervisor image: ${WANTED_SUPERVISOR_IMAGE_PATH}")
target_compile_definitions(${project} PRIVATE
    SUPERVISOR_IMAGE_PATH="${WANTED_SUPERVISOR_IMAGE_PATH}")

# Engine version string (git-derived SemVer), surfaced at /proc/wanted.
target_compile_definitions(${project} PRIVATE
    WANTED_VERSION="${SEMVER}")

# Local (AF_UNIX) transport for the 9P driver, letting an on-box driver server
# be reached over a filesystem socket. Hosted targets only — the lwIP-based MCU
# builds have no local socket family.
if (WANTED_PLATFORM STREQUAL "linux")
    target_compile_definitions(${project} PRIVATE WANTED_9P_UNIX_TRANSPORT=1)
endif()

# Add debug symbols
target_compile_options(${project} PUBLIC -g)

# Strict-warning guard on our own sources only — tiny-json / c9 (vendored) are
# compiled into this library and stay exempt.
set_source_files_properties(${sources} ${vfs-srcs} ${wasi-src}
    PROPERTIES COMPILE_OPTIONS "${WANTED_STRICT_WARNINGS}")

option(WANTED_DEBUG_TRACES "Enable debug prints for Wanted" OFF)
if (WANTED_DEBUG_TRACES)
    message("-- Debug tracing for Wanted enabled")
    target_compile_definitions(${project} PRIVATE DEBUG=1)
endif()


if (SECURE_SOCKETS)
    message("-- Building with support for secure sockets")
    target_compile_definitions(${project} PUBLIC SECURE_SOCKETS=1)
endif()
