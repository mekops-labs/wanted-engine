set(sources
    "wanted.c"
    "my_api.c"
)

set(vfs-srcs
    "vfs/vfs.c"
    "vfs/vfs-romfs.c"
    "vfs/vfs-linux.c"
    "vfs/vfs-dummy.c"
    )

set(wasiTypes Linux Romfs Vfs)
set(wasiType Vfs CACHE STRING "WASI type")
set_property(CACHE wasiType PROPERTY STRINGS ${wasiTypes})

if(NOT wasiType IN_LIST wasiTypes)
    message(FATAL_ERROR "wasiType must be one of ${wasiTypes}")
endif()

message("Wasi type is ${wasiType}")

if (wasiType STREQUAL "Linux")
    set(wasi-src
        "wasi/wasi-linux.c")
elseif (wasiType STREQUAL "Romfs")
    set(wasi-src
        "wasi/wasi-romfs.c"
    )
elseif (wasiType STREQUAL "Vfs")
    set(wasi-src
        "wasi/wasi-vfs.c"
        ${vfs-srcs}
    )
endif()

# Add tiny-json to the build
set(tiny-json-src
    ${CMAKE_SOURCE_DIR}/vendor/tiny-json/tiny-json.c
)

add_subdirectory(${CMAKE_SOURCE_DIR}/vendor/wasm3/source ${CMAKE_BINARY_DIR}/m3)

if (wasiType STREQUAL "Romfs" OR wasiType STREQUAL "Vfs")
    add_subdirectory(${CMAKE_SOURCE_DIR}/vendor/romfs-lib/src ${CMAKE_BINARY_DIR}/romfs)
endif()

add_library(${target} STATIC
    ${sources}
    ${wasi-src}
    ${tiny-json-src}
)

target_include_directories(${target}
    PUBLIC ${CMAKE_SOURCE_DIR}/include
    PRIVATE ${CMAKE_SOURCE_DIR}/src/include
    PRIVATE ${CMAKE_SOURCE_DIR}/vendor/tiny-json
    )

if (wasiType STREQUAL "Romfs" OR wasiType STREQUAL "Vfs")
    add_subdirectory(${CMAKE_SOURCE_DIR}/vendor/cwalk cwalk)
    target_include_directories(${target} PRIVATE ${CMAKE_SOURCE_DIR}/vendor/romfs-lib/include)
    target_link_libraries(${target} PRIVATE romfs PRIVATE cwalk)
    target_compile_definitions(${target} PRIVATE WANTED_ROMFS=1)
endif()

target_link_libraries(${target} PRIVATE m3)
target_link_libraries(${target} PRIVATE pthread)

# Add debug symbols
target_compile_options(${target} PUBLIC -g)
target_compile_options(m3 PUBLIC -Wno-extern-initializer -g)

option(EnableWasm3Trace "Enable wasm3 tracing" OFF)

if (EnableWasm3Trace)
    message("Trace for WASM3 enabled")
    target_compile_definitions(m3 PRIVATE d_m3EnableStrace=1 DEBUG=1)
endif()

option(WANTED_DEBUG_TRACES "Enable debug prints for Wanted" OFF)

if (WANTED_DEBUG_TRACES)
    message("-- Debug tracing for Wanted enabled")
    target_compile_definitions(${target} PRIVATE DEBUG=1)
endif()

target_link_libraries(${target} PRIVATE m)


