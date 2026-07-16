<!-- SPDX-License-Identifier: Apache-2.0 -->
# `src/` — engine core

Platform-independent engine. Everything here must stay portable — **no OS
syscalls directly**; all platform-specific operations go through the
`platform/include/` contract (see [`platform/README.md`](../platform/README.md)).
The one public integration symbol, `WantedStart()`, lives in
[`include/wanted.h`](../include/wanted.h); `src/include/wanted-api.h` is the
primary internal API surface.

## Where to start reading

`wanted.c` is the spine — engine init, supervisor loading, and the wapp
lifecycle (`WantedWappRun` → `WantedWappStop`/`WantedWappTerminate`). From there
follow a WASI call outward through `wasi/` → `vfs/`. The end-to-end data flow
(WASI syscall → VFS resolve → driver dispatch) and the lifecycle sequence are
diagrammed in [`docs/architecture.md`](../docs/architecture.md).

## Module map

| Path | Owns |
|------|------|
| `wanted.c` | Engine initialization, supervisor loading, wapp lifecycle orchestration. |
| `wanted_malloc.c` | Allocation wrapper — the single place to swap allocators. |
| `wanted_wasm_api.c` | WAMR `NativeSymbol` registration for the `wanted` host module. |
| `wanted-vfs-api.c` | The VFS calls exposed to the WASM/WASI layer. |
| `default_supervisor_cfg.json.h` | Compiled-in default config (generated header). |
| `include/` | Internal headers; `wanted-api.h` is the primary internal surface, `wanted-config.h` the compile-time resource limits. |

### `vfs/` — the VFS router

Routes WASI syscalls to a driver by prefix-matching the mount table.

| Path | Owns |
|------|------|
| `vfs.c` | Core router: `open`/`openat`/`close`/`read`/`write`/`stat`/`seek`/`readdir`. |
| `vfs-tarfs.c` | OCI layer merging — shadowing + `.wh.` whiteouts, O(log N) zero-copy lookup. |
| `vfs-devfs.c` | `/dev/` dispatcher. |
| `vfs-netfs.c` | `/net/` dispatcher (TCP/UDP sockets). |
| `vfs-virtual.c` | Platform-independent virtual drivers (`/dev/null`, `/dev/config`, …). |
| `vfs-socket.c` | Raw socket I/O (plain + TLS via the platform net seam). |
| `vfs-9p.c` | 9P2000 protocol driver (host communication). |
| `vfs-log*.c` | Per-wapp log store and its read-only mount view. |
| `vfs-wanted*.c` | Supervisor control-plane drivers — the root `ctl` and the per-wapp `wapps/` namespace (`vfs-wanted-wapps.c`), config, registry. |

Adding a driver: implement it in `vfs/`, register it in `vfs-devfs.c` or
`vfs-netfs.c`, and add its header to `src/include/`. A new driver or core
behaviour needs a matching `test/test-*.c` group.

### `wasi/` — WASI-to-VFS bridge

`wasi-vfs.c` translates WASI syscall numbers into VFS router calls. This is the
**only** translation layer — do not add platform logic here, and never add a
direct syscall from a WASM host function (it bypasses the VFS isolation
boundary).

## Ground rules

- **C99, no compiler/GNU extensions, no VLAs.** See `CONTRIBUTING.md`.
- **Compile-time limits** (`MAX_WAPPS`, stack/heap sizes, `MAX_PATH_LEN`, …) live
  in `include/wanted-config.h`; changing one resizes static structures — audit
  every array dimensioned by it.
- **No dynamic allocation in wapp context after init** on constrained targets.
