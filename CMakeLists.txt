cmake_minimum_required(VERSION 3.9)
project(
    wanted
    VERSION 0.0.1
    LANGUAGES C
    )

set(target ${CMAKE_PROJECT_NAME})

add_subdirectory(${CMAKE_SOURCE_DIR}/src)

add_executable(${target}_cmd cmd/main.c)

target_link_libraries(${target}_cmd
    PUBLIC pthread
    PUBLIC ${target}
    )

add_custom_target(wasm
    COMMAND make -C ${CMAKE_SOURCE_DIR}/wasm clean
    COMMAND make -C ${CMAKE_SOURCE_DIR}/wasm
)
#add_dependencies(${CMAKE_PROJECT_NAME} compile_wasm)
