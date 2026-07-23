# WANTED Engine — canonical command runner.
#
# Every build/test/lint recipe lives here and assumes it runs *inside* the
# standardized build container (toolchain on PATH, repo at the working dir).
# That is how CI and the devcontainer invoke them.
#
# Overrides are read from the environment so one recipe serves local + CI:
#   BUILD_DIR (default build) · DEFCONFIG (a name under configs/, sans suffix)
#   CC · CMAKE_EXTRA_ARGS · NUTTX_SKIP_BUILD · NUTTX_CLEAN

build_dir := env_var_or_default("BUILD_DIR", "build")
defconfig := env_var_or_default("DEFCONFIG", "")
cmake_extra := env_var_or_default("CMAKE_EXTRA_ARGS", "")

# Optional board defconfig (configs/<name>_defconfig), used only when this build
# dir has no .config yet — an existing configuration is never overwritten by a
# build.
defconfig_arg := if defconfig != "" { "-DWANTED_DEFCONFIG=" + defconfig + "_defconfig" } else { "" }

# First-party C/H sources (vendored deps and generated trees are excluded).
src_dirs := "src platform cmd include"
# clang-tidy reads flags per TU from this build's compile_commands.json.
tidy_build_dir := env_var_or_default("TIDY_BUILD_DIR", "build-clang")

# List available recipes.
default:
    @just --list

# Build the engine + CLI and run the unit suite.
all: build test

kconfig := "PYTHONPATH=" + justfile_directory() + "/tools/kconfiglib KCONFIG_CONFIG=" + build_dir + "/.config"
kcl := justfile_directory() + "/tools/kconfiglib"

# Ensure this build dir has a .config, then carry it forward over Kconfig edits
_config:
    #!/usr/bin/env bash
    set -euo pipefail
    mkdir -p {{build_dir}}
    if [ ! -f {{build_dir}}/.config ] && [ -n "{{defconfig}}" ]; then
        {{kconfig}} python3 {{kcl}}/defconfig.py --kconfig Kconfig \
            configs/{{defconfig}}_defconfig
    else
        {{kconfig}} python3 {{kcl}}/olddefconfig.py Kconfig
    fi

# Print one CONFIG_ value from this build dir's .config, or nothing if unset
_cfg sym:
    @sed -n 's/^{{sym}}=//p' {{build_dir}}/.config | tr -d '"'

# Each of these writes a .config, so each reports what it now costs.

# Edit this build dir's configuration in the terminal UI.
menuconfig:
    mkdir -p {{build_dir}}
    {{kconfig}} python3 {{kcl}}/menuconfig.py Kconfig
    @just sizes current

# Write the minimal defconfig for this build dir's .config to configs/<name>.
savedefconfig name:
    {{kconfig}} python3 {{kcl}}/savedefconfig.py --kconfig Kconfig \
        --out configs/{{name}}_defconfig

# Seed this build dir's .config from configs/<name>, replacing any existing one.
defconfig name:
    mkdir -p {{build_dir}}
    {{kconfig}} python3 {{kcl}}/defconfig.py --kconfig Kconfig \
        configs/{{name}}_defconfig
    @just sizes current

# Bring this build dir's .config forward over Kconfig edits
olddefconfig:
    mkdir -p {{build_dir}}
    {{kconfig}} python3 {{kcl}}/olddefconfig.py Kconfig
    @just sizes current

# Select the supervisor image for this build dir (sheriff | wsh | selftest)
supervisor-variant name: _config
    {{kconfig}} python3 {{kcl}}/setconfig.py --kconfig Kconfig \
        WANTED_SUPERVISOR_$(echo {{name}} | tr a-z A-Z)=y

# Select this build dir's target (linux | nuttx | esp-idf | openwrt) without opening menuconfig
target name: _config
    {{kconfig}} python3 {{kcl}}/setconfig.py --kconfig Kconfig \
        WANTED_TARGET_$(echo {{name}} | tr 'a-z-' 'A-Z_')=y

# Set one symbol in this build dir's .config, e.g.: setconfig 'WANTED_TARGET_NUTTX_BOARD="esp32-devkitc:wanted"'
setconfig assignment: _config
    {{kconfig}} python3 {{kcl}}/setconfig.py --kconfig Kconfig {{assignment}}

# Build the configured target [DEFCONFIG=... seeds a fresh .config].
build:
    #!/usr/bin/env bash
    set -euo pipefail
    just _config
    target=$(just _cfg CONFIG_WANTED_TARGET)
    cfg=$(just _cfg CONFIG_WANTED_DEFAULT_CONFIG)
    echo "==> building target: ${target:-linux}  (build dir: {{build_dir}})"
    echo "==> default configuration: ${cfg}"
    # Validated before anything compiles: a JSON that does not parse is a node
    # that will not boot.
    ./utils/default-config.sh "{{justfile_directory()}}" "$cfg" >/dev/null
    dist="{{justfile_directory()}}/dist/${target:-linux}"
    case "${target:-linux}" in
    linux)
        cd {{build_dir}} && cmake -GNinja {{defconfig_arg}} {{cmake_extra}} .. && ninja
        cd {{justfile_directory()}}
        mkdir -p "$dist"
        install -m 0755 {{build_dir}}/cmd/wanted-cli "$dist/wanted-cli"
        ./utils/default-config.sh "{{justfile_directory()}}" "$cfg" "$dist/config.json" >/dev/null
        # Supervisor image at the relative path the config names, so dist/linux
        # runs without reaching back into the source tree. Absent is not fatal —
        # it is built separately, in the wapp SDK image.
        sup=$(just _cfg CONFIG_WANTED_SUPERVISOR_IMAGE)
        if [ -n "$sup" ] && [ -f "${sup#./}" ]; then
            install -D -m 0644 "${sup#./}" "$dist/${sup#./}"
            echo "==> dist: $dist/wanted-cli + config.json + ${sup#./}"
        else
            echo "==> dist: $dist/wanted-cli + config.json"
            echo "    (supervisor image ${sup:-unset} not built — \`make sheriff\` / \`make supervisor\`)"
        fi
        ;;
    nuttx)
        board=$(just _cfg CONFIG_WANTED_TARGET_NUTTX_BOARD)
        NUTTX_BOARD="$board" ./test/nuttx-sim.sh deps build
        # The sim builds an ELF, hardware boards a .bin/.uf2. Named per board,
        # or two boards overwrite each other.
        mkdir -p "$dist"
        for img in nuttx nuttx.bin nuttx.uf2 nuttx.hex; do
            [ -f "third_party/nuttx/$img" ] || continue
            install -m 0644 "third_party/nuttx/$img" "$dist/${board%%:*}-$img"
            echo "==> dist: $dist/${board%%:*}-$img"
        done
        ;;
    esp-idf)
        chip=$(just _cfg CONFIG_WANTED_TARGET_ESP_IDF_CHIP)
        cd platform/esp-idf/project
        # set-target regenerates sdkconfig and clears the build dir, so run it
        # only when the chip actually changed — unconditionally would make every
        # build a cold one and discard any local sdkconfig edits.
        if ! grep -qx "CONFIG_IDF_TARGET=\"$chip\"" sdkconfig 2>/dev/null; then
            idf.py set-target "$chip"
        fi
        # Passed in: embedded in a host tree the engine's .config is narrowed to
        # the engine half, which carries no build-host paths.
        idf.py -DWANTED_DEFAULT_CONFIG="$cfg" build
        # Flashable at offset 0; the app binary alone is not.
        mkdir -p "$dist"
        idf.py merge-bin -o "$dist/wanted-$chip-merged.bin" >/dev/null
        echo "==> dist: $dist/wanted-$chip-merged.bin"
        ;;
    openwrt)
        WANTED_CONFIG="{{build_dir}}/.config" \
            packaging/openwrt/openwrt-package.sh "$(just _cfg CONFIG_WANTED_TARGET_OPENWRT_SDK)"
        ;;
    *)
        echo "build: unknown target '$target'" >&2; exit 1
        ;;
    esac

# Run the unit + smoke suite via ctest (JUnit report emitted for CI).
test:
    cd {{build_dir}} && ctest -j"$(nproc)" --output-on-failure --output-junit rspec.xml

# Build and test with an out-of-tree driver tree linked in
test-extra-drivers:
    BUILD_DIR=build-extra-drivers just setconfig \
        'WANTED_EXTRA_DRIVERS_DIR="{{justfile_directory()}}/test/extra-drivers"'
    BUILD_DIR=build-extra-drivers just build
    cd build-extra-drivers && ctest -j"$(nproc)" --output-on-failure -R driver_tables

# Cobertura coverage report (build with WANTED_BUILD_COVERAGE=y set first).
coverage:
    cd {{build_dir}} && ninja coverage

# Boot the production supervisor and assert a clean instantiate.
smoke-engine:
    ./test/smoke-engine.sh ./{{build_dir}}/cmd/wanted-cli

# Run the in-WASM selftest suite on Linux.
selftest:
    ./test/selftest.sh ./{{build_dir}}/cmd/wanted-cli

# Run the in-WASM selftest suite against a cross-built engine under qemu. sdk = aarch64 | mipsel | SDK URL | local SDK dir; cached under .openwrt-sdk/
selftest-qemu sdk:
    ./test/selftest-qemu.sh "{{sdk}}"

# Run the system-control (poweroff/reboot/exit) checks on Linux.
syscontrol:
    ./test/syscontrol.sh ./{{build_dir}}/cmd/wanted-cli

# Swap the supervisor image under a running engine.
live-update:
    BUILD_DIR=build-wsh just supervisor-variant wsh
    BUILD_DIR=build-wsh just build
    ./test/live-update.sh ./build-wsh/cmd/wanted-cli

# Run the full Linux integration suite, emitting one JUnit report.
integration:
    ./test/run-integration.sh

# Negative test: WASM_MAX_MEMORY_PAGES bounds a wapp's linear-memory growth.
memcap:
    ./test/memcap.sh

# Memory footprint per defconfig, or `current` for this build dir alone.
sizes mode="all":
    ./utils/measure-sizes.sh {{mode}}

# --- NuttX simulator ------------------------------------------------------
# The build/test recipe lives in test/nuttx-sim.sh (shared with CI); these
# recipes just dispatch to it. DEFCONFIG / NUTTX_SKIP_BUILD / NUTTX_CLEAN are read
# from the environment by the script.

# Link the engine app package into the checked-out nuttx-apps submodule.
nuttx-deps:
    ./test/nuttx-sim.sh deps

# Build just the sim kernel binary (no hostfs staging) — the split-CI artifact.
nuttx-kernel:
    ./test/nuttx-sim.sh kernel

# Run the in-WASM selftest suite on the NuttX sim (JUnit report for CI).
nuttx-selftest:
    ./test/run-one-junit.sh build-nuttx/selftest-junit.xml nuttx-selftest selftest -- ./test/nuttx-sim.sh selftest

# Run the system-control (poweroff/reboot/exit) checks on the NuttX sim (JUnit report for CI).
nuttx-syscontrol:
    ./test/run-one-junit.sh build-nuttx/syscontrol-junit.xml nuttx-syscontrol syscontrol -- ./test/nuttx-sim.sh syscontrol

# Distclean the NuttX submodule tree.
nuttx-clean:
    ./test/nuttx-sim.sh clean

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

# Prunes ESP-IDF's own build/vendored dirs under platform/esp-idf/project/
# (build/, managed_components/, .cache/ — gitignored, not our source; idf.py
# downloads and regenerates them, so linting them is both wrong and
# non-reproducible).

# Reject any formatting drift (clang-format config in .clang-format).
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

# clang-tidy the compiled first-party sources
tidy:
    mkdir -p {{tidy_build_dir}}
    cd {{tidy_build_dir}} && CC=clang cmake -GNinja .. && ninja
    clang-tidy -p {{tidy_build_dir}} --config-file=.clang-tidy --warnings-as-errors='*' \
        $(python3 -c "import json,os; print('\n'.join(sorted({os.path.relpath(e['file']) for e in json.load(open('{{tidy_build_dir}}/compile_commands.json')) if os.path.relpath(e['file']).startswith(('src/','platform/linux/','cmd/'))})))")

# Excludes ESP-IDF's own downloaded/generated build/managed_components/.cache
# dirs (idf.py-owned, not our source — same reasoning as lint-format's prune).

# Static analysis across every platform; does its own parsing, so needs no build.
cppcheck:
    #!/bin/sh
    set -e
    excludes=""
    for d in $(find platform/esp-idf -type d \( -name build -o -name managed_components -o -name .cache \) 2>/dev/null); do
        excludes="$excludes -i$d"
    done
    # cppcheck parses without a build, so it has no generated configuration
    # header. Without one every CONFIG_WANTED_* reads as undefined: the host
    # guard's #error fires, and the configurable drivers analyse as absent.
    # Generate one from the Kconfig defaults (every driver on) into a scratch
    # dir so the analysis sees the code as it is actually built.
    cfg=$(mktemp -d); trap 'rm -rf "$cfg"' EXIT
    PYTHONPATH=tools/kconfiglib KCONFIG_CONFIG="$cfg/.config" \
        python3 tools/kconfiglib/olddefconfig.py Kconfig.engine >/dev/null
    PYTHONPATH=tools/kconfiglib KCONFIG_CONFIG="$cfg/.config" \
        python3 tools/kconfiglib/genconfig.py --header-path "$cfg/wanted-autoconf.h" \
        Kconfig.engine >/dev/null
    # SECURE_SOCKETS is a build-system define, not part of the generated header,
    # so derive it from the same .config — otherwise the analysis contradicts
    # itself and the host guard stops it.
    if grep -q '^CONFIG_WANTED_VFS_SOCKET_TLS=y$' "$cfg/.config"; then
        tls=1
    else
        tls=0
    fi
    cppcheck --enable=warning,style,performance,portability \
        --suppress=missingIncludeSystem --suppress=normalCheckLevelMaxBranches \
        --inline-suppr --error-exitcode=1 \
        -DSECURE_SOCKETS=$tls \
        -I include -I src/include -I platform/include -I "$cfg" \
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
