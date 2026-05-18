set(sources
    "wanted_malloc.c"
    "wanted.c"
    "wanted_wasm_api.c"
    "wanted-vfs-api.c"
    "default_supervisor_cfg.json.h"
)

set(vfs-srcs
    "vfs/vfs.c"
    "vfs/vfs-9p.c"
    "vfs/vfs-config.c"
    "vfs/vfs-devfs.c"
    "vfs/vfs-netfs.c"
    "vfs/vfs-null.c"
    "vfs/vfs-pipe.c"
    "vfs/vfs-procfs.c"
    "vfs/vfs-stdio.c"
    "vfs/vfs-tarfs.c"
    "vfs/vfs-socket.c"
    "vfs/vfs-wanted.c"
    "vfs/vfs-wanted-config.c"
    "vfs/vfs-wanted-ctrl.c"
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

set(json-maker-src
    ${CMAKE_SOURCE_DIR}/vendor/json-maker/src/json-maker.c
)

# Add c9
set(c9-src
    ${CMAKE_SOURCE_DIR}/vendor/c9/c9.c
)

add_subdirectory(${CMAKE_SOURCE_DIR}/vendor/cwalk ${CMAKE_BINARY_DIR}/cwalk)

# WAMR runtime
set(WAMR_ROOT_DIR ${CMAKE_SOURCE_DIR}/vendor/wamr)
set(WAMR_BUILD_PLATFORM "linux")
set(WAMR_BUILD_INTERP 1)
set(WAMR_BUILD_AOT 0)
set(WAMR_BUILD_LIBC_WASI 0)
set(WAMR_BUILD_LIBC_BUILTIN 1)
set(WAMR_BUILD_MULTI_MODULE 0)
set(WAMR_BUILD_SHARED_MEMORY 0)
include(${WAMR_ROOT_DIR}/build-scripts/runtime_lib.cmake)
add_library(vmlib STATIC ${WAMR_RUNTIME_LIB_SOURCE})
target_include_directories(vmlib INTERFACE
    ${WAMR_ROOT_DIR}/core/iwasm/include
)

set(WANTED_SUPERVISOR_IMAGE_PATH "./wasm/supervisor/sheriff/supervisor.tar" CACHE STRING
    "Supervisor TAR image path loaded at boot")

add_library(${project} STATIC
    ${sources}
    ${vfs-srcs}
    ${wasi-src}
    ${tiny-json-src}
    ${json-maker-src}
    ${c9-src}
)

target_include_directories(${project}
    PUBLIC ${CMAKE_SOURCE_DIR}/include
    PRIVATE ${CMAKE_SOURCE_DIR}/platform/include
    PRIVATE ${CMAKE_SOURCE_DIR}/src/include
    PRIVATE ${CMAKE_SOURCE_DIR}/vendor/tiny-json
    PRIVATE ${CMAKE_SOURCE_DIR}/vendor/json-maker/src/include
    PRIVATE ${CMAKE_SOURCE_DIR}/vendor/c9
    PRIVATE ${WAMR_ROOT_DIR}/core/iwasm/include
    )



target_link_libraries(${project}
    PRIVATE cwalk
    PRIVATE vmlib
    PRIVATE m
    )

message("-- Supervisor image: ${WANTED_SUPERVISOR_IMAGE_PATH}")
target_compile_definitions(${project} PRIVATE
    SUPERVISOR_IMAGE_PATH="${WANTED_SUPERVISOR_IMAGE_PATH}")

# Add debug symbols
target_compile_options(${project} PUBLIC -g)

option(WANTED_DEBUG_TRACES "Enable debug prints for Wanted" OFF)
if (WANTED_DEBUG_TRACES)
    message("-- Debug tracing for Wanted enabled")
    target_compile_definitions(${project} PRIVATE DEBUG=1)
endif()


if (SECURE_SOCKETS)
    message("-- Building with support for secure sockets")
    target_compile_definitions(${project} PUBLIC SECURE_SOCKETS=1)
endif()
