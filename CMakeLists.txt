cmake_minimum_required(VERSION 3.9)

# Emit compile_commands.json in the build dir so clang-tidy (and editors) can
# resolve include paths and flags per translation unit.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

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

# Instrument every target for gcov when coverage is requested. Must be set
# before any target is defined so the wanted library itself is covered, not
# just the test binary. The `coverage` report target is wired in test/.
option(ENABLE_CODE_COVERAGE "Enable build with code coverage analysis." OFF)
if(ENABLE_CODE_COVERAGE)
    message("-- Code coverage build enabled")
    add_compile_options(--coverage -O0 -g)
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} --coverage")
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} --coverage")
endif()

# Engine-only warning guard. The NuttX kbuild already compiles with these.
set(WANTED_STRICT_WARNINGS -Werror=strict-prototypes -Werror=undef)

# Engine resource-limit overrides (see src/include/wanted-config.h). Each limit
# defaults to the conservative "constrained" value baked into that header;
# defining the matching CMake variable — on the command line (-DMAX_WAPPS=16) or
# via a profile in cmake/profiles/ — forwards it as a compile definition that
# wins over the header's #ifndef default. Applied with add_compile_definitions
# so every target (engine, platform, CLI, tests) compiles with the same value; a
# per-target mismatch would desync statically sized structures, since MAX_WAPPS
# dimensions arrays in both the engine and platform libraries. Driver-private
# limits are not here — they live in their driver and are not profile-tunable.
foreach(_limit MAX_WAPPS WASM_STACK_SIZE WASM_HEAP_SIZE WASM_MAX_MEMORY_PAGES
               MAX_PATH_LEN)
    if(DEFINED ${_limit})
        add_compile_definitions(${_limit}=${${_limit}})
        message("-- Resource limit override: ${_limit}=${${_limit}}")
    endif()
endforeach()

# Build Wanted static library
add_subdirectory(${CMAKE_SOURCE_DIR}/src)

add_custom_target(wasm
    COMMAND make -C ${CMAKE_SOURCE_DIR}/wasm clean
    COMMAND make -C ${CMAKE_SOURCE_DIR}/wasm NAME=test_prog
    COMMAND make -C ${CMAKE_SOURCE_DIR}/wasm NAME=test_wasi WASI=1
    )
#add_dependencies(${CMAKE_PROJECT_NAME} compile_wasm)

# Rebuilds wasm/supervisor/<variant>/supervisor.tar (a TAR layer) from app.wasm.
# Not wired as a build dependency: the archive is checked in and only refreshed
# when the supervisor sources change.
add_custom_target(supervisor
    COMMAND make -C ${CMAKE_SOURCE_DIR}/wasm/supervisor
    )

# The standalone CLI (cmd/main.c) is a host/Linux artifact. On NuttX the entry
# point is platform/nuttx/wanted_main.c, built as a NuttX built-in app, so the
# CLI is off by default there.
if (WANTED_PLATFORM STREQUAL "nuttx")
    option (BUILD_EXECUTABLE "Build standalone executable. Linux only" OFF)
else()
    option (BUILD_EXECUTABLE "Build standalone executable. Linux only" ON)
endif()

if (BUILD_EXECUTABLE)
    # cmd/ also pulls in the selected platform library and links the CLI.
    add_subdirectory(${CMAKE_SOURCE_DIR}/cmd)
elseif (WANTED_PLATFORM STREQUAL "nuttx")
    # Without the CLI, still build the platform layer so the NuttX scaffolding
    # link is exercised; the NuttX app build compiles wanted_main.c separately.
    add_subdirectory(${CMAKE_SOURCE_DIR}/platform/nuttx
                     ${CMAKE_BINARY_DIR}/platform/nuttx)
endif()

if (BUILD_TESTING)
    add_subdirectory(test)
endif()
