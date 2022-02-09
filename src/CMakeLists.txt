set(sources
    "wanted.c"
    "my_api.c"
)

set(vfs-srcs
    "vfs/vfs.c"
    "vfs/vfs-romfs.c"
    "vfs/vfs-dummy.c"
    )

set(wasi-src
    "wasi/wasi-vfs.c"
)

# Add tiny-json to the build
set(tiny-json-src
    ${CMAKE_SOURCE_DIR}/vendor/tiny-json/tiny-json.c
)

add_subdirectory(${CMAKE_SOURCE_DIR}/platform ${CMAKE_BINARY_DIR}/platform)
add_subdirectory(${CMAKE_SOURCE_DIR}/vendor/wasm3/source ${CMAKE_BINARY_DIR}/m3)
add_subdirectory(${CMAKE_SOURCE_DIR}/vendor/romfs-lib/src ${CMAKE_BINARY_DIR}/romfs)
add_subdirectory(${CMAKE_SOURCE_DIR}/vendor/cwalk ${CMAKE_BINARY_DIR}/cwalk)

add_library(${project} STATIC
    ${sources}
    ${vfs-srcs}
    ${wasi-src}
    ${tiny-json-src}
)

target_include_directories(${project}
    PUBLIC ${CMAKE_SOURCE_DIR}/include
    PRIVATE ${CMAKE_SOURCE_DIR}/src/include
    PRIVATE ${CMAKE_SOURCE_DIR}/vendor/tiny-json
    )

target_link_libraries(${project} PRIVATE romfs PRIVATE cwalk PRIVATE platform)
target_link_libraries(${project} PRIVATE m3)
target_link_libraries(${project} PRIVATE pthread)
target_link_libraries(${project} PRIVATE m)

# Add debug symbols
target_compile_options(${project} PUBLIC -g)

# Set flags for m3, disable harmless wanings
target_compile_options(m3 PRIVATE -Wno-unused-function -Wno-unused-variable -Wno-unused-parameter -Wno-missing-field-initializers -Wno-extern-initializer)

option(M3_DEBUG_TRACES "Enable wasm3 tracing" OFF)

if (M3_DEBUG_TRACES)
    message("Trace for WASM3 enabled")
    target_compile_definitions(m3 PRIVATE d_m3EnableStrace=1 DEBUG=1)
endif()

option(WANTED_DEBUG_TRACES "Enable debug prints for Wanted" OFF)

if (WANTED_DEBUG_TRACES)
    message("-- Debug tracing for Wanted enabled")
    target_compile_definitions(${project} PRIVATE DEBUG=1)
endif()
