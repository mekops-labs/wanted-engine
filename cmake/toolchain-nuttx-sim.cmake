# Toolchain for the NuttX simulation target.
#
# The sim runs on the host, so no cross-compiler is set here — the host gcc/clang
# is used. This file only selects the NuttX platform sources and routes WAMR to
# its NuttX platform layer.
#
# Usage:
#   cmake -B build-nuttx-sim -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-nuttx-sim.cmake
#
# Note: WAMR's NuttX platform pulls in <nuttx/config.h>/<nuttx/cache.h>, which
# exist only inside a configured NuttX tree. A bare host build with this
# toolchain compiles the wanted scaffolding but not WAMR; the full sim build
# runs through the NuttX app integration.

set(WANTED_PLATFORM "nuttx" CACHE STRING "Target platform" FORCE)
set(WAMR_BUILD_PLATFORM "nuttx" CACHE STRING "WAMR platform" FORCE)
