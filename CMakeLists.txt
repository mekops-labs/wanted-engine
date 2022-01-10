cmake_minimum_required(VERSION 3.9)
project(wanted)

set(target ${CMAKE_PROJECT_NAME})

add_subdirectory(${CMAKE_SOURCE_DIR}/src)

target_include_directories(${target} PUBLIC include)

add_custom_target(wasm
    COMMAND make -C ${CMAKE_SOURCE_DIR}/wasm clean
    COMMAND make -C ${CMAKE_SOURCE_DIR}/wasm
)
#add_dependencies(${CMAKE_PROJECT_NAME} compile_wasm)
