---
title: "WANTED Engine"
date: 2026-06-08T17:00:00+01:00
description: "A WebAssembly nanocontainer runtime that isolates and runs multiple apps as threads, with all I/O mediated through a cloud-native VFS router."
---

WANTED is a WebAssembly nanocontainer runtime for embedded and IoT devices. It loads, isolates, and runs multiple WebAssembly applications — **wapps** — as independent threads inside a single process. Each wapp runs in its own WASM linear memory and reaches the outside world only through a virtual filesystem: hardware, network sockets, IPC channels, and the runtime control plane are all paths in a per-wapp namespace. There is no ambient host access and no shared memory between wapps.

The engine targets constrained hardware first. It runs on Linux today and on NuttX (host simulator), uses the WAMR interpreter so no per-target codegen is needed, and packages wapps as OCI-compatible layered TAR images for delta updates and offline distribution.

## Key properties

- **Offline-first.** Wapps are self-contained OCI TAR images; no registry connectivity is required to install or run them.
- **Capability isolation via the VFS.** A wapp can only touch what its launch config mounts into its namespace — `/dev/`, `/net/`, `/proc/`, and the read-only TarFS root. No grant, no access.
- **Standard WASI ABI.** Wapps are ordinary `wasm32-wasi` binaries; the host interface is WASI `snapshot_preview1` plus a small VFS-mediated control surface.
- **OCI-compatible packaging.** Layered ustar TARs with shadowing and whiteout semantics, indexed for O(log N) lookup and zero-copy boot.
- **Portable across targets.** A thin `Platform*` seam abstracts threads, sockets, files, clock, and memory stats; Linux and NuttX are the two production implementations.

## Feature matrix

| Area | Capability |
|------|-----------|
| Runtime | WAMR 2.4.4, fast interpreter; thread-per-wapp execution; WASI `snapshot_preview1` bridge |
| VFS drivers | TarFS root, DevFS (`/dev/`), NetFS (`/net/`), ProcFS (`/proc/`), named pipes, sockets (TCP/UDP/TLS), 9P client, log console |
| Packaging | OCI-compatible layered ustar TAR; up to 4 layers; whiteout deletion; PAX/GNU long names |
| Supervisor | Privileged wapp loaded at boot; variants for production control (sheriff), interactive debug (wsh), and in-WASM self-test |
| Control plane | `/dev/wanted/*` — install, start/stop, observe state, read logs, and drive engine power state |
| Platforms | Linux (primary); NuttX simulator (CI-gated); NuttX on ESP32 hardware planned |

## Documentation

{{< children >}}

## Source

The engine is developed in the open at [gitlab.com/wanted-project/wanted-engine](https://gitlab.com/wanted-project/wanted-engine).
