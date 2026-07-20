# WANTED Engine — canonical command runner.
#
# Every build/test/lint recipe lives here and assumes it runs *inside* the
# standardized build container (toolchain on PATH, repo at the working dir).
# That is how CI and the devcontainer invoke them. On a bare host, the root
# `Makefile` is a thin wrapper that runs these same recipes in the container
# (`make build` == `just build` in the image) — see Makefile.
#
# Two images: this build image runs everything below except the wasm recipes,
# which need the wapp-sdk image and are absent here — see "--- wasm" below.
#
# Overrides are read from the environment so one recipe serves local + CI:
#   BUILD_DIR (default build) · PROFILE (tiny|constrained|small|big)
#   CC · CMAKE_EXTRA_ARGS · NUTTX_SKIP_BUILD · NUTTX_CLEAN

build_dir := env_var_or_default("BUILD_DIR", "build")
profile   := env_var_or_default("PROFILE", "")
cmake_extra := env_var_or_default("CMAKE_EXTRA_ARGS", "")
wsh_tar   := "./wasm/supervisor/wsh/supervisor.tar"

# OpenWRT SDKs for the qemu selftest lanes, matching the reference routers.
sdk_aarch64 := "https://downloads.openwrt.org/releases/24.10.0/targets/mediatek/filogic/openwrt-sdk-24.10.0-mediatek-filogic_gcc-13.3.0_musl.Linux-x86_64.tar.zst"
sdk_mipsel  := "https://downloads.openwrt.org/releases/23.05.5/targets/ramips/mt7621/openwrt-sdk-23.05.5-ramips-mt7621_gcc-12.3.0_musl.Linux-x86_64.tar.xz"

# Optional resource-limit profile (cmake/profiles/<name>.cmake). Absolute: the
# build recipes cd into {{build_dir}} before invoking cmake.
profile_arg := if profile != "" { "-C " + justfile_directory() + "/cmake/profiles/" + profile + ".cmake" } else { "" }

# First-party C/H sources (vendored deps and generated trees are excluded).
src_dirs := "src platform cmd include"
# clang-tidy reads flags per TU from this build's compile_commands.json.
tidy_build_dir := env_var_or_default("TIDY_BUILD_DIR", "build-clang")

# List available recipes.
default:
    @just --list

# Build the engine + CLI and run the unit suite.
all: build test

# --- wasm -----------------------------------------------------------------
# Deliberately no wasm recipes here — the wapp-sdk image has no `just`, so those
# builds run through plain Makefiles (wasm/supervisor/Makefile, wapps/Makefile)
# instead. From a host: `make wasm` / `supervisor` / `wapps` / `sheriff`.

# --- build ----------------------------------------------------------------
# No dependency on the wasm recipes: the supervisor TAR is loaded by path at
# runtime, so it only has to exist (built separately via `make wasm`) by boot.

# Build the engine + CLI with the production (sheriff) supervisor [PROFILE=...].
build:
    mkdir -p {{build_dir}}
    cd {{build_dir}} && cmake -GNinja {{profile_arg}} {{cmake_extra}} .. && ninja

# Build the engine + CLI with the wsh debug supervisor compiled in [PROFILE=...].
wsh:
    mkdir -p {{build_dir}}
    cd {{build_dir}} && cmake -GNinja {{profile_arg}} {{cmake_extra}} -DWANTED_SUPERVISOR_IMAGE_PATH={{wsh_tar}} .. && ninja

# Build a production OpenWRT .ipk -> dist/. sdk = SDK URL or local SDK dir.
openwrt-package sdk:
    packaging/openwrt/openwrt-package.sh "{{sdk}}"

# --- test (run against an already-built {{build_dir}}) ---------------------

# Run the unit + smoke suite via ctest (JUnit report emitted for CI).
test:
    cd {{build_dir}} && ctest -j"$(nproc)" --output-on-failure --output-junit rspec.xml

# Cobertura coverage report (build with CMAKE_EXTRA_ARGS=-DENABLE_CODE_COVERAGE=on).
coverage:
    cd {{build_dir}} && ninja coverage

# Boot the production supervisor and assert a clean instantiate.
smoke-engine:
    ./test/smoke-engine.sh ./{{build_dir}}/cmd/wanted-cli

# Run the in-WASM selftest suite on Linux.
selftest:
    ./test/selftest.sh ./{{build_dir}}/cmd/wanted-cli

# Run the in-WASM selftest suite against a cross-built engine under qemu.
# sdk = OpenWRT SDK URL or local SDK dir; the aarch64/mipsel shorthands below
# pin the SDKs the reference routers run.
selftest-qemu sdk:
    ./test/selftest-qemu.sh "{{sdk}}"

selftest-qemu-aarch64:
    ./test/selftest-qemu.sh "{{sdk_aarch64}}"

selftest-qemu-mipsel:
    ./test/selftest-qemu.sh "{{sdk_mipsel}}"

# Run the system-control (poweroff/reboot/exit) checks on Linux.
syscontrol:
    ./test/syscontrol.sh ./{{build_dir}}/cmd/wanted-cli

# Swap the supervisor image under a running engine: child wapps keep running,
# an armed reload adopts the staged image, a bad one rolls back.
live-update:
    BUILD_DIR=build-wsh just wsh
    ./test/live-update.sh ./build-wsh/cmd/wanted-cli

# Negative test: WASM_MAX_MEMORY_PAGES bounds a wapp's linear-memory growth.
memcap:
    ./test/memcap.sh

# Package the ESP-IDF factory-seed images from built wapps. The firmware embeds
# these into the app binary, so they must exist before `idf.py build`; they are
# gitignored artifacts, like every other packaged .wapp. Needs `just wapps`
# (wapp SDK image) to have produced the .wasm files first.
registry-seed:
    #!/usr/bin/env bash
    set -euo pipefail
    mkdir -p wasm/registry-seed
    for n in looper wifi-connect devcheck; do
        src="wapps/$n/$n.wasm"
        [ -f "$src" ] || { echo "registry-seed: $src missing — run 'make wapps' first" >&2; exit 1; }
        s=$(mktemp -d); cp "$src" "$s/app.wasm"
        tar --format=ustar --owner=0 --group=0 --mtime='1970-01-01 00:00:00 UTC' \
            -C "$s" -cf "wasm/registry-seed/$n.wapp" app.wasm
        rm -rf "$s"
        echo "registry-seed: wasm/registry-seed/$n.wapp"
    done

# Report per-wapp + engine memory footprint per profile (linux + nuttx ABIs).
sizes:
    ./utils/measure-sizes.sh

# --- NuttX simulator ------------------------------------------------------
# The build/test recipe lives in test/nuttx-sim.sh (shared with CI); these
# recipes just dispatch to it. PROFILE / NUTTX_SKIP_BUILD / NUTTX_CLEAN are read
# from the environment by the script.

# Link the engine app package into the checked-out nuttx-apps submodule.
nuttx-deps:
    ./test/nuttx-sim.sh deps

# Configure + build the NuttX sim (wsh as init over hostfs) [PROFILE=...].
nuttx-build:
    ./test/nuttx-sim.sh deps build

# Build just the sim kernel binary (no hostfs staging) — the split-CI artifact.
nuttx-kernel:
    ./test/nuttx-sim.sh kernel

# Run the in-WASM selftest suite on the NuttX sim.
nuttx-selftest:
    ./test/nuttx-sim.sh selftest

# Run the system-control (poweroff/reboot/exit) checks on the NuttX sim.
nuttx-syscontrol:
    ./test/nuttx-sim.sh syscontrol

# Distclean the NuttX submodule tree.
nuttx-clean:
    ./test/nuttx-sim.sh clean

# --- ESP32 firmware (real hardware) ---------------------------------------
# Cross-build the NuttX firmware for the Waveshare ESP32 One. Run *inside the
# xtensa toolchain image*; the Makefile `esp32` target stages the supervisor +
# nuttx-deps in the build image first, then runs this in the xtensa image.
# Output is third_party/nuttx/nuttx.bin, flashed at 0x1000 (ESP32 Simple Boot).
esp32-build:
    cd third_party/nuttx && \
        { [ -f .config ] || ./tools/configure.sh -a ../nuttx-apps esp32-devkitc:wanted; } && \
        make -j"$(nproc)"

# --- clean ----------------------------------------------------------------

# Remove every build artifact (Linux + NuttX sim + wasm/wapps + submodule objects).
clean:
    rm -rf {{build_dir}} build-nuttx registry
    # Every supervisor app.wasm is a gitignored build output, sheriff's included.
    rm -f wasm/*.wasm* wasm/supervisor/*/supervisor.tar wasm/supervisor/*/app.wasm
    rm -f wapps/*/*.wasm wapps/*/*.wasm.h wapps/*/*.o
    # NuttX kernel objects + .config live in the submodule tree; an incremental
    # rebuild over a stale tree silently runs old code, so distclean it too.
    ./test/nuttx-sim.sh clean

# --- lint / static analysis / security ------------------------------------

# All blocking lint checks.
lint: lint-format lint-shell

# Reject any formatting drift (clang-format config in .clang-format). Prunes
# ESP-IDF's own build/vendored dirs under platform/esp-idf/project/ (build/,
# managed_components/, .cache/ — gitignored, not our source; idf.py downloads
# and regenerates them, so linting them is both wrong and non-reproducible).
lint-format:
    find {{src_dirs}} \( -name build -o -name 'build.*' -o -name managed_components -o -name .cache \) -prune -o \( -name '*.c' -o -name '*.h' \) -print0 \
        | xargs -0 clang-format --dry-run --Werror

# Reformat the tree in place (developer helper; not run in CI).
format-fix:
    find {{src_dirs}} \( -name build -o -name 'build.*' -o -name managed_components -o -name .cache \) -prune -o \( -name '*.c' -o -name '*.h' \) -print0 \
        | xargs -0 clang-format -i

# Lint shell scripts. error severity only for now; ratchet down over time.
lint-shell:
    find . -name '*.sh' -not -path './vendor/*' -not -path './third_party/*' -not -path './build*/*' -not -path './.openwrt-sdk/*' -print0 \
        | xargs -0 shellcheck --severity=error

# Configure + build the clang build dir clang-tidy reads compile_commands.json
# from. Idempotent: a no-op when the dir is already built (CI build-clang
# artifact). Builds so generated headers (e.g. the supervisor config) exist.
tidy-build:
    mkdir -p {{tidy_build_dir}}
    cd {{tidy_build_dir}} && CC=clang cmake -GNinja .. && ninja

# clang-tidy the compiled first-party sources. The file list is taken from the
# compile DB so conditionally-built sources (e.g. ssocket.c without OpenSSL) are
# analysed only when actually compiled. --config-file pins the root config so
# clang-tidy never loads a vendored .clang-tidy from an included header's tree.
tidy: tidy-build
    clang-tidy -p {{tidy_build_dir}} --config-file=.clang-tidy --warnings-as-errors='*' \
        $(python3 -c "import json,os; print('\n'.join(sorted({os.path.relpath(e['file']) for e in json.load(open('{{tidy_build_dir}}/compile_commands.json')) if os.path.relpath(e['file']).startswith(('src/','platform/linux/','cmd/'))})))")

# cppcheck does its own parsing, so it covers every platform without a build.
# Excludes ESP-IDF's own downloaded/generated build/managed_components/.cache
# dirs (idf.py-owned, not our source — same reasoning as lint-format's prune).
cppcheck:
    #!/bin/sh
    set -e
    excludes=""
    for d in $(find platform/esp-idf -type d \( -name build -o -name managed_components -o -name .cache \) 2>/dev/null); do
        excludes="$excludes -i$d"
    done
    cppcheck --enable=warning,style,performance,portability \
        --suppress=missingIncludeSystem --suppress=normalCheckLevelMaxBranches \
        --inline-suppr --error-exitcode=1 \
        -I include -I src/include -I platform/include \
        -DCONFIG_RP23XX_FLASH_MTD_MOUNTPOINT='"/mnt/flash"' \
        $excludes \
        src platform cmd

# gcc -fanalyzer: deep but slow/verbose — run out-of-band, not on every push.
analyze build_dir="build-analyze":
    mkdir -p {{build_dir}}
    cd {{build_dir}} && cmake -GNinja -DCMAKE_C_FLAGS="-fanalyzer" .. && ninja

# Pattern-based security scan (C/C++ ruleset).
security:
    semgrep --config "p/c" --error --quiet {{src_dirs}}

# Scan the build image definition and the working tree for CVEs and secrets.
scan-image:
    trivy config --severity HIGH,CRITICAL docker/Dockerfile
    trivy fs --severity HIGH,CRITICAL --scanners vuln,secret .
