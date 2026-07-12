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
| Wapp lifecycle | `PlatformWappLoad` / `Unload` / `Start` / `Stop` / `Loop` / `GetState` |
| Registry backend | `PlatformRegistryRead` / `Write` / `Remove` / `WappLoad` |
| Filesystem | `PlatformOpenStateDir`, `PlatformFsRename`, `PlatformFsMkdir` |
| Network | `PlatformNetOpen` / `Connect` / `Recv` / `Send` / `Accept` / `Shutdown` / `Close` / `Free` |
| Clock | `PlatformClockGetRes` / `GetTime` / `NanoSleep` |
| Memory | `PlatformMemoryStats` |
| Concurrency | `PlatformMutexNew` / `Lock` / `Unlock` / `Free` |
| Crypto | `PlatformEd25519Verify` — the one seam symbol allowed to report an absent backend (`-ENOSYS`); the `/dev/ed25519` verdict read surfaces it to the wapp. Real on Linux (OpenSSL) and NuttX (vendored `orlp/ed25519`); the ESP-IDF port still uses the dummy backend |
| Power / process | `PlatformSetProcessArgs`, `PlatformRequestShutdown`, `PlatformRequestReboot` |

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

A target layer (`platform/linux/`, `platform/nuttx/`) links those sources and implements only what genuinely differs between the two:

| Concern | Linux | NuttX |
|---------|-------|-------|
| Threads + stop | pthreads; async `pthread_cancel` | tasks; cooperative `SIGUSR2` + WAMR terminate flag |
| Sleep (`PlatformNanoSleep`) | `api/clock-sleep.c` | `api/clock-sleep.c` (signal-EINTR quirk) |
| Secure sockets | OpenSSL (`api/ssocket.c`) | none yet |
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
- **Stop mechanism** — `pthread_cancel(ASYNCHRONOUS)` interrupts a blocked syscall immediately, so a wapp stuck in a host call is reaped promptly.
- **Registry** — a host-filesystem directory (`./registry/`) scanned for `<name>@<version>.wapp` images.
- **TLS** — OpenSSL-backed secure sockets (`T`/`U` socket options).
- **Memory stats** — `mallinfo2`.

CMake options of note: `WANTED_PLATFORM` (the platform layer), `WANTED_SUPERVISOR_IMAGE_PATH` (compile-time supervisor image), `SECURE_SOCKETS` (TLS).

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
- **Stop mechanism** — cooperative: `PlatformWappStop` sets the WAMR terminate flag and sends `SIGUSR2` to the worker so a wapp blocked in a host call is interrupted and checks the flag on return. A per-worker `interrupted` flag bridges `clock_nanosleep`'s success-on-signal quirk.
- **Submodules** — `third_party/nuttx` and `third_party/nuttx-apps` are shallow submodules pinned to the `wanted` branch of the mekops forks; `just nuttx-deps` initialises them (idempotent) and must run once before `nuttx-build`.

**Differences from Linux.** No TLS yet. The `sim:wanted` board is built without a network stack (`CONFIG_NET` off), so `socket()` fails and `/net` sockets are unavailable on the sim — socket paths are exercised on Linux only, and the selftest skips its `/net` socket check on the sim. The cooperative stop cannot pre-empt a bare native call that never checks `EINTR`, where Linux's async cancel can.

## Hardware targets

Three real-hardware targets live in the tree. Two are **NuttX** boards that share `platform/nuttx/` with the simulator — only the board config, the registry backend, and the entry point differ — and the third is a native **ESP-IDF** port under `platform/esp-idf/`.

### RP2350 (Adafruit Feather RP2350, NuttX) — reference embedded target

The reference constrained target, and the one the control-plane story is proven on.

- **Core / board** — ARM Cortex-M33; board `adafruit-feather-rp2350` with two WANTED configs: `:wanted` (the `wsh` debug supervisor) and `:sheriff` (the Sheriff control-plane agent). A RISC-V/Hazard3 build of the silicon runs (PSRAM + `ostest` clean) but a WANTED board config for it is a deprioritized stretch.

  ```bash
  make rp2350 RP2350_CONFIG=adafruit-feather-rp2350:sheriff PROFILE=small
  make rp2350-flash-swd    # flash over SWD via a Raspberry Pi Debug Probe (no BOOTSEL)
  ```

- **Registry** — a LittleFS volume on a reserved region of the internal QSPI flash (the flash-MTD backend), written through the RP2350 ROM flash routines. Full wapp lifecycle (`create → start → running → stop → exited`) is hardware-verified.
- **PSRAM** — 8 MB external PSRAM on QMI CS1 (GPIO8), merged with internal SRAM into one ~8.5 MiB heap (`RP23XX_PSRAM_HEAP_SINGLE`). The large engine buffers (WAMR linear memory, the wapp image cache) live in PSRAM while worker stacks stay in scarce internal SRAM. Measured ceiling: ~38 concurrent wapps. Because flash program/erase and PSRAM share the QMI hardware, the internal-flash MTD driver cleans the XIP cache and saves/restores the CS1 registers around every flash op — without which a flash write silently corrupts PSRAM.
- **Crypto** — a **real Ed25519 verify**: NuttX's vendored mbedTLS has no Ed25519, so the port vendors `orlp/ed25519` (verify-only) behind `PlatformEd25519Verify`. The only target with a genuine (non-stub) Ed25519 backend on embedded silicon.
- **No TLS, no WiFi** — NuttX has no secure-socket backend here, and the CYW43439 radio is unported to rp23xx, so the control plane runs over a wired link (below).
- **Console + flashing** — with the `:sheriff` config the console moves to UART0 (read it over the Debug Probe's UART bridge), which frees the native USB-CDC for the control plane. Flash over SWD with `make rp2350-flash-swd` (no button dance) or over USB in BOOTSEL with `make rp2350-flash`; `make rp2350-reset` resets a running board over SWD.
- **Control plane over USB-CDC** — with no network stack, Sheriff reaches a host Deputy over the Feather's native USB-CDC using the engine's `serial://` socket scheme (a device path in place of `host:port`). The full reconcile loop runs on real hardware: State Report uplink → Ed25519-verified signed Desired State → wapp `RUNNING`, and a wrongly-signed Desired State is rejected. This is the ecosystem's first genuine (not demo-stubbed) signed-workload verification on embedded hardware.
- **Secure boot** — validated entirely offline via `picotool seal --sign` (`make rp2350-sign`); the one-way OTP `SECURE_BOOT_ENABLE` fuse is deliberately never burned.

### ESP32-S3 (ESP-IDF)

A native ESP-IDF port (`platform/esp-idf/`, `app_main`) — not NuttX — targeting the ESP32-S3 (e.g. S3R8, octal PSRAM), 8 MB flash. Built with `idf.py` from `platform/esp-idf/project/`, so it is not wired into this Makefile's `esp32` host targets (those are the *classic* ESP32 NuttX build below).

- **Threads / stop** — FreeRTOS via the ESP-IDF pthread wrapper; cooperative stop (the WAMR terminate flag aborts the in-flight call).
- **Registry / PSRAM** — flash-backed LittleFS registry (`registry_flash.c`); octal PSRAM via `extram.c`.
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

## Porting to a new platform

1. Create `platform/<name>/`. If the target is POSIX-like, link `platform/posix/` and implement only the deltas (thread/stop model, memory stats, secure sockets if any, registry-backend glue, entry point); otherwise implement every `Platform*` symbol from `platform.h` yourself, with `platform/dummy/` as the model for a from-scratch port. Either way, no stubs.
2. Provide a stop mechanism that **interrupts a blocked host syscall**, not just a cooperative flag.
3. Implement a registry backend (filesystem, flash blob, or in-memory) behind `PlatformRegistry*`.
4. Wire the build (`WANTED_PLATFORM=<name>`) and a board/app entry point.

The NuttX port is the reference for a constrained target. Its findings carry over: keep `wamrData_t` opaque and reach it through the `WantedWappTerminate` accessor; avoid VLAs; avoid `scandir` and GNU extensions (use `opendir`/`readdir`/`qsort`); confirm `CONFIG_INTERPRETERS_WAMR_THREAD_MGR` so `wasm_runtime_terminate` takes effect, and that thread cleanup does not rely on `pthread_cleanup_push`.

## See also

- [Architecture](architecture.md) — where the platform seam sits in the system.
- [Testing Guide](testing-guide.md) — the same suites run on Linux and the NuttX sim.
