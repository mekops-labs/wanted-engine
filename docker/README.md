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

### Triggering BOOTSEL over SWD (no physical button needed)

`picotool load`/`rp2350-flash` need the board in BOOTSEL, normally a physical
hold-BOOTSEL-tap-RESET. If the Debug Probe is already attached and the current
firmware is healthy (not hung), SWD alone can trigger BOOTSEL by calling the
RP2350 bootrom's `reboot()` ROM function directly on the halted core — the
same mechanism `picotool reboot -c riscv`/`-u` uses over USB, just invoked
through the debugger instead. Two ROM lookups are needed: `rom_table_lookup`
(its address is a fixed 16-bit value at ROM offset `0x16`) resolves the actual
`reboot` function by 2-character code, matching the addresses in
`arch/arm/src/rp23xx/rp23xx_rom.h` (`ROM_TABLE_CODE('R','B')` = `0x4252`,
`RT_FLAG_FUNC_ARM_SEC` = `4`), then `reboot(flags, delay_ms, p0, p1)` with
`flags = REBOOT2_FLAG_REBOOT_TYPE_BOOTSEL | REBOOT2_FLAG_REBOOT_TO_ARM`
(`0x2 | 0x10 = 0x12`) does the actual switch:

```sh
gdb-multiarch -q -batch \
  -ex "set architecture arm" \
  -ex "file nuttx"  `# any valid ELF - only used for gdb's call-frame setup, need not match current flash` \
  -ex "target extended-remote localhost:3333" \
  -ex "monitor halt" \
  -ex "print/x *(unsigned short*)0x16" \
  -ex 'print/x ((void*(*)(unsigned int,unsigned int))$1)(0x4252, 4)' \
  -ex 'print/x ((int(*)(unsigned int,unsigned int,unsigned int,unsigned int))$2)(0x12, 10, 0, 0)'
```

The final call resets the chip into BOOTSEL and never returns cleanly (the
gdb session reports a disconnect/timeout — expected, not an error). Confirm
with `picotool info -a` from the host; it should now report the family/image
type instead of "No accessible RP-series devices in BOOTSEL mode". `file`
needs *some* ELF loaded (any board's `nuttx` binary works) purely so gdb has
entry-point context to set up its inferior-call return breakpoint — it does
not have to match what's actually flashed.

**This only works if the target actually halts.** If the running firmware is
stuck in a low-power/blocking-syscall state, `monitor halt` (and even a plain
`openocd ... -c halt` with no gdb involved) can time out indefinitely
("target was in unknown state when halt was requested"). Use `rescue_reset`
first to force both cores to a known-halted state in the bootrom regardless
of what the firmware was doing:

```sh
openocd -f interface/cmsis-dap.cfg -c "adapter speed 1000" -f target/rp2350.cfg \
  -c "init" -c "rescue_reset" -c "halt"
```

`rescue_reset` is a recovery mechanism, not a safe place to resume execution
from — attempting the `reboot()` call sequence *from* a rescue-reset halt (as
opposed to a halt on genuinely running application code) reliably faults
("clearing lockup after double fault"), even after pointing `$sp` at real
SRAM. If `rescue_reset` was needed to regain control, skip the ROM-call dance
entirely and just reflash directly over SWD instead — raw SWD flash+reset
works for ARM→ARM (no picotool/BOOTSEL involved at all); it's only an
ARM↔RISC-V *architecture* switch that raw SWD can't do (see the RISC-V
section below):

```sh
openocd -f interface/cmsis-dap.cfg -c "adapter speed 1000" -f target/rp2350.cfg \
  -c "init" -c "rescue_reset" -c "program nuttx verify reset exit"
```

## RP2350 RISC-V cross-build + debug image (`Containerfile.rp2350-riscv`)

Sibling of `Containerfile.rp2350`, targeting the RP2350's *other* on-die
core: the RISC-V Hazard3. Same physical chip, same board — which core
architecture boots is a runtime/flash selection, not a hardware
difference. Only the cross-compiler differs from `Containerfile.rp2350`;
the debug half (openocd, gdb-multiarch, probe-rs, picocom) is identical
and already supports both cores (the Raspberry Pi openocd fork ships
`target/rp2350-riscv.cfg` alongside `target/rp2350.cfg`).

```sh
podman build -f docker/Containerfile.rp2350-riscv -t wanted-rp2350-riscv docker/
```

Cross-build (no hardware needed) — same mount layout as the ARM image:

```sh
podman run --rm \
  -v /path/to/nuttx:/src/nuttx:Z \
  -v /path/to/nuttx-apps:/src/nuttx-apps:Z \
  --entrypoint=/bin/sh wanted-rp2350-riscv -c \
  'cd /src/nuttx && ./tools/configure.sh raspberrypi-pico-2-rv:nsh && make -j"$(nproc)"'
```

### Two gotchas specific to this toolchain

Debian's `gcc-riscv64-unknown-elf` package (14.2.0, has the Zba/Zbb/Zbs/
Zbkb/Zcb/Zcmp extension support NuttX's rp23xx-rv Kconfig needs — those
are recent extensions requiring GCC 13+) has two gaps the ARM package
doesn't:

1. **No `riscv32-unknown-elf-*` binary names.** It only ships
   `riscv64-unknown-elf-*`, even though it's a multilib toolchain that
   targets rv32 fine via `-march=`. NuttX's rp23xx-rv `Toolchain.defs`
   resolves `CROSSDEV` to `riscv32-unknown-elf-`, so the image aliases the
   whole toolchain (and `gdb-multiarch`) under that prefix.
2. **No libc wired in by default.** Unlike `libnewlib-arm-none-eabi`,
   `picolibc-riscv64-unknown-elf` installs its headers/libs under
   `/usr/lib/picolibc/riscv64-unknown-elf/`, not gcc's actual default
   sysroot search path (`/usr/lib/riscv64-unknown-elf/`, confirmed via
   `gcc -E -Wp,-v -`) — so `<math.h>` and friends 404 without a symlink
   bridging the two.

### Flashing a RISC-V image needs `picotool`, not raw SWD

A RISC-V-tagged UF2 (`file nuttx.uf2` reports `RISC-V image`) does **not**
switch the RP2350's boot architecture just by being written to flash over
SWD (`openocd ... program nuttx.uf2 reset`) — the chip comes back up on
whichever core was already selected (ARM, by default). The architecture
switch is a `picotool` operation: with the board in BOOTSEL mode,

```sh
picotool load -x nuttx.uf2
```

recognizes the `rp2350-riscv` family ID and performs the switch as part
of loading — confirmed by `openocd -f target/rp2350-riscv.cfg` then
showing `rp2350.rv0`/`rp2350.rv1` as `running` (XLEN=32, Hazard3) instead
of `unavailable`. This matches the RPi openocd fork's own comment in
`target/rp2350-riscv.cfg`: *"each CPU must already be selected (e.g. from
`picotool reboot -u -c riscv`)"* — `picotool reboot -c riscv` on an
already-running board does the same switch without a full reflash, but
needs either BOOTSEL or a currently-running image that exposes a USB
PICOBOOT reset endpoint.

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
