set(sources
    "wanted_malloc.c"
    "wanted.c"
    "wanted_wasm_api.c"
    "wanted-vfs-api.c"
)

set(vfs-srcs
    "vfs/vfs.c"
    "vfs/vfs-null.c"
    "vfs/vfs-virtual.c"
    "vfs/vfs-romfs.c"
    "vfs/vfs-wanted.c"
    "vfs/vfs-wanted-config.c"
    "vfs/vfs-wanted-ctrl.c"
    "vfs/vfs-wanted-registry.c"
    "vfs/vfs-socket.c"
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

add_subdirectory(${CMAKE_SOURCE_DIR}/platform ${CMAKE_BINARY_DIR}/platform)

set(CUSTOM_MALLOC_IMPL ${CMAKE_CURRENT_SOURCE_DIR}/romfs_malloc.c)
add_subdirectory(${CMAKE_SOURCE_DIR}/vendor/romfs-lib/src ${CMAKE_BINARY_DIR}/romfs)
target_compile_definitions(romfs PRIVATE ROMFS_CUSTOM_MALLOC=1)

add_subdirectory(${CMAKE_SOURCE_DIR}/vendor/cwalk ${CMAKE_BINARY_DIR}/cwalk)

# WAMS3 related things
add_subdirectory(${CMAKE_SOURCE_DIR}/vendor/wasm3/source ${CMAKE_BINARY_DIR}/m3)

option(M3_DEBUG_TRACES "Enable wasm3 tracing" OFF)
if (M3_DEBUG_TRACES)
    message("Trace for WASM3 enabled")
    target_compile_definitions(m3 PRIVATE d_m3EnableStrace=1 DEBUG=1 d_m3LogHeapOps=0)
endif()

# Set flags for m3, disable harmless wanings
target_compile_options(m3 PRIVATE -Wno-unused-function -Wno-unused-variable -Wno-unused-parameter -Wno-missing-field-initializers -Wno-extern-initializer)

add_library(${project} STATIC
    ${sources}
    ${vfs-srcs}
    ${wasi-src}
    ${tiny-json-src}
    ${json-maker-src}
)

target_include_directories(${project}
    PUBLIC ${CMAKE_SOURCE_DIR}/include
    PRIVATE ${CMAKE_SOURCE_DIR}/src/include
    PRIVATE ${CMAKE_SOURCE_DIR}/wasm
    PRIVATE ${CMAKE_SOURCE_DIR}/vendor/tiny-json
    PRIVATE ${CMAKE_SOURCE_DIR}/vendor/json-maker/src/include
    )

target_link_libraries(${project}
    PRIVATE romfs
    PRIVATE cwalk
    PRIVATE platform
    PRIVATE m3
    PRIVATE m
    )

# Add debug symbols
target_compile_options(${project} PUBLIC -g)

option(WANTED_DEBUG_TRACES "Enable debug prints for Wanted" OFF)
if (WANTED_DEBUG_TRACES)
    message("-- Debug tracing for Wanted enabled")
    target_compile_definitions(${project} PRIVATE DEBUG=1)
endif()
