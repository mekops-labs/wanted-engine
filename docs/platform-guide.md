---
title: "Platform Guide"
date: 2026-06-08T17:30:00+01:00
weight: 70
toc: true
description: "Building for and porting to each target: how the build target is configured, the Platform seam, Linux, the NuttX simulator, the RP2350 and ESP32 hardware targets, OpenWrt, and what a new port must implement."
---

The engine core is platform-agnostic; everything OS-specific lives behind the `Platform*` seam. This guide covers the seam, the shared POSIX core the production targets build on, each target, how the build is configured, and the checklist for a new port.

**Which target gets built is configuration, not a choice of recipe.** `just build` reads the target from this build directory's `.config` and dispatches — see [Target selection](#target-selection). The per-target build recipes it replaced are gone.

## The platform seam

Every platform implements the contract in `platform/include/platform.h`. A conforming port must provide a working body for all of it — there are no optional symbols:

| Area | Symbols |
|------|---------|
| Wapp lifecycle | `PlatformWappLoad` / `Unload` / `Start` / `Stop` / `Release` / `Loop` / `GetState`, `PlatformWorkerStackSize` |
| Registry backend | `PlatformRegistryRead` / `Write` / `Remove` / `WappLoad` / `ReadImage` |
| Filesystem | `PlatformOpenStateDir`, `PlatformFsRename`, `PlatformFsMkdir`, `PlatformVolumeRoot` |
| Network | `PlatformNetOpen` / `Connect` / `Recv` / `Send` / `Accept` / `Shutdown` / `Close` / `Free` |
| Clock | `PlatformClockGetRes` / `GetTime` / `NanoSleep` |
| Random | `PlatfromGetRandom` |
| Memory | `PlatformMemoryStats`; `PlatformExtram*` (`Malloc` / `Realloc` / `Free` / `EarlyInit`) — the external-RAM (PSRAM) heap backing the engine's large allocations (image cache, WAMR runtime) where one exists |
| Concurrency | `PlatformMutexNew` / `Lock` / `Unlock` / `Free` |
| Drivers | `PlatformDriverTable` — the platform's additions to the launch-config driver names (`gpio`/`wifi` on NuttX, `wifi`/`ota` on ESP-IDF, none on Linux). A build may add a third table from a tree outside this repo; see [Out-of-tree drivers](#out-of-tree-drivers) |
| Crypto | `PlatformSha256New` / `Update` / `Final` / `Free` — streaming digest behind `/dev/sha256` (software body in `posix/sha256.c`; ESP32-S3 uses the SHA peripheral; no `-ENOSYS` path). `PlatformEd25519Verify` — the one seam symbol allowed to report an absent backend (`-ENOSYS`); the `/dev/ed25519` verdict read surfaces it to the wapp. Real on Linux (OpenSSL) and NuttX (vendored `orlp/ed25519`); the ESP-IDF port still uses the dummy backend |
| Firmware update | `PlatformOtaInit` / `Confirm` / `GetBootState` / `BeginWrite` / `Write` / `Commit` / `Rollback` — the A/B OTA seam behind `/dev/ota`; real on ESP-IDF (`esp_ota_ops`), a fixed single-slot stub elsewhere |
| Power / process | `PlatformSetProcessArgs`, `PlatformRequestShutdown`, `PlatformRequestReboot`, `PlatformName` |

The invariants every platform must honour: a wapp runs on its own thread; `PlatformWappStop` must interrupt a wapp that is blocked in a host syscall (not merely set a flag and wait); `PlatformWappLoop` blocks until an explicit shutdown/reboot request and respawns a supervisor that exits on its own; memory stats report heap usage; the registry resolves a wapp image by name.

All recipes build inside the standardized container — the host only needs a container runtime. `just --list` lists them; on a bare host `make <recipe>` runs the same recipe in the container (append `RUNNER=docker` to use Docker).

## Shared POSIX core

Linux and the NuttX simulator are both POSIX environments, so most of the seam has **one** implementation they both compile in: `platform/posix/`. It carries the generic, syscall-backed bodies:

| Source | Provides |
|--------|----------|
| `posix/socket.c` | `PlatformNet*` — the BSD socket calls (open, connect, recv, send, accept, shutdown, close) |
| `posix/mutex.c` | `PlatformMutex*` |
| `posix/clock.c` | `PlatformClockGetRes` / `GetTime` |
| `posix/fs.c` | `PlatformOpenStateDir`, `PlatformFsRename`, `PlatformFsMkdir` |
| `posix/registry-store.c` | the filesystem registry store behind `PlatformRegistry*` |
| `posix/wapps-image.c` | image load/unload behind `PlatformWappLoad` / `Unload` |
| `posix/sha256.c` | the software SHA-256 body behind `PlatformSha256*` |

The ESP-IDF port also reuses `posix/socket.c` (over lwIP). A target layer (`platform/linux/`, `platform/nuttx/`) links those sources and implements only what genuinely differs between the two:

| Concern | Linux | NuttX |
|---------|-------|-------|
| Threads + stop | pthreads; cooperative `SIGUSR2` + WAMR terminate flag | tasks; cooperative `SIGUSR2` + WAMR terminate flag |
| Sleep (`PlatformNanoSleep`) | `api/clock-sleep.c` | `api/clock-sleep.c` (signal-EINTR quirk) |
| Secure sockets | OpenSSL (`api/ssocket.c`) | mbedTLS — the shared `posix/ssocket-mbedtls.c`, gated by `CONFIG_SYSTEM_WANTED_TLS` |
| Memory stats | `mallinfo2` | NuttX heap walk |
| Registry backend glue | host directory (`api/registry.c`) | hostfs (`api/registry.c`) |
| Entry point | `wanted-cli` `main` | `wanted_sim_main` (NuttX init task) |

`platform/dummy/` is the exception: a unit-test stub that implements the whole seam **in memory** and shares none of the POSIX sources — the model for a target that is not POSIX at all.

## Linux

The primary target.

```bash
just build                                  # build the configured target
just supervisor-variant wsh && just build   # ...with the wsh debug supervisor
```

- **Threads** — pthreads.
- **Stop mechanism** — cooperative: `PlatformWappStop` sets the WAMR terminate flag and sends `SIGUSR2` to the worker, so a wapp spinning in the interpreter and one blocked in a host call are both reaped promptly.
- **Registry** — a host-filesystem directory (`./registry/`) scanned for `<name>@<version>.wapp` images.
- **TLS** — OpenSSL-backed secure sockets (`T`/`U` socket options).
- **Memory stats** — `mallinfo2`.

CMake options of note: `WANTED_PLATFORM` (the platform layer) and `WANTED_DEFCONFIG` (seed the configuration from `configs/<name>_defconfig`). Everything else is Kconfig — see [Build configuration](#build-configuration). TLS is `CONFIG_WANTED_VFS_SOCKET_TLS`: the engine states the intent and the host supplies the backend (OpenSSL here, mbedTLS on NuttX and ESP-IDF), so a host that cannot provide one fails the build rather than silently handing back a binary whose secure sockets are rejected at launch.

## NuttX simulator

A first-class, CI-gated target: the full engine running as a NuttX application on the host-stack simulator. The `platform/nuttx/` layer is complete — every `Platform*` symbol has a working body, with no remaining stubs.

```bash
just nuttx-deps      # init the nuttx + nuttx-apps submodules, link the app package
just target nuttx && just build     # configure + build the sim
just nuttx-selftest  # run the in-WASM selftest suite on the sim
make nuttx-shell     # boot the sim to an interactive wsh prompt (host wrapper)
```

- **Board config** — `sim:wanted`, a native defconfig in the NuttX fork. The engine runs as the NuttX init task via `wanted_sim_main`, which `chdir`s to `/data` (so `./registry` resolves against the sim's hostfs) and powers the board off cleanly when `wanted_main` returns.
- **Console** — raw `write(1/2)` for output.
- **Stop mechanism** — cooperative: `PlatformWappStop` sets the WAMR terminate flag and sends `SIGUSR2` to the worker so a wapp blocked in a host call is interrupted and checks the flag on return. A per-worker `interrupted` flag bridges `clock_nanosleep`'s success-on-signal quirk (Linux reports `EINTR` directly and needs no such bridge).
- **Submodules** — `third_party/nuttx` and `third_party/nuttx-apps` are shallow submodules pinned to the `wanted` branch of the mekops forks; `just nuttx-deps` initialises them (idempotent); `just build` runs it for you on the nuttx target.

**Differences from Linux.** The `sim:wanted` board routes sockets to the host through usrsock (`CONFIG_SIM_NETUSRSOCK`), so `/net` sockets reach the host network; TLS comes from the shared raw-mbedTLS layer (`CONFIG_SYSTEM_WANTED_TLS` selects `apps/crypto/mbedtls` — same no-CA-bundle caveat as ESP-IDF: encrypted but unauthenticated). Neither target's cooperative stop can pre-empt a bare native call that never checks `EINTR`.

## Hardware targets

Three hardware target families live in the tree. Two are **NuttX** targets that share `platform/nuttx/` with the simulator — only the board config, the registry backend, and the entry point differ — and the third is a native **ESP-IDF** port under `platform/esp-idf/`.

### RP2350 (NuttX) — reference embedded target

The reference constrained target, and the one the control-plane story is proven on.

- **Core / boards** — ARM Cortex-M33 (RP2350). Two boards carry WANTED configs in the pinned NuttX fork: `adafruit-feather-rp2350:wanted` (the `wsh` debug supervisor) and `pimoroni-pico-2-plus-w:sheriff` (the Sheriff control-plane agent, with the onboard CYW43439 radio). A RISC-V/Hazard3 build of the silicon runs (PSRAM + `ostest` clean) but a WANTED board config for it is a deprioritized stretch.

  ```bash
  make rp2350 RP2350_CONFIG=pimoroni-pico-2-plus-w:sheriff
  make rp2350-flash-swd    # flash over SWD via a Raspberry Pi Debug Probe (no BOOTSEL)
  ```

- **Registry** — a LittleFS volume on a reserved region of the internal QSPI flash (the flash-MTD backend), written through the RP2350 ROM flash routines. Full wapp lifecycle (`create → start → running → stop → exited`) is hardware-verified.
- **PSRAM** — 8 MB external PSRAM on QMI CS1 (GPIO8), merged with internal SRAM into one ~8.5 MiB heap (`RP23XX_PSRAM_HEAP_SINGLE`). The large engine buffers (WAMR linear memory, the wapp image cache) live in PSRAM while worker stacks stay in scarce internal SRAM. Measured ceiling: ~38 concurrent wapps. Because flash program/erase and PSRAM share the QMI hardware, the internal-flash MTD driver cleans the XIP cache and saves/restores the CS1 registers around every flash op — without which a flash write silently corrupts PSRAM.
- **Crypto** — a **real Ed25519 verify**: NuttX's vendored mbedTLS has no Ed25519, so the port vendors `orlp/ed25519` (verify-only) behind `PlatformEd25519Verify`. The only target with a genuine (non-stub) Ed25519 backend on embedded silicon.
- **Wi-Fi (CYW43439)** — on the Pico Plus 2 W the pinned fork drives the onboard CYW43439 radio: the `wifi` driver is available to wapps, and the `:sheriff` boot path joins Wi-Fi before Sheriff's manager loop starts — SSID and passphrase are read interactively from the console (never baked into firmware), association goes through the NuttX WAPI library with DHCP retry. On a CYW43439 board Sheriff's manager socket is `tcp://`; a board without the radio uses the `serial://` USB-CDC link below. The RP2350 board configs do not enable `CONFIG_SYSTEM_WANTED_TLS` (the mbedTLS layer is proven on the sim; its flash/RAM cost on this board is unmeasured), so the control-plane transport is plain TCP or the wired serial link.
- **Console + flashing** — with the `:sheriff` config the console moves to UART0 (read it over the Debug Probe's UART bridge), which frees the native USB-CDC for the control plane. Flash over SWD with `make rp2350-flash-swd` (no button dance) or over USB in BOOTSEL with `make rp2350-flash`; `make rp2350-reset` resets a running board over SWD.
- **Control plane over USB-CDC** — on a board with no radio, Sheriff reaches a host Deputy over the native USB-CDC using the engine's `serial://` socket scheme (a device path in place of `host:port`). The full reconcile loop runs on real hardware (verified on the Feather RP2350): State Report uplink → Ed25519-verified signed Desired State → wapp `RUNNING`, and a wrongly-signed Desired State is rejected. This is the ecosystem's first genuine (not demo-stubbed) signed-workload verification on embedded hardware.
- **Secure boot** — validated entirely offline via `picotool seal --sign` (`make rp2350-sign`); the one-way OTP `SECURE_BOOT_ENABLE` fuse is deliberately never burned.

### ESP32-S3 (ESP-IDF)

A native ESP-IDF port (`platform/esp-idf/`, `app_main`) — not NuttX — targeting the ESP32-S3 (e.g. S3R8, octal PSRAM), 8 MB flash. Distinct from the *classic* ESP32 NuttX build below: same vendor, different silicon and OS.

```bash
just target esp-idf && just build   # -> platform/esp-idf/project/build/wanted-esp-idf.bin
```

Runs in the ESP-IDF toolchain image (`docker/Containerfile.esp-idf`), which is built locally rather than published. `just` must be the container *command*, not an `--entrypoint` override — the base image's entrypoint sources `export.sh`, and bypassing it leaves `idf.py` off `PATH`.

- **Threads / stop** — FreeRTOS via the ESP-IDF pthread wrapper; cooperative stop (the WAMR terminate flag aborts the in-flight call).
- **Registry / PSRAM** — flash-backed LittleFS registry (`registry_flash.c`); octal PSRAM via `extram.c`. Built with `DEFCONFIG=xiao_esp32s3` (`MAX_WAPPS=20`); measured on an S3 with 8 MB PSRAM: the supervisor plus 19 concurrent user wapps fit, the 20th `start` is rejected cleanly with `-ENOSPC`.
- **OTA** — A/B firmware update through `esp_ota_ops` (`ota.c`), with a pending-verify / rollback seam.
- **Secure sockets** — raw mbedTLS with ESP32-S3 hardware AES/SHA/ECC acceleration. No CA bundle is provisioned (`MBEDTLS_SSL_VERIFY_NONE`), so `tcps://` here is encrypted but **unauthenticated** — a demo transport, not production TLS.
- **Crypto** — SHA-256 is hardware-backed, but **Ed25519 verify is not yet ported** (still the dummy backend, `platform/dummy/dummy-crypto.c`). So an ESP32-S3 control-plane demo reconciles with signature verification stubbed — the genuine Ed25519 path is the RP2350's.

### Classic ESP32 (Waveshare ESP32 One, NuttX)

The `nuttx` target with board `esp32-devkitc:wanted` — no separate target, just a different pass-through board string. Bring-up landed in engine 0.8.0: a ROMFS-boot supervisor, an in-RAM image cache, a PSRAM heap, a registry, and the `gpio` / `wifi` drivers.

```bash
make esp32          # cross-build -> third_party/nuttx/nuttx.bin (xtensa toolchain image)
make esp32-flash    # esptool over ESP32_PORT (default /dev/ttyUSB0)
```

`make esp32` exists because the xtensa toolchain lives in its own container: the wrapper selects that image and sets the board and envelope, then runs the same `just build` inside it. Container choice is the one thing the engine's configuration cannot decide for itself.

## OpenWrt

Routers, built as an `.ipk` package from an OpenWrt SDK. Unlike NuttX and ESP-IDF, OpenWrt builds the engine as an **external package** — it compiles no engine sources into its own tree and shares no symbol namespace.

```bash
just target openwrt && just build    # -> dist/wanted-engine_<ver>_<arch>.ipk
```

The **OpenWrt SDK** choice picks which SDK cross-builds the package:

| Choice | OpenWrt target | Architecture |
|---|---|---|
| `WANTED_OPENWRT_SDK_AARCH64` | `armsr/armv8` | 64-bit ARM |
| `WANTED_OPENWRT_SDK_MIPSEL` | `malta/le` | 32-bit little-endian MIPS |
| `WANTED_OPENWRT_SDK_CUSTOM` | any — you name it | auto-detected |

**The first two are generic, board-independent SDKs on purpose** — the engine is built for an architecture, not for a particular router.

For any other target, choose **custom** and give `CONFIG_WANTED_TARGET_OPENWRT_SDK_URL` either a URL to an SDK archive (`.tar.zst` / `.tar.xz`, downloaded and cached) or a path to one already extracted:

```bash
just target openwrt
just setconfig WANTED_OPENWRT_SDK_CUSTOM=y
just setconfig 'WANTED_TARGET_OPENWRT_SDK_URL="https://downloads.openwrt.org/releases/24.10.0/targets/ath79/generic/openwrt-sdk-24.10.0-ath79-generic_gcc-13.3.0_musl.Linux-x86_64.tar.zst"'
just build
```

The target, architecture, and toolchain prefix are auto-detected from the SDK's own `staging_dir` layout, so a custom SDK needs nothing else stated — which is why the engine carries no table of OpenWrt targets to fall out of date.

The SDK is downloaded and cached under `.openwrt-sdk/` on first use, and OpenSSL is staged into it once (skipped when TLS is off, which is why the qemu lane below is faster). The cache is extracted with `--no-same-owner`: the tarballs carry the build farm's uid, and preserving it leaves the SDK owned by a user that does not exist locally.

The same SDK toolchain backs the cross-architecture test lane — see [Testing Guide](testing-guide.md#cross-architecture-selftest-qemu). Running the suite under qemu is the cheap way to catch faults that are invisible on x86: alignment, calling convention, signal handling.

## Build configuration

The engine is configured through a **Kconfig** tree at the repository root, read by the vendored kconfiglib in `tools/kconfiglib`. Configuring produces `.config` and a generated `wanted-autoconf.h` that every engine source compiles against; there is no second place a value can come from.

Each build directory owns its own `.config`, so a debug build, a cross build, and the extra-drivers lane can differ without fighting each other:

```bash
just menuconfig                  # edit this build dir's configuration
just defconfig small             # seed it from a named envelope
DEFCONFIG=openwrt just build     # seed on first configure, then build
just savedefconfig my_board      # write the minimal diff back
```

`DEFCONFIG` and the `defconfig` / `savedefconfig` recipes name the envelope **without** the `_defconfig` suffix; the file is `configs/<name>_defconfig`.

Editing `.config` re-runs the configure step and regenerates the header, so a changed configuration cannot leave stale values compiled in.

### Target selection

**Which target gets built is configuration, not a choice of recipe.** The `Target` menu selects linux, nuttx, esp-idf or openwrt and carries that target's build inputs; `just build` reads it and dispatches, echoing the target before anything runs.

```bash
just target nuttx                                          # linux | nuttx | esp-idf | openwrt
just setconfig 'WANTED_TARGET_NUTTX_BOARD="sim:wanted"'    # that target's input
just build
```

| Target | Input | Default | Reaches |
|---|---|---|---|
| `linux` | — | — | cmake + ninja |
| `nuttx` | `CONFIG_WANTED_TARGET_NUTTX_BOARD` | `sim:wanted` | NuttX `tools/configure.sh` |
| `esp-idf` | `CONFIG_WANTED_TARGET_ESP_IDF_CHIP` | `esp32s3` | `idf.py set-target` |
| `openwrt` | the **OpenWrt SDK** choice — see below | `aarch64` | the OpenWrt SDK packaging script |

**Boards and SDKs are pass-through strings, never enumerated here.** `sim:wanted` reaches `configure.sh` verbatim and the engine holds no opinion about which boards exist — modelling them would be a second source of truth that drifts from the vendor tree. The same string selects the simulator or real hardware; what differs for hardware is the toolchain container, which the `Makefile` picks (`make esp32`, `make rp2350`).

Because `.config` is per build directory, two targets can be configured side by side:

```bash
BUILD_DIR=build-mips just target openwrt
BUILD_DIR=build-mips just setconfig WANTED_OPENWRT_SDK_MIPSEL=y
BUILD_DIR=build-mips just build      # .ipk, while build/ stays on linux
```

The cost of configuring rather than naming the target is that a stale `.config` builds something other than what you expected. That is why the target is echoed at the top of every build.

### How the tree is split

One menu, three files:

```
Kconfig            top level — sources both; what `just menuconfig` opens
├─ Kconfig.target  how to build it: target, board, SDK
└─ Kconfig.engine  what the binary IS — host-agnostic, sourced by hosts
```

`wanted-autoconf.h` is generated from `Kconfig.engine` alone while `.config` spans the whole tree. So an SDK URL or a board string never reaches a translation unit, and no engine source can test a build-orchestration symbol; the build system reads those off `.config`, which it already parses.

The split exists for the hosts. NuttX and ESP-IDF compile engine sources into their own trees and source `Kconfig.engine`, not the top level — they have answered the target question by existing, and asking again would give the build two answers. A `Target` menu in the shared file would have contradicted that.

### General and host-build options

Two menus hold the compilation knobs, split by the same rule as the files:

**General** (`Kconfig.engine`) is for options that change the binary, so every host gets them:

| Symbol | Default | Effect |
|---|---|---|
| `CONFIG_WANTED_DEBUG_TRACES` | `n` | Compiles in the `DEBUG_TRACE()` call sites — VFS routing, wapp lifecycle, driver open/close. Off, they expand to nothing. A development aid, not an operational log: the traces bypass the per-wapp log capture and are verbose enough to change timing on a constrained target. |

**Host build options** (`Kconfig.target`, shown for the linux and openwrt targets) drive CMake and never reach a translation unit — NuttX and ESP-IDF run no CMake of ours and could not honour them:

| Symbol | Default | Effect |
|---|---|---|
| `CONFIG_WANTED_BUILD_COVERAGE` | `n` | `--coverage -O0` across every target, so `just coverage` can report. Applied before any target is defined, so the engine library is instrumented too — not just the test binary. |
| `CONFIG_WANTED_BUILD_STATIC_CLI` | `n` | Links `wanted-cli` with `-static`. Needs a static OpenSSL if secure sockets are on, and glibc still wants its shared NSS modules to resolve hostnames — see the symbol's help. |
| `CONFIG_WANTED_EXTRA_DRIVERS_DIR` | `""` | An out-of-tree driver tree; see [Out-of-tree drivers](#out-of-tree-drivers). |

### Resource limits

The static memory envelope is set here. Every symbol is prefixed `CONFIG_WANTED_` without exception — engine and host symbols reach the same translation unit on NuttX and ESP-IDF, and an unprefixed name is a collision waiting to happen.

| Symbol | Default | Sizes |
|---|---|---|
| `CONFIG_WANTED_MAX_WAPPS` | 3 | concurrent wapp instances (and, via the log slots, the per-wapp log rings) |
| `CONFIG_WANTED_WASM_STACK_SIZE` | 8192 | per-instance operand (interpreter) stack |
| `CONFIG_WANTED_WASM_HEAP_SIZE` | 8192 | per-instance app heap |
| `CONFIG_WANTED_WASM_MAX_MEMORY_PAGES` | 1 | per-instance linear-memory ceiling, in 64 KiB pages (`0` = uncapped) |
| `CONFIG_WANTED_MAX_PATH_LEN` | 256 | VFS path buffers |

Driver allocation sizes that size a static structure are configurable too, under their own submenu: the pipe ring and pipe count, the per-wapp log capacity, and the log mount's open-handle table. Driver *behavioural* knobs (poll intervals and the like) stay local to their driver — the criterion for appearing in the configuration is whether the value sizes an allocation.

### Selectable drivers

Drivers a launch config reaches by name can be deselected, which drops their source, their factory-table row, and their declaration together. `9p` and `inflate` also drag the vendored c9 and uzlib libraries out of the build, so they are the two largest wins. The VFS core — `tarfs`, `devfs`, `netfs`, `procfs`, plus `null`/`log`/`pipe`/`stdio`/`virtual` — is mandatory and carries no symbols.

### A wapp's memory

Three engine-controlled regions are passed to WAMR per instance:

- **Operand stack** (`WASM_STACK_SIZE`) — the interpreter's evaluation stack, in host memory, **outside** linear memory. Distinct from the wapp's C aux stack, which lives inside linear memory and is fixed by the wapp's own linker (`wasm-ld -z stack-size`).
- **App heap** (`WASM_HEAP_SIZE`) — a host-managed heap for `wasm_runtime_module_malloc`, **outside** linear memory. WAMR disables it when the module exports its own `malloc`/`free`, so a WASI wapp (which allocates from its libc heap at the top of linear memory) usually does not use it.
- **Linear memory** (`WASM_MAX_MEMORY_PAGES`) — the memory the wapp actually addresses: its data, C aux stack, and libc heap. Enforced two ways: WAMR bounds `memory.grow` to the cap at runtime, and the engine refuses at load any image whose declared *initial* memory exceeds it (otherwise WAMR clamps the cap up to the module's initial, letting a large initial bypass the runtime bound). `0` disables both. (A module containing no `memory.grow` is collapsed by WAMR to a single fixed page - `WAMR_BUILD_SHRUNK_MEMORY` flag is on by default.)

### Defconfigs

`configs/` holds two kinds of defconfig. **Envelopes** describe a capacity class; **boards** describe a specific target and additionally pick its supervisor and install paths.

| Envelope | Target class | wapps | stack / heap | linear cap |
|---|---|---|---|---|
| `tiny` | no-PSRAM (ESP32-WROOM, ~180 KB internal RAM) | 2 | 4 KiB / 4 KiB | 1 page |
| `constrained` | ~512 KB RAM, PSRAM (ESP32-WROVER/NuttX) — the defaults | 3 | 8 KiB / 8 KiB | 1 page |
| `small` | routers (128 MB–1 GB) | 16 | 64 KiB / 256 KiB | 16 pages |
| `big` | Linux / cloud | 64 | 128 KiB / 1 MiB | uncapped |

| Board | Host | Notes |
|---|---|---|
| `xiao_esp32s3` | ESP-IDF | octal PSRAM, app heap off, linear memory capped at 2 pages so a full house fits the 8 MB part; `-storage` variant trades wapp slots for persist space |
| `esp32-nuttx` | NuttX | classic ESP32; 24 KiB worker stacks, which must fit scarce internal DRAM |
| `openwrt` | OpenWrt | packaged `.ipk`; supervisor read from its install path |

A board defconfig exists only where the board needs something an envelope does not give it. The RP2350 has no entry because `small` already describes it exactly — build it with `DEFCONFIG=small` rather than carrying a file that restates those numbers and would silently stop tracking them.

A defconfig seeds a build directory that has no `.config` yet; it never overwrites an existing one, so a configuration you edited is not silently replaced by a rebuild.

**Keep the target out of a defconfig.** `just savedefconfig` records whatever the build directory holds, including `CONFIG_WANTED_TARGET_*` if you changed it — but an envelope describes a capacity class, not a target, and the two are independent axes. A stray target line is harmless in practice (a host reading `Kconfig.engine` drops it silently, which is the right answer there) but it makes the file claim something it does not mean. Save envelopes from a directory left at the default target, or delete the line.

### Supervisor variant

Which supervisor the engine boots is part of the configuration rather than a build flag:

```bash
just supervisor-variant wsh      # sheriff | wsh | selftest
```

The choice sets the image path. A package that installs the image elsewhere sets `CONFIG_WANTED_SUPERVISOR_IMAGE_PATH` to an absolute path, which wins — that is for where the image *lives*, not which one it is.

### Hosts that build the engine themselves

NuttX and ESP-IDF compile engine sources into their own trees. They read **`Kconfig.engine`**, not the top-level `Kconfig`, and generate the engine's header from an engine `.config`; their own Kconfig files carry only the **edges** that cross into host symbols (`depends on NET`, `select CRYPTO_MBEDTLS`). The engine's own tree must never reference a host symbol: it is also read standalone, and kconfiglib resolves an unknown symbol to `n` with at most a warning — a feature would disappear with nothing to show why.

The host also decides what only it can know. NuttX's `CONFIG_SYSTEM_WANTED_TLS` sets the engine's `WANTED_VFS_SOCKET_TLS` for it, so a board built without mbedTLS cannot meet an engine that assumed TLS was there — rather than each board defconfig restating the answer.

OpenWrt instead builds the engine as an external package and shares no symbol namespace at all. `src/include/wanted-host-guard.h` catches what a preprocessor can there — a self-contradictory configuration, or a build that never ran the Kconfig step. A missing host library still surfaces at link time.

### Measuring the footprint

- `just sizes` reports each profile's per-wapp and worst-case memory for both the host (LP64) and 32-bit embedded (ILP32) ABIs, measured from the real engine structs, but it's just approximate value (e.g. wamr overhead is arbitrary worst case value), it doesn't actually measure the whole runtime overhead on specific hardware, using specifc compiler, just the struct sizes.
- `just memcap` is a negative test that verifies the `WASM_MAX_MEMORY_PAGES` cap actually bounds a wapp's `memory.grow`.

## Out-of-tree drivers

A driver written for one deployment — a router's config store, a site-specific sensor bus — does not have to live in this repo to be linked into a target. Point the build at a source tree that supplies `ExtraDriverTable()` and its entries join the launch-config driver names:

```bash
just setconfig 'WANTED_EXTRA_DRIVERS_DIR="/path/to/tree"'
just build
```

The tree's `CMakeLists.txt` defines a library target named `wanted_extra_drivers`; the engine adds its headers to that target. The coupling is source-level — the tree is compiled as part of this build against `vfs-drivers.h`, so there is no binary ABI to keep stable, and no runtime loader. `test/extra-drivers/` is a minimal working example. With the option unset, a default `ExtraDriverTable()` returning `NULL` is compiled in.

Two properties are worth stating plainly:

- **The extra table is searched last** — core names first, then the platform's, then the extra tree's. A tree claiming `wanted`, `socket`, or any other core name cannot shadow it.
- **An extra driver runs at full engine privilege.** Living outside this repo keeps it out of core review; it does not put it outside the trust boundary. A fault in it faults the engine. For a driver that should be isolated instead, run it as a 9P server process and grant the wapp a `9p` mount — `unix://<socket-path>` reaches one on the same box.

## Porting to a new platform

1. Create `platform/<name>/`. If the target is POSIX-like, link `platform/posix/` and implement only the deltas (thread/stop model, memory stats, secure sockets if any, registry-backend glue, entry point); otherwise implement every `Platform*` symbol from `platform.h` yourself, with `platform/dummy/` as the model for a from-scratch port. Either way, no stubs.
2. Provide a stop mechanism that **interrupts a blocked host syscall**, not just a cooperative flag.
3. Implement a registry backend (filesystem, flash blob, or in-memory) behind `PlatformRegistry*`.
4. Wire the build (`WANTED_PLATFORM=<name>`) and a board/app entry point.

The NuttX port is the reference for a constrained target. Its findings carry over: keep `wamrData_t` opaque and reach it through the `WantedWappTerminate` accessor; avoid VLAs; avoid `scandir` and GNU extensions (use `opendir`/`readdir`/`qsort`); confirm `CONFIG_INTERPRETERS_WAMR_THREAD_MGR` so `wasm_runtime_terminate` takes effect, and that thread cleanup does not rely on `pthread_cleanup_push`.

## See also

- [Architecture](architecture.md) — where the platform seam sits in the system.
- [Testing Guide](testing-guide.md) — the same suites run on Linux and the NuttX sim.
