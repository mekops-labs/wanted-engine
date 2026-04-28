cmake_minimum_required(VERSION 3.9)

include(cmake/VersionFromGit.cmake)

version_from_git(
    TIMESTAMP "%Y%m%d%H%M%S"
    )

project(
    wanted
    VERSION ${VERSION}
    LANGUAGES C
    )

include(CTest)

set(project ${CMAKE_PROJECT_NAME})
message("Building ${project} v${SEMVER}")

# Build Wanted static library
add_subdirectory(${CMAKE_SOURCE_DIR}/src)

add_custom_target(wasm
    COMMAND make -C ${CMAKE_SOURCE_DIR}/wasm clean
    COMMAND make -C ${CMAKE_SOURCE_DIR}/wasm NAME=test_prog
    COMMAND make -C ${CMAKE_SOURCE_DIR}/wasm NAME=test_wasi WASI=1
    )
#add_dependencies(${CMAKE_PROJECT_NAME} compile_wasm)

# Rebuilds wasm/supervisor/<variant>/supervisor.h (a TAR layer) from app.wasm +
# manifest.json. Not wired as a build dependency: the header is checked in and
# only refreshed when the supervisor sources change.
add_custom_target(supervisor
    COMMAND make -C ${CMAKE_SOURCE_DIR}/wasm/supervisor
    )

option (BUILD_EXECUTABLE "Build standalone executable. Linux only" ON)
if (BUILD_EXECUTABLE)
    add_subdirectory(${CMAKE_SOURCE_DIR}/cmd)
endif()

if (BUILD_TESTING)
    add_subdirectory(test)
endif()
