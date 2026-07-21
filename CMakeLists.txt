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
#
# Configured (CONFIG_WANTED_BUILD_COVERAGE), so it must come after the Kconfig
# step below reads .config — hence the include, then this block.
include(cmake/Kconfig.cmake)

# The configured launch config, compiled in as the CLI's default. Same generated
# header the NuttX builds get (test/nuttx-sim.sh), in a directory both include.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${CMAKE_SOURCE_DIR}/${CONFIG_WANTED_DEFAULT_CONFIG})
execute_process(
    COMMAND ${CMAKE_SOURCE_DIR}/utils/default-config-header.sh
            ${CMAKE_SOURCE_DIR} ${CONFIG_WANTED_DEFAULT_CONFIG}
            ${CMAKE_SOURCE_DIR}/src/include/wanted-config.h
    RESULT_VARIABLE _cfg_rc OUTPUT_VARIABLE _cfg_out ERROR_VARIABLE _cfg_err)
if(NOT _cfg_rc EQUAL 0)
    message(FATAL_ERROR "default configuration: ${_cfg_out}${_cfg_err}")
endif()
message("-- Default configuration: ${CONFIG_WANTED_DEFAULT_CONFIG}")

if(CONFIG_WANTED_BUILD_COVERAGE)
    message("-- Code coverage build enabled")
    add_compile_options(--coverage -O0 -g)
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} --coverage")
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} --coverage")
endif()

# Engine-only warning guard. The NuttX kbuild already compiles with these.
set(WANTED_STRICT_WARNINGS -Werror=strict-prototypes -Werror=undef)

# The generated header is build-wide configuration, not one target's private
# header: engine core, platform layers, and tests all compile against it.
include_directories(${WANTED_AUTOCONF_DIR})

# Secure sockets, defined build-wide rather than per target: the configuration
# symbol is global, so a per-target define made translation units disagree about
# whether the build has TLS — and the host guard rightly failed them.
if(CONFIG_WANTED_VFS_SOCKET_TLS)
    add_compile_definitions(SECURE_SOCKETS=1)
else()
    add_compile_definitions(SECURE_SOCKETS=0)
endif()

# Build Wanted static library
add_subdirectory(${CMAKE_SOURCE_DIR}/src)

add_custom_target(wasm
    COMMAND make -C ${CMAKE_SOURCE_DIR}/wasm clean
    COMMAND make -C ${CMAKE_SOURCE_DIR}/wasm NAME=test_prog
    COMMAND make -C ${CMAKE_SOURCE_DIR}/wasm NAME=test_wasi WASI=1
    )
#add_dependencies(${CMAKE_PROJECT_NAME} compile_wasm)

# Rebuilds wasm/supervisor/<variant>/supervisor.tar (a TAR layer) from app.wasm.
# Not wired as a build dependency: the wasm toolchain lives in the wapp-sdk
# image, not this build environment, and the engine only opens the archive by
# path at boot. Build it with `make wasm` (or CI's build-wasm job) instead.
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
