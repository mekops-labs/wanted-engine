#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Runs inside the engine build container. MODE=classic|fast.
set -euo pipefail
cd /src
export ENGINE_DIR=/src BUILD_DIR=build-nuttx-poc DEFCONFIG=nuttx_poc
[ "$MODE" = fast ] && DEFCONFIG=nuttx_poc_fast
N=third_party/nuttx
# keep the stock simulator configuration, then optimise and drop coverage
[ -f $N/.config.orig-wanted-sim ] || cp $N/.config $N/.config.orig-wanted-sim
sed -i \
 -e 's/^CONFIG_DEBUG_NOOPT=y/# CONFIG_DEBUG_NOOPT is not set/' \
 -e 's/^# CONFIG_DEBUG_FULLOPT is not set/CONFIG_DEBUG_FULLOPT=y/' \
 -e 's/^CONFIG_COVERAGE_ALL=y/# CONFIG_COVERAGE_ALL is not set/' \
 -e 's/^CONFIG_COVERAGE_TOOLCHAIN=y/# CONFIG_COVERAGE_TOOLCHAIN is not set/' \
 -e 's/^# CONFIG_COVERAGE_NONE is not set/CONFIG_COVERAGE_NONE=y/' $N/.config
(cd $N && make olddefconfig >/dev/null 2>&1 || true)
grep -E "^CONFIG_DEBUG_(NOOPT|FULLOPT)|^CONFIG_COVERAGE_(NONE|ALL|TOOLCHAIN)" $N/.config
# the configuration change affects every object, so clean the whole tree
(cd $N && make clean >/dev/null 2>&1 || true)
# a mode switch changes the WAMR flags, so rebuild every engine object
rm -f third_party/nuttx-apps/system/wanted/*.o third_party/nuttx-apps/system/wanted/*.gc??
rm -f $N/nuttx
./test/nuttx-sim.sh build
ls -l $N/nuttx
cp $N/nuttx /scratch/nuttx-"$MODE"
