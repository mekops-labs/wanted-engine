---
title: "Platform Guide"
date: 2026-06-08T17:30:00+01:00
weight: 70
toc: true
description: "Building for and porting to each target: the Platform seam, Linux, the NuttX simulator, the RP2350 and ESP32 hardware targets, and what a new port must implement."
---

The engine core is platform-agnostic; everything OS-specific lives behind the `Platform*` seam. This guide covers the seam, the shared POSIX core that both production targets build on, the two targets themselves, and the checklist for a new port.

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
just build        # engine + CLI with the production sheriff supervisor
just wsh          # engine with the wsh debug supervisor
```

- **Threads** — pthreads.
- **Stop mechanism** — cooperative: `PlatformWappStop` sets the WAMR terminate flag and sends `SIGUSR2` to the worker, so a wapp spinning in the interpreter and one blocked in a host call are both reaped promptly.
- **Registry** — a host-filesystem directory (`./registry/`) scanned for `<name>@<version>.wapp` images.
- **TLS** — OpenSSL-backed secure sockets (`T`/`U` socket options).
- **Memory stats** — `mallinfo2`.

CMake options of note: `WANTED_PLATFORM` (the platform layer), `WANTED_SUPERVISOR_IMAGE_PATH` (compile-time supervisor image), `SECURE_SOCKETS` (TLS), `WANTED_EXTRA_DRIVERS_DIR` (an out-of-tree driver tree).

## NuttX simulator

A first-class, CI-gated target: the full engine running as a NuttX application on the host-stack simulator. The `platform/nuttx/` layer is complete — every `Platform*` symbol has a working body, with no remaining stubs.

```bash
just nuttx-deps      # init the nuttx + nuttx-apps submodules, link the app package
just nuttx-build     # configure + build the sim from a clean tree
just nuttx-selftest  # run the in-WASM selftest suite on the sim
make nuttx-shell     # boot the sim to an interactive wsh prompt (host wrapper)
```

- **Board config** — `sim:wanted`, a native defconfig in the NuttX fork. The engine runs as the NuttX init task via `wanted_sim_main`, which `chdir`s to `/data` (so `./registry` resolves against the sim's hostfs) and powers the board off cleanly when `wanted_main` returns.
- **Console** — raw `write(1/2)` for output.
- **Stop mechanism** — cooperative: `PlatformWappStop` sets the WAMR terminate flag and sends `SIGUSR2` to the worker so a wapp blocked in a host call is interrupted and checks the flag on return. A per-worker `interrupted` flag bridges `clock_nanosleep`'s success-on-signal quirk (Linux reports `EINTR` directly and needs no such bridge).
- **Submodules** — `third_party/nuttx` and `third_party/nuttx-apps` are shallow submodules pinned to the `wanted` branch of the mekops forks; `just nuttx-deps` initialises them (idempotent) and must run once before `nuttx-build`.

**Differences from Linux.** The `sim:wanted` board routes sockets to the host through usrsock (`CONFIG_SIM_NETUSRSOCK`), so `/net` sockets reach the host network; TLS comes from the shared raw-mbedTLS layer (`CONFIG_SYSTEM_WANTED_TLS` selects `apps/crypto/mbedtls` — same no-CA-bundle caveat as ESP-IDF: encrypted but unauthenticated). Neither target's cooperative stop can pre-empt a bare native call that never checks `EINTR`.

## Hardware targets

Three hardware target families live in the tree. Two are **NuttX** targets that share `platform/nuttx/` with the simulator — only the board config, the registry backend, and the entry point differ — and the third is a native **ESP-IDF** port under `platform/esp-idf/`.

### RP2350 (NuttX) — reference embedded target

The reference constrained target, and the one the control-plane story is proven on.

- **Core / boards** — ARM Cortex-M33 (RP2350). Two boards carry WANTED configs in the pinned NuttX fork: `adafruit-feather-rp2350:wanted` (the `wsh` debug supervisor) and `pimoroni-pico-2-plus-w:sheriff` (the Sheriff control-plane agent, with the onboard CYW43439 radio). A RISC-V/Hazard3 build of the silicon runs (PSRAM + `ostest` clean) but a WANTED board config for it is a deprioritized stretch.

  ```bash
  make rp2350 RP2350_CONFIG=pimoroni-pico-2-plus-w:sheriff PROFILE=small
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

A native ESP-IDF port (`platform/esp-idf/`, `app_main`) — not NuttX — targeting the ESP32-S3 (e.g. S3R8, octal PSRAM), 8 MB flash. Built with `idf.py` from `platform/esp-idf/project/`, so it is not wired into this Makefile's `esp32` host targets (those are the *classic* ESP32 NuttX build below).

- **Threads / stop** — FreeRTOS via the ESP-IDF pthread wrapper; cooperative stop (the WAMR terminate flag aborts the in-flight call).
- **Registry / PSRAM** — flash-backed LittleFS registry (`registry_flash.c`); octal PSRAM via `extram.c`. Built with the `psram-s3` profile (`MAX_WAPPS=20`); measured on an S3 with 8 MB PSRAM: the supervisor plus 19 concurrent user wapps fit, the 20th `start` is rejected cleanly with `-ENOSPC`.
- **OTA** — A/B firmware update through `esp_ota_ops` (`ota.c`), with a pending-verify / rollback seam.
- **Secure sockets** — raw mbedTLS with ESP32-S3 hardware AES/SHA/ECC acceleration. No CA bundle is provisioned (`MBEDTLS_SSL_VERIFY_NONE`), so `tcps://` here is encrypted but **unauthenticated** — a demo transport, not production TLS.
- **Crypto** — SHA-256 is hardware-backed, but **Ed25519 verify is not yet ported** (still the dummy backend, `platform/dummy/dummy-crypto.c`). So an ESP32-S3 control-plane demo reconciles with signature verification stubbed — the genuine Ed25519 path is the RP2350's.

### Classic ESP32 (Waveshare ESP32 One, NuttX)

Xtensa NuttX firmware via the `esp32` / `esp32-flash` host targets (board `esp32-devkitc:wanted`). Bring-up landed in engine 0.8.0: a ROMFS-boot supervisor, an in-RAM image cache, a PSRAM heap, a registry, and the `gpio` / `wifi` drivers. Distinct from the ESP32-S3 ESP-IDF port above (same vendor, different silicon and OS).

```bash
make esp32          # cross-build -> third_party/nuttx/nuttx.bin (xtensa toolchain image)
make esp32-flash    # esptool over ESP32_PORT (default /dev/ttyUSB0)
```

## Resource limits and build profiles

The engine's static memory envelope is set at build time. Every engine-wide limit lives in one header, `src/include/wanted-config.h`, each `#ifndef`-guarded so the build system overrides it without editing source:

| Constant | Default | Sizes |
|---|---|---|
| `MAX_WAPPS` | 3 | concurrent wapp instances (and, via `LOG_SLOTS`, the per-wapp log rings) |
| `WASM_STACK_SIZE` | 8192 | per-instance operand (interpreter) stack |
| `WASM_HEAP_SIZE` | 8192 | per-instance app heap |
| `WASM_MAX_MEMORY_PAGES` | 1 | per-instance linear-memory ceiling, in 64 KiB pages (`0` = uncapped) |
| `MAX_PATH_LEN` | 256 | VFS path buffers |

Driver-private limits (e.g. the 9P open-file table, the socket address buffer) stay local to their driver and are not part of this surface.

### A wapp's memory

Three engine-controlled regions are passed to WAMR per instance:

- **Operand stack** (`WASM_STACK_SIZE`) — the interpreter's evaluation stack, in host memory, **outside** linear memory. Distinct from the wapp's C aux stack, which lives inside linear memory and is fixed by the wapp's own linker (`wasm-ld -z stack-size`).
- **App heap** (`WASM_HEAP_SIZE`) — a host-managed heap for `wasm_runtime_module_malloc`, **outside** linear memory. WAMR disables it when the module exports its own `malloc`/`free`, so a WASI wapp (which allocates from its libc heap at the top of linear memory) usually does not use it.
- **Linear memory** (`WASM_MAX_MEMORY_PAGES`) — the memory the wapp actually addresses: its data, C aux stack, and libc heap. Enforced two ways: WAMR bounds `memory.grow` to the cap at runtime, and the engine refuses at load any image whose declared *initial* memory exceeds it (otherwise WAMR clamps the cap up to the module's initial, letting a large initial bypass the runtime bound). `0` disables both. (A module containing no `memory.grow` is collapsed by WAMR to a single fixed page - `WAMR_BUILD_SHRUNK_MEMORY` flag is on by default.)

### Profiles

Per-capacity profiles ship as CMake cache fragments under `cmake/profiles/`:

| Profile | Target class | `MAX_WAPPS` | stack / heap | linear cap |
|---|---|---|---|---|
| `tiny` | no-PSRAM (ESP32-WROOM, ~180 KB internal RAM) | 2 | 4 KiB / 4 KiB | 1 page |
| `constrained` | ~512 KB RAM, PSRAM (ESP32-WROVER/NuttX) | 3 | 8 KiB / 8 KiB | 1 page |
| `psram-s3` | ESP32-S3 + octal PSRAM (ESP-IDF port) | 20 | 64 KiB / 0 (app heap off) | 16 pages |
| `small` | routers (128 MB–1 GB) | 16 | 64 KiB / 256 KiB | 16 pages |
| `big` | Linux / cloud | 64 | 128 KiB / 1 MiB | uncapped |

Select one — the unset default is the `constrained` header values:

```bash
PROFILE=small just build         # Linux engine + CLI
PROFILE=small just nuttx-build   # NuttX sim
cmake -C cmake/profiles/small.cmake -S . -B build   # direct cmake
```

On Linux the fragment seeds the CMake cache; for NuttX the same values are forwarded as `-D` overrides into the engine app build. A command-line `-DMAX_WAPPS=…` overrides a profile.

### Measuring the footprint

- `just sizes` reports each profile's per-wapp and worst-case memory for both the host (LP64) and 32-bit embedded (ILP32) ABIs, measured from the real engine structs, but it's just approximate value (e.g. wamr overhead is arbitrary worst case value), it doesn't actually measure the whole runtime overhead on specific hardware, using specifc compiler, just the struct sizes.
- `just memcap` is a negative test that verifies the `WASM_MAX_MEMORY_PAGES` cap actually bounds a wapp's `memory.grow`.

## Out-of-tree drivers

A driver written for one deployment — a router's config store, a site-specific sensor bus — does not have to live in this repo to be linked into a target. Point the build at a source tree that supplies `ExtraDriverTable()` and its entries join the launch-config driver names:

```bash
cmake -GNinja -DWANTED_EXTRA_DRIVERS_DIR=/path/to/tree ..
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
