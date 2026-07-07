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

## RP2350 cross-build + debug image (`Containerfile.rp2350`)

A dedicated image for building the engine as a NuttX built-in app for the
**RP2350 (ARM Cortex-M33, Adafruit Feather RP2350)** target, and for driving
the board on the bench over SWD with a **Raspberry Pi Debug Probe** (any
CMSIS-DAP adapter works). It carries `gcc-arm-none-eabi` + `picotool` for the
cross-build/flash half, and `openocd` (built from the **Raspberry Pi fork**,
not Debian's package — Debian's 0.12.0 has no RP2350 target support) +
`gdb-multiarch` + `probe-rs` + `picocom` for the debug half.

Build:

```sh
podman build -f docker/Containerfile.rp2350 -t wanted-rp2350 docker/
```

Cross-build (no hardware needed) — mount the NuttX tree and its `apps/`
sibling so `tools/configure.sh` finds `../nuttx-apps` by its usual relative
lookup; the container entrypoint expects the source at `/src`:

```sh
podman run --rm \
  -v /path/to/nuttx:/src/nuttx:Z \
  -v /path/to/nuttx-apps:/src/nuttx-apps:Z \
  --entrypoint=/bin/sh wanted-rp2350 -c \
  'cd /src/nuttx && ./tools/configure.sh adafruit-feather-rp2350:nsh && make -j"$(nproc)"'
```

Flash over the board's main USB (BOOTSEL mode: hold BOOTSEL, tap RESET) —
`picotool` works directly on the host, no container needed:

```sh
picotool load -x /path/to/nuttx/nuttx.uf2
```

### Debug Probe: SWD (openocd/gdb) + UART console

The Debug Probe exposes **two USB interfaces**: a CMSIS-DAP interface (SWD,
`2e8a:000c`) for `openocd`, and a UART bridge that shows up on the host as a
plain `/dev/ttyACM*` — wire its UART lines to the target's console UART pins.
This UART is independent of the board's native USB port, so it's the
reliable console path on boards/configs where native USB-CDC (`usbnsh`)
doesn't come up.

Console — no container needed, `picocom` also runs fine on the host:

```sh
picocom -b 115200 /dev/ttyACM0
```

SWD, from inside the container — pass the raw USB bus through. Under
rootless podman this needs **three** things together, not just `:Z` on the
bind mounts: `--userns=keep-id` (so the container's UID matches yours),
`--group-add keep-groups` (so it inherits your `plugdev`-equivalent group
membership), and on an SELinux host, `--security-opt label=disable` — `:Z`
relabels bind-mounted *files*, but a raw `--device /dev/bus/usb` passthrough
is denied by SELinux without it, and the failure mode is a generic
"Permission denied" with no AVC hint in the container's own output:

```sh
podman run --rm --userns=keep-id --device /dev/bus/usb \
  --group-add keep-groups --security-opt label=disable \
  --entrypoint=/bin/sh wanted-rp2350 -c \
  'openocd -f interface/cmsis-dap.cfg -c "adapter speed 5000" -f target/rp2350.cfg'
```

This starts a gdb server on `:3333` (also telnet on `:4444`). From a second
shell in the same container (or `gdb-multiarch` on the host if it has RP2350
gdb scripts available):

```sh
gdb-multiarch -q -ex "target extended-remote localhost:3333" nuttx.elf
```

RP2350 has **two Cortex-M33 cores** (`rp2350.cm0`/`rp2350.cm1`); openocd
targets `cm0` by default. A `halt` on one core does not imply the other is
halted too — `resume`/`reset run` both cores explicitly before ending the
session, or the board is left stuck and stops responding on the UART console
until a clean `reset run` is issued.

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
