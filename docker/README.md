# Development docker image for Wanted Engine

For building and CI use.

## Usage for local dev

There is a helper script in the root directory of this project (`run-docker.sh`).
Just run it without parameters. Your `$PWD` needs to be set in the root directory of the project.

## Building the image for multiple platforms

One of the easiest way to build the image for multiple platfroms is to use `buildx` with `binfmt-qemu-static` and `qemu-user-static` installed.

```sh
docker buildx build --platform linux/amd64,linux/arm64 -t registry.gitlab.com/mekops/wanted/wanted-engine/build:latest --push .
```

Platforms are limited to `amd64` and `arm64` — the bundled wasi-sdk ships host
binaries for those two Linux arches only.

## ESP32 cross-build image (`Containerfile.esp32`)

A separate image for building the engine as a NuttX built-in app for the
**ESP32 (xtensa LX6)** target and flashing it over serial. It is kept out of the
main build image because the xtensa toolchain is large (~700 MB unpacked) and
only the ESP32 jobs need it.

It carries the `xtensa-esp-elf` GCC the NuttX ESP32 port documents
(`esp-14.2.0_20241119`), `esptool` (the `make` MKIMAGE/flash step), the NuttX
kconfig/kbuild prerequisites, and cmake/ninja — all routed through ccache.

Build:

```sh
# single arch
podman build -f docker/Containerfile.esp32 -t wanted-esp32 docker/
# multi arch + push
podman build --platform linux/amd64,linux/arm64 \
  -f docker/Containerfile.esp32 \
  -t registry.gitlab.com/mekops/wanted/wanted-engine/esp32-build:latest \
  --push docker/
```

Cross-build (no hardware needed):

```sh
podman run --rm -v "$PWD:/src" wanted-esp32 \
  bash -c 'cd third_party/nuttx && \
    ./tools/configure.sh -a ../nuttx-apps esp32-devkitc:nsh && make -j"$(nproc)"'
```

Flash to an attached board — pass the serial device through. On rootless podman
also preserve the host's supplementary groups so the container user can open the
`dialout`-owned device node:

```sh
podman run --rm -it \
  --device /dev/ttyUSB0 \
  --group-add keep-groups \
  -v "$PWD:/src" wanted-esp32 \
  bash -c 'cd third_party/nuttx && make flash ESPTOOL_PORT=/dev/ttyUSB0 ESPTOOL_BINDIR=./'
```

The image uses the same UID-remap entrypoint as the main build image (`entry.sh`
+ `gosu`), so under a GitLab `docker` runner `/src` is owned by the runner UID
and builds run as that user. For local **rootless podman**, add `--userns=keep-id`
so the bind-mount owner is your UID (not container root) and the entrypoint can
create the matching build user.

On an SELinux host (Fedora, RHEL), add `:z` to bind mounts so the container can
read them (e.g. `-v "$PWD:/src:z"`); without it the toolchain hits "Permission
denied" on the mounted sources.

Only `amd64` and `arm64` are built — Espressif publishes xtensa-esp-elf for
those two Linux arches only.

## Changelog

### 0.6.5

- add `libssl-dev`: `platform/linux`'s `find_package(OpenSSL)` needs it to enable `SECURE_SOCKETS` (TLS sockets + the real `/dev/ed25519` `PlatformEd25519Verify` body) — without it both silently fall back to stubs

### 0.6.4

- drop `osv-scanner`: it only flagged dependency manifests in upstream WAMR/NuttX language bindings, tests, and docs tooling the engine never builds, and had no visibility into the vendored C submodules it ships; `trivy` is retained

### 0.6.3

- mark `/src` as a git `safe.directory` so builds work under `RUNNER=docker`

### 0.6.2

- add static-analysis tooling: `cppcheck`, `shellcheck`, and `semgrep` (in a dedicated venv, its deps collide with distro packages); `python3-venv`
- add `trivy` (image/fs CVE + secret scan) and `osv-scanner` (dependency CVEs) from upstream release artifacts, per build arch

### 0.6.1

- add `just` for easier command handling and ci/dev unification

### 0.6.0

- add `ccache` for compiler caching, wired as a PATH masquerade (`/usr/lib/ccache`, with `cc`/`c++` added) so both the cmake engine builds and the NuttX kbuild compile route through one shared cache

### 0.5.0

- add nuttx tooling

### 0.4.0

- rebase on `debian:trixie-slim` (was `ubuntu:22.04`); smaller base, current toolchain (clang/lld 19, gcc 14)
- unversioned LLVM packages track the distro default; install with `--no-install-recommends`
- select the wasi-sdk bundle per build arch (`TARGETARCH`); drop the `arm/v7` image (no wasi-sdk host build)
- switch the wapp toolchain to the full bundled wasi-sdk v24 at `/opt/wasi-sdk` (its own clang + lld), replacing the system-clang-14 + supplemental `libclang_rt`/`wasi-sysroot` approach — the bundled lld defines `__heap_end`, which wasi-libc's allocator requires, so wapps that use `malloc` now link

### 0.3.0

- update WASI SDK v16 → v24 (last version compatible with the libclang_rt supplement approach; v26+ require switching to the full bundled SDK)
- add `cmake-curses-gui` (`ccmake` interactive CMake configurator)
- add `gdb` and `lldb-14` for native and LLVM debugging
- add `valgrind` for heap profiling and memory leak detection
- add `strace` for syscall tracing
- add `clang-format-14` and `clang-tidy-14` for formatting and static analysis
- add `wabt` (WebAssembly Binary Toolkit: `wasm-objdump`, `wasm2wat`, `wat2wasm`)
- add `jq` for JSON manipulation
- group `apt-get` package list with inline comments by category

### 0.2.0

- add `gcovr` for generating code coverage report in CI

### 0.1.1

- multiplatform image (amd64, aarch64, armv7)

### 0.1.0

- first release
- WASI Sdk v16
- clang 14
