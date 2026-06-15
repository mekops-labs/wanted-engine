---
title: "Platform Guide"
date: 2026-06-08T17:30:00+01:00
weight: 70
toc: true
description: "Building for and porting to each target: the Platform seam, Linux, the NuttX simulator, and what a new port must implement."
---

The engine core is platform-agnostic; everything OS-specific lives behind the `Platform*` seam. This guide covers the seam, the two production targets, and the checklist for a new port.

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
| Power / process | `PlatformSetProcessArgs`, `PlatformRequestShutdown`, `PlatformRequestReboot` |

The invariants every platform must honour: a wapp runs on its own thread; `PlatformWappStop` must interrupt a wapp that is blocked in a host syscall (not merely set a flag and wait); `PlatformWappLoop` blocks until an explicit shutdown/reboot request and respawns a supervisor that exits on its own; memory stats report heap usage; the registry resolves a wapp image by name.

All targets build inside the standardized container — the host only needs a container runtime. `make help` lists the targets; append `RUNNER=docker` to use Docker.

## Linux

The primary target.

```bash
make build        # engine + CLI with the production sheriff supervisor
make wsh          # engine with the wsh debug supervisor
```

- **Threads** — pthreads.
- **Stop mechanism** — `pthread_cancel(ASYNCHRONOUS)` interrupts a blocked syscall immediately, so a wapp stuck in a host call is reaped promptly.
- **Registry** — a host-filesystem directory (`./registry/`) scanned for `<name>:<version>.wapp` images.
- **TLS** — OpenSSL-backed secure sockets (`T`/`U` socket options).
- **Memory stats** — `mallinfo2`.

CMake options of note: `WANTED_PLATFORM` (the platform layer), `WANTED_SUPERVISOR_IMAGE_PATH` (compile-time supervisor image), `SECURE_SOCKETS` (TLS).

## NuttX simulator

A first-class, CI-gated target: the full engine running as a NuttX application on the host-stack simulator. The `platform/nuttx/` layer is complete — every `Platform*` symbol has a working body, with no remaining stubs.

```bash
make nuttx-deps      # init the nuttx + nuttx-apps submodules, link the app package
make nuttx-build     # configure + build the sim from a clean tree
make nuttx-selftest  # run the in-WASM selftest suite on the sim
make nuttx-shell     # boot the sim to an interactive wsh prompt
```

- **Board config** — `sim:wanted`, a native defconfig in the NuttX fork. The engine runs as the NuttX init task via `wanted_sim_main`, which `chdir`s to `/data` (so `./registry` resolves against the sim's hostfs) and powers the board off cleanly when `wanted_main` returns.
- **Console** — raw `write(1/2)` for output.
- **Stop mechanism** — cooperative: `PlatformWappStop` sets the WAMR terminate flag and sends `SIGUSR2` to the worker so a wapp blocked in a host call is interrupted and checks the flag on return. A per-worker `interrupted` flag bridges `clock_nanosleep`'s success-on-signal quirk.
- **Submodules** — `third_party/nuttx` and `third_party/nuttx-apps` are shallow submodules pinned to the `wanted` branch of the mekops forks; `make nuttx-deps` initialises them (idempotent) and must run once before `nuttx-build`.

**Differences from Linux.** No TLS yet. The `sim:wanted` board is built without a network stack (`CONFIG_NET` off), so `socket()` fails and `/net` sockets are unavailable on the sim — socket paths are exercised on Linux only, and the selftest skips its `/net` socket check on the sim. The cooperative stop cannot pre-empt a bare native call that never checks `EINTR`, where Linux's async cancel can.

## NuttX on real HW (upcoming)

The planned architecture: wapp images loaded from XIP flash, a LittleFS-backed registry slot table, and mbedTLS for secure sockets. The simulator port is the staging ground — the `Platform*` bodies are largely shared; the flash registry backend and TLS are the remaining hardware-specific pieces.

## Porting to a new platform

1. Create `platform/<name>/` and implement every `Platform*` symbol from `platform.h` — no stubs.
2. Provide a stop mechanism that **interrupts a blocked host syscall**, not just a cooperative flag.
3. Implement a registry backend (filesystem, flash blob, or in-memory) behind `PlatformRegistry*`.
4. Wire the build (`WANTED_PLATFORM=<name>`) and a board/app entry point.

The NuttX port is the reference for a constrained target. Its findings carry over: keep `wamrData_t` opaque and reach it through the `WantedWappTerminate` accessor; avoid VLAs; avoid `scandir` and GNU extensions (use `opendir`/`readdir`/`qsort`); confirm `CONFIG_INTERPRETERS_WAMR_THREAD_MGR` so `wasm_runtime_terminate` takes effect, and that thread cleanup does not rely on `pthread_cleanup_push`.

## See also

- [Architecture](architecture.md) — where the platform seam sits in the system.
- [Testing Guide](testing-guide.md) — the same suites run on Linux and the NuttX sim.
