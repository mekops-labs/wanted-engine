set(sources
    "main.c"
    "my_api.c"
)

set(wasiTypes Linux Romfs)
set(wasiType Linux CACHE STRING "WASI type")
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
endif()

# Add tiny-json to the build
set(tiny-json-src
    ${CMAKE_SOURCE_DIR}/vendor/tiny-json/tiny-json.c
)

add_subdirectory(${CMAKE_SOURCE_DIR}/vendor/wasm3/source ${CMAKE_BINARY_DIR}/m3)

if (wasiType STREQUAL "Romfs")
    add_subdirectory(${CMAKE_SOURCE_DIR}/vendor/romfs-lib/src ${CMAKE_BINARY_DIR}/romfs)
endif()

add_executable(${target}
    ${sources}
    ${wasi-src}
    ${tiny-json-src}
)

target_include_directories(${target} PRIVATE ${CMAKE_SOURCE_DIR}/vendor/tiny-json)

if (wasiType STREQUAL "Romfs")
    target_include_directories(${target} PRIVATE ${CMAKE_SOURCE_DIR}/vendor/romfs-lib/include)
    target_link_libraries(${target} PRIVATE romfs)
    target_compile_definitions(${target} PUBLIC WANTED_ROMFS=1)
endif()

target_link_libraries(${target} PRIVATE m3)
target_link_libraries(${target} PRIVATE pthread)

# Add debug symbols
target_compile_options(${target} PUBLIC -g)
target_compile_options(m3 PUBLIC -Wno-extern-initializer -g)

option(EnableWasm3Trace "Enable wasm3 tracing" OFF)

if (EnableWasm3Trace)
    message("Trace for WASM3 enabled")
    target_compile_definitions(m3 PUBLIC d_m3EnableStrace=1 DEBUG=1)
endif()

target_link_libraries(${target} PRIVATE m)


