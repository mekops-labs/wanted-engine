# WANTED Engine — canonical command runner.
#
# Every build/test/lint recipe lives here and assumes it runs *inside* the
# standardized build container (toolchain on PATH, repo at the working dir).
# That is how CI and the devcontainer invoke them. On a bare host, the root
# `Makefile` is a thin wrapper that runs these same recipes in the container
# (`make build` == `just build` in the image) — see Makefile.
#
# Overrides are read from the environment so one recipe serves local + CI:
#   BUILD_DIR (default build) · PROFILE (tiny|constrained|small|big)
#   CC · CMAKE_EXTRA_ARGS · NUTTX_SKIP_BUILD · NUTTX_CLEAN

build_dir := env_var_or_default("BUILD_DIR", "build")
profile   := env_var_or_default("PROFILE", "")
cmake_extra := env_var_or_default("CMAKE_EXTRA_ARGS", "")
wsh_tar   := "./wasm/supervisor/wsh/supervisor.tar"

# Optional resource-limit profile (cmake/profiles/<name>.cmake).
profile_arg := if profile != "" { "-C cmake/profiles/" + profile + ".cmake" } else { "" }

# First-party C/H sources (vendored deps and generated trees are excluded).
src_dirs := "src platform cmd include"
# clang-tidy reads flags per TU from this build's compile_commands.json.
tidy_build_dir := env_var_or_default("TIDY_BUILD_DIR", "build-clang")

# List available recipes.
default:
    @just --list

# --- build ----------------------------------------------------------------

# Build the engine + CLI and run the unit suite.
all: build test

# Compile the supervisor TAR images (sheriff committed; wsh/selftest from source).
supervisor:
    make -C wasm/supervisor

# Compile the sample wapp images under wapps/ (excludes the wsh supervisor).
wapps:
    #!/bin/sh
    set -e
    for d in wapps/*/; do
        [ "$d" = wapps/wsh/ ] && continue
        [ -f "${d}Makefile" ] && make -C "$d"
    done

# Build the engine + CLI with the production (sheriff) supervisor [PROFILE=...].
build: supervisor
    mkdir -p {{build_dir}}
    cd {{build_dir}} && cmake -GNinja {{profile_arg}} {{cmake_extra}} .. && ninja

# Build the engine + CLI with the wsh debug supervisor compiled in [PROFILE=...].
wsh: supervisor
    mkdir -p {{build_dir}}
    cd {{build_dir}} && cmake -GNinja {{profile_arg}} {{cmake_extra}} -DWANTED_SUPERVISOR_IMAGE_PATH={{wsh_tar}} .. && ninja

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

# Run the system-control (poweroff/reboot/exit) checks on Linux.
syscontrol:
    ./test/syscontrol.sh ./{{build_dir}}/cmd/wanted-cli

# Negative test: WASM_MAX_MEMORY_PAGES bounds a wapp's linear-memory growth.
memcap: supervisor
    ./test/memcap.sh

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
nuttx-build: supervisor
    ./test/nuttx-sim.sh deps build

# Build just the sim kernel binary (no hostfs staging) — the split-CI artifact.
nuttx-kernel:
    ./test/nuttx-sim.sh kernel

# Run the in-WASM selftest suite on the NuttX sim.
nuttx-selftest: supervisor
    ./test/nuttx-sim.sh selftest

# Run the system-control (poweroff/reboot/exit) checks on the NuttX sim.
nuttx-syscontrol: supervisor
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
    # Only wsh/selftest app.wasm are generated from wapps/ source; sheriff's is a
    # committed blob, so never delete it.
    rm -f wasm/*.wasm* wasm/supervisor/*/supervisor.tar \
          wasm/supervisor/wsh/app.wasm wasm/supervisor/selftest/app.wasm
    rm -f wapps/*/*.wasm wapps/*/*.wasm.h wapps/*/*.o
    # NuttX kernel objects + .config live in the submodule tree; an incremental
    # rebuild over a stale tree silently runs old code, so distclean it too.
    ./test/nuttx-sim.sh clean

# --- lint / static analysis / security ------------------------------------

# All blocking lint checks.
lint: lint-format lint-shell

# Reject any formatting drift (clang-format config in .clang-format).
lint-format:
    find {{src_dirs}} \( -name '*.c' -o -name '*.h' \) -print0 | xargs -0 clang-format --dry-run --Werror

# Reformat the tree in place (developer helper; not run in CI).
format-fix:
    find {{src_dirs}} \( -name '*.c' -o -name '*.h' \) -print0 | xargs -0 clang-format -i

# Lint shell scripts. error severity only for now; ratchet down over time.
lint-shell:
    find . -name '*.sh' -not -path './vendor/*' -not -path './third_party/*' -not -path './build*/*' -print0 \
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
cppcheck:
    cppcheck --enable=warning,style,performance,portability \
        --suppress=missingIncludeSystem --suppress=normalCheckLevelMaxBranches \
        --inline-suppr --error-exitcode=1 \
        -I include -I src/include -I platform/include \
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
