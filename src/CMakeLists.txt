set(sources
    "main.c"
    "my_api.c"
    "my_wasi.c"
)

# Add tiny-json to the build
set(tiny-json-src
    ${CMAKE_SOURCE_DIR}/vendor/tiny-json/tiny-json.c
)

add_subdirectory(${CMAKE_SOURCE_DIR}/vendor/wasm3/source ${CMAKE_BINARY_DIR}/m3)

add_executable(${target}
    ${sources}
    ${tiny-json-src}
)

target_include_directories(${target} PRIVATE ${CMAKE_SOURCE_DIR}/vendor/tiny-json)

target_link_libraries(${target} PRIVATE m3)
target_link_libraries(${target} PRIVATE pthread)

# Add debug symbols
target_compile_options(${target} PUBLIC -g)
target_compile_options(m3 PUBLIC -g)

option(ENABLE_WASM3_TRACE "Enable wasm3 tracing" OFF)

if (ENABLE_WASM3_TRACE)
    message("Trace for WASM3 enabled")
    target_compile_definitions(m3 PUBLIC d_m3EnableStrace=1 DEBUG=1)
endif()

target_link_libraries(${target} PRIVATE m)


