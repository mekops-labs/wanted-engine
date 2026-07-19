# Cross-compile toolchain for OpenWRT (musl) targets, driven by an OpenWRT SDK.
#
# Env: OPENWRT_TOOLCHAIN (SDK toolchain-* dir), OPENWRT_CROSS (tool prefix),
#      OPENWRT_SYSROOT (SDK target-* dir), OPENWRT_ARCH (aarch64|mips).
# See packaging/openwrt/README.md.

set(CMAKE_SYSTEM_NAME Linux)

set(_owrt_toolchain "$ENV{OPENWRT_TOOLCHAIN}")
set(_owrt_cross "$ENV{OPENWRT_CROSS}")
set(_owrt_sysroot "$ENV{OPENWRT_SYSROOT}")
set(_owrt_arch "$ENV{OPENWRT_ARCH}")

if(NOT _owrt_toolchain OR NOT _owrt_cross OR NOT _owrt_sysroot OR NOT _owrt_arch)
    message(FATAL_ERROR
        "OpenWRT toolchain file needs OPENWRT_TOOLCHAIN, OPENWRT_CROSS, "
        "OPENWRT_SYSROOT and OPENWRT_ARCH exported. See the header of this file.")
endif()

set(CMAKE_SYSTEM_PROCESSOR "${_owrt_arch}")

set(CMAKE_C_COMPILER   "${_owrt_toolchain}/bin/${_owrt_cross}-gcc")
set(CMAKE_CXX_COMPILER "${_owrt_toolchain}/bin/${_owrt_cross}-g++")

set(CMAKE_SYSROOT "${_owrt_sysroot}")

# Libraries/headers from the sysroot; executables from the host (cmake probes).
set(CMAKE_FIND_ROOT_PATH "${_owrt_sysroot}" "${_owrt_toolchain}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# WAMR defaults 32-bit non-ARM to X86_32, and its mips asm trampoline is
# hard-float O32 — OpenWRT mips is soft-float, so use the portable C one.
if(_owrt_arch STREQUAL "mips")
    set(WAMR_BUILD_TARGET "MIPS" CACHE STRING "WAMR build target" FORCE)
    set(WAMR_BUILD_INVOKE_NATIVE_GENERAL 1 CACHE STRING
        "WAMR portable C trampoline" FORCE)
endif()
