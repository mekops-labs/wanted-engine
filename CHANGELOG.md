Changelog
=========

0.5.0 (2026-05-19)
------------------

### WebAssembly runtime — WAMR migration (breaking internal change)

- Replaced `wasm3` with **WAMR 2.4.4** (BytecodeAlliance) in classic interpreted mode (`WAMR_BUILD_INTERP=1`, `WAMR_BUILD_AOT=0`, `WAMR_BUILD_LIBC_WASI=0`).
- WAMR vendored as a git submodule at `vendor/wamr` pinned to tag `WAMR-2.4.4`.
- `platform/esp-idf` removed; `platform/nuttx/` stub added as the target embedded platform skeleton.
- `src/CMakeLists.txt` rewritten: `runtime_lib.cmake` consumed to populate `WAMR_RUNTIME_LIB_SOURCE`; `vmlib` static target created explicitly.
- Full wapp lifecycle ported: `wasm_runtime_full_init` → `wasm_runtime_load` → `wasm_runtime_instantiate` → `wasm_runtime_create_exec_env` → `wasm_runtime_call_wasm` → teardown via `wasm_runtime_destroy_exec_env` / `wasm_runtime_deinstantiate` / `wasm_runtime_unload`.
- Native host functions registered via `NativeSymbol` arrays and `wasm_runtime_register_natives` (replaces `m3_LinkRawFunctionEx`).
- Runtime init extracted into `EnsureWamrInit()` — idempotent, called from both `WantedStart` and `WantedWappRun` so unit tests that bypass `WantedStart` initialise correctly.
- Per-thread signal environment (`wasm_runtime_init_thread_env()`) wired into every worker entry point — required by WAMR's hardware bounds-check trap handler.
- `wasm_runtime_load` mutates its input buffer (LEB128 patching); engine keeps a heap-owned writable copy (`wamrData_t.wasm_bytes`) freed after `wasm_runtime_unload`.
- `proc_exit` modelled as `wasm_runtime_set_exception(inst, "proc_exit")`; `wanted.c` distinguishes clean exit from runtime errors via `strcmp`.
- `lookup_function` API drop in WAMR 2.x: third (signature) argument removed.

### WASI bridge rewrite

- `wasi-vfs.c` fully rewritten to WAMR `NativeSymbol` conventions (~700 lines).
- All 24 WASI snapshot-preview1 handlers ported; shared `vaddr()` helper validates wasm linear-memory addresses via `wasm_runtime_validate_app_addr` and translates them with `wasm_runtime_addr_app_to_native`.
- Local `wasi_types.h` added (`src/wasi/`) — vendors the snapshot-preview1 type definitions and constants (`__wasi_fdstat_t`, `__wasi_filestat_t`, `__wasi_subscription_t`, errno codes, rights flags) because `WAMR_BUILD_LIBC_WASI=0` excludes WAMR's own WASI headers.
- Both `wasi_unstable` and `wasi_snapshot_preview1` namespaces registered from the same `NativeSymbol` table.

### TarFS — OCI-layered filesystem (new subsystem)

- `wapp_t` refactored to carry an OCI layer stack instead of a single blob pointer.
- OCI layered TAR parser added with PSRAM-friendly index (`vfs/tarfs`): O(log N) path lookup, boot-time pre-fetch, whiteout (`.wh.`) semantics for layer shadowing.
- PAX extended headers and GNU long-name entries supported — required for OCI-exported images with paths > 100 bytes.
- Phase 5 file and directory operations implemented (`VfsOpen`, `VfsStat`, `VfsReaddir` via TarFS).
- End-to-end `VfsOpen` coverage added (Phase 7 test).
- Wapp boot wired through TarFS entrypoints; per-wapp `tarfs_ctx_t` lifetime owned by the engine.
- Legacy `fildes`/romfs dispatch path retired.

### Supervisor — TAR image loading

- Supervisor loaded from a ustar TAR archive at runtime (`PlatformWappLoad`) instead of being compiled into the binary.
- Default supervisor variant renamed to **sheriff**; `wsh` debug shell remains as an alternative.
- `supervisor.imagePath` JSON config field added — runtime override requires no recompile; resolved before the compiled-in `WANTED_SUPERVISOR_IMAGE_PATH` CMake default.
- Fix: sheriff supervisor boot failure caused by stale config/path stack state on startup.

### VFS — router, DevFS, NetFS, and fixes

- Stateless prefix router added; `DevFs` and `NetFs` collapsed onto direct registration (no shim layer).
- Typed FD table scaffolded on `vfs_ctx_t`; `vfs_fd_t.path` carries the resolved path for relative operations across namespace boundaries.
- `PathNormalize` replaced with `cwk_path_normalize` (cwalk library) — handles `..`, `.`, double-slash correctly.
- Mount-table VFS router replaces hardcoded prefix chain; `PathNormalize` + `VfsResolvePath` for namespace-boundary resolution.
- ProcFS (`/proc`) — read-only flat namespace; privileged-entry access control; `/proc/wapps`, `/proc/memory`.
- Named pipes (`/dev/pipe/<name>`) — 4096-byte ring buffer; up to 8 concurrent pipes; write→close→read lifecycle.
- Stdio stub devices (`/dev/null`, `/dev/stdin`, `/dev/stdout`, `/dev/stderr`) always registered.
- `system.privileged` config flag; `VfsSetPrivileged()` public API.
- Fix: error propagation from `route_open` (M1/M3), `DevFs_Open`, and `NetFs_Open` — driver errno codes now surface to callers.
- Fix: `devfs` readdir returning stale entries.

### Manifest

- `requirements` field added to `manifest.json` schema — parsed by `WantedWappParseManifestBytes` and stored in `wapp_t`; Sheriff validates capabilities before issuing start actions.
- `requirements[]` added to both `sheriff` and `wsh` supervisor manifests.

### wsh

- v0.4.1: fix `ls` path resolution — `lstat` was called with a relative path that broke directory listings outside the working directory.

### CLI and tooling

- Executable target renamed from `wanted_cmd` to `wanted-cli`.
- Devcontainer configuration added for VS Code / JetBrains remote development.
- Source tree reformatted to LLVM coding style; `.clang-format` and IDE helper files added.
- Smoke test suite added (`test/smoke.sh`, wsh-based, 16 tests).

### Test baseline

- 20/20 ctest, 16/16 smoke tests pass on the `wamr` branch.

0.4.0
-----

- added initial support for ESP32 platform (sockets not yet supported)
- reduced stack usage

0.3.0
-----

- added 9P2000 client driver (for external driver plugins) - still WIP (error handling!), but usable
- prepared docker environment for building and CI

0.2.1
-----

- first working version
- can run multiple wapps

0.2.0
-----

- still not usable
- most of the vfs, wasm, wasi implementation is done
