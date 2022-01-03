cmake_minimum_required(VERSION 3.9)
project(wanted)

set(target ${CMAKE_PROJECT_NAME})

add_subdirectory(${CMAKE_SOURCE_DIR}/vendor/wasm3/source ${CMAKE_BINARY_DIR}/m3)

add_executable(${target}
    main.c my_api.c
    ${CMAKE_SOURCE_DIR}/vendor/tiny-json/tiny-json.c)

target_link_libraries(${target} PRIVATE m3)

target_link_libraries(${target} PRIVATE pthread)

# Add debug symbols
target_compile_options(${target} PUBLIC -g)
target_compile_options(m3 PUBLIC -g)
#target_compile_definitions(m3 PUBLIC d_m3EnableStrace=3 DEBUG=1)

target_link_libraries(${target} PRIVATE m)

target_include_directories(${target} PRIVATE ${CMAKE_SOURCE_DIR}/vendor/tiny-json)

# Copy the 'wasm' directory into the build directory, so that
# wasm/test_prog.wasm is found even if wasm3_cpp_example is executed
# from the build directory.
add_custom_target(wasm
    COMMAND make -C ${CMAKE_SOURCE_DIR}/wasm clean
    COMMAND make -C ${CMAKE_SOURCE_DIR}/wasm
)
#add_dependencies(${CMAKE_PROJECT_NAME} compile_wasm)
