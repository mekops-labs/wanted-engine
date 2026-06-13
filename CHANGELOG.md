Changelog
=========

Unreleased
----------

### Launch config — `platform` bind mounts (host-path mapping + read-only)

- A `platform` `mounts[]` entry is now a full bind mount. Its `options` string carries `src=<hostpath>` to back the wapp-visible `path` with an arbitrary host directory (defaulting to `path`, so existing single-path mounts are unchanged) and `ro`/`rw` to set the access mode (defaulting to `rw`).
- A `ro` mount is enforced in the engine: the platform VFS driver rejects writes, directory creation, and renames with `-EROFS`, and the host directory is opened without write intent. A read-only mount requires its host directory to already exist (it is never created); a missing one fails the launch.
- Malformed `platform` options — a relative or empty `src`, or an unrecognised token — are rejected at install.

### Launch config — drivers / mounts / sockets split

- The flat `drivers[]` array (every entry a `name` **and** a mount `path`) is replaced by three purpose-specific sections, each addressed the way its resource actually is: `drivers[]` are device singletons mounted at `/dev/<name>` (no `path`); `mounts[]` are file/backend drivers bound at an arbitrary absolute `path` outside `/dev` and `/net`; `sockets[]` are connections created at `/net/<name>`, the transport carried in `address`. Install-time validation is loud per section — a `path` on a `drivers[]`/`sockets[]` entry, or a `mounts[]` path under a reserved namespace (`/dev`, `/net`, `/proc`), is rejected.
- `/dev/stdin`, `/dev/stdout`, `/dev/stderr` now alias the wapp's own console streams (WASI fd 0/1/2): opening the `/dev` path reaches the same backing as the matching fd — the `platform` console, the `log` ring, or `/dev/null` — instead of the previous discard/EOF stubs.
- `preopens[]` is removed; a host directory is now a `mounts[]` entry with the `platform` backend (`{ "name": "platform", "path": "/var/lib/app" }`), which the engine binds as a native WASI preopen at that path.
- The VFS router gains a general single-driver mount: a file/backend driver (config-map, `platform`, 9P) can be bound at any absolute path such as `/etc/config`, reachable outside the device and network namespaces. A deep mount surfaces a synthetic parent directory in the root listing.
- Socket addresses use a URL form `<scheme>://<host>:<port>` — `tcp`/`udp` (plain), `tcps`/`udps` (TLS/DTLS) — replacing the `t|u|T|U host port` string. The vestigial `bus` transport (no platform backing; every open returned `-ECONNABORTED`) is removed. The `9p` driver's address adopts the same URL form (`tcp://host:port` / `udp://host:port`), replacing the Plan 9 `tcp!host!port` dial string.
- The config-exposed `virt` driver is removed (it could not be usefully driven from config); the virtual-namespace primitive remains internal.

### Licensing

- Relicensed from MIT to Apache License 2.0. Added `NOTICE` (project copyright plus attribution for bundled third-party components: WAMR, cwalk, tiny-json, c9, Unity, NuttX) and a minimal `CONTRIBUTING.md`. All first-party `.c`/`.h` files now carry an `SPDX-License-Identifier: Apache-2.0` header.

### Images — manifest removed, image identity from the registry, multi-instance

- **`manifest.json` is gone.** A wapp image is `app.wasm` (+ any TarFS payload); the loader no longer requires or parses an in-image manifest. Image identity (name + version) comes from the registry filename `<name>:<version>-<package>.wapp` — `PlatformRegistryWappLoad` stamps `image` and `version` onto the wapp from the resolved registry entry and never overwrites the instance name. `requirements[]` (parsed but never consumed) is dropped; the capability-declaration home is deferred.
- **Instance identity is decoupled from image identity.** `create <name>` reserves an instance; the image it runs is resolved at start as an explicit `start <image>` argument → the launch config's `image` field → the instance name. One image can therefore run as N instances. The image an instance runs is recorded on the new `wapps/<name>/image` control node.
- **Install by ref.** Streaming an image into the registry now names the stored file by the write path (`reg/<name>:<version>`) instead of parsing a manifest at finalize; `PlatformRegistryWrite` takes the target ref. Reading a registry entry returns a small synthesized `{name,version,size}` descriptor — no image load.
- Supervisor TARs (`sheriff`/`wsh`/`selftest`) and all sample wapps ship `app.wasm` only; the supervisor's identity is its compiled-in name. Docs, selftest staging, and the malformed-image battery (now: no-entrypoint, invalid WASM, truncated TAR) updated accordingly.

### Control plane — env/argv, first-start lifecycle, exit codes, and slot release

- Wapps receive **environment variables and command-line arguments** via standard WASI. The launch config gains `args[]` (→ `argv[1..]`; `argv[0]` is always the wapp name) and `envs[]` (POSIX `KEY=VALUE` → `environ`); the engine fills WAMR's `argv`/`envp` and implements the previously-stubbed `environ_sizes_get`/`environ_get`.
- **Explicit first-start lifecycle.** A wapp is launched in three deliberate steps: `create <name>` on the root `ctl` reserves the namespace (state `created`), a write to `wapps/<name>/config` buffers the launch config (→ `not_started`), and a bare `start` on `wapps/<name>/ctl` launches it. `create` is the sole entry point — opening any node of an unknown name returns `-ENOENT`, so a name cannot be probed or configured by guessing its path. The root `ctl` no longer launches wapps; `start`/`stop` are per-wapp only.
- **`delete <name>` root verb** releases a slot: it frees a `create` reservation and/or a terminal (`exited`/`failure`) platform slot (new `PlatformWappRelease` on linux/nuttx/dummy), so the name leaves `wapps/` and its nodes return `-ENOENT` again. A `running`/`starting` wapp is rejected with `-EBUSY` — stop it first; there is no implicit stop-then-delete.
- **Exit-code exposure.** `wapps/<name>/exit_code` reports the wapp's WASI exit code as plain text, authoritative only when `state == exited` (sentinel `-1` otherwise). A genuine trap now resolves to `state == failure` (previously a blanket success), giving a supervisor a three-way exit analysis: clean zero, clean non-zero, trap.
- `wsh` gains `create`, `delete`, and `set_config` builtins for the lifecycle; `start`/`stop` write the bare per-wapp verb.

### Engine — `/proc/wanted` introspection node

- Added a read-only `/proc/wanted` node exposing engine identity and compile-time resource ceilings as `key:\tvalue` lines: `platform`, `version`, `max_wapps`, `max_wapp_name`, `max_path`, `wasm_stack`, `wasm_heap`. Unprivileged — any wapp may read it to size itself to the host.
- `platform` comes from a new `PlatformName()` accessor in the platform API (`linux`/`nuttx`/`dummy`), keeping the platform boundary intact. `version` is the git-derived SemVer baked in via a `WANTED_VERSION` compile definition.

### Engine — default console backing

- A wapp whose launch config omits a stdio slot no longer fails to start. An unset `console.in`/`out`/`err` now resolves to a default backing — `stdin` to `null`, `stdout`/`stderr` to `log` — so a wapp launches without an explicit console and its output is captured to the per-wapp log ring buffer instead of being lost. A slot set explicitly still overrides its default.
- selftest covers the default and all-null console backings.

### NuttX platform port — (Linux sim, CI-gated)

- `platform/nuttx/` is fully implemented — every `Platform*` symbol has a working body with no `-ENOSYS` stubs remaining. Covers: pthreads wapp lifecycle, `opendir`/`readdir`/`qsort` registry (no scandir/VLA), NuttX VFS driver, `clock_gettime`/`clock_nanosleep`, `/dev/urandom` random, BSD sockets (TLS not supported), `wanted_main` built-in app entry point, pthread mutex.
- The NuttX sim (host-stack `sim:wanted` defconfig) is a first-class build target: `make nuttx-{deps,build,smoke,selftest,shell}`. NuttX + apps forks vendored as shallow submodules at `third_party/`.
- `nuttx-integration-tests` CI job builds the engine as a NuttX built-in and runs `smoke-engine` + selftest on the sim.
- Two NuttX-specific bugs fixed: `CONFIG_INTERPRETERS_WAMR_THREAD_MGR` required for `wasm_runtime_terminate` to have effect; `pthread_cleanup_push/pop` is a no-op without `CONFIG_PTHREAD_CLEANUP` — wapp reaping now calls `WA_threadEnd` directly.
- `wanted_sim_main` calls `boardctl(BOARDIOC_POWEROFF)` after `wanted_main` returns so the sim exits cleanly.

### In-WASM selftest suite

- Added a `selftest` supervisor variant (`wapps/selftest/`) that orchestrates the full test suite from inside WASM. Runs identically on Linux and the NuttX sim via `make selftest` / `make nuttx-selftest`; output is TAP.
- 29 test scenarios across VFS/namespace, inter-wapp IPC, concurrency/stop, and negative/robustness categories.
- Shell smoke scripts (`smoke.sh`, `smoke-multiwapp.sh`, `smoke-pipe.sh`, `smoke-driver.sh`) retired in favour of the in-WASM suite. `smoke-engine.sh` kept for production-supervisor sanity.

### Engine — log console driver

- Added a per-wapp log console driver (`src/vfs/vfs-log.c` + `src/log-store.c`): a ring-buffer store captures a wapp's stdout/stderr and exposes it at `/dev/wanted/wapps/<name>/log`. Wapps configured with `console:log` no longer share the platform console; the supervisor reads their output via the control plane. LRU eviction when all name slots are full.
- `debug_trace` now emits via a raw `write()` syscall so it reaches the console on every target.

### Engine — supervisor priority and interruptible stop

- Supervisor thread is created one priority step above worker threads on both Linux and NuttX; worker priorities are set explicitly to prevent inheritance.
- Added interruptible stop on NuttX: `PlatformWappStop` sends `SIGUSR2` to the worker after `WantedWappTerminate` so a wapp blocked in a host syscall is interrupted and the stop flag checked on return. A per-worker `interrupted` flag bridges `clock_nanosleep`'s success-on-signal quirk. Linux retains `pthread_cancel(ASYNCHRONOUS)`.

### Engine — system control (poweroff / reboot)

- The supervisor can shut the engine down or restart it through the control plane by writing a verb to the existing root control node: `poweroff` to `/dev/wanted/ctl` stops the engine without respawning the supervisor; `reboot` restarts it. No new wapp ABI — both are ordinary writes alongside the existing `start <name>` verb, so the `/dev/wanted` grant remains the capability gate (an ordinary wapp cannot reach the node).
- The engine run loop now only exits on an explicit poweroff/reboot request; a supervisor that exits on its own (e.g. `wsh` `exit` or EOF) is respawned. Previously the loop exited whenever no wapps were left.
- `wsh` gains `poweroff` and `reboot` builtins: each drains the child wapps, then writes the verb to `/dev/wanted/ctl`. `exit` simply returns from the shell and is respawned by the engine.
- Platform behaviour: poweroff exits the process (host) / `boardctl(BOARDIOC_POWEROFF)` (NuttX); reboot re-execs the engine image (host) / `boardctl(BOARDIOC_RESET)` (NuttX).
- Fixed: a wapp's console (`/dev/stdin`/`/dev/stdout`/`/dev/stderr` wired to the engine's native stdio) is no longer closed when that wapp is torn down. The stream fds (0/1/2) belong to the engine process and are shared across supervisor respawns; closing them left a respawned or re-exec'd supervisor without a console. The platform VFS driver now spares the native stdio fds on close.
- Added `test/syscontrol.sh` (and a NuttX-sim counterpart) covering poweroff/reboot/exit end to end, including that a respawned supervisor keeps a working console. Wired into the Linux `selftest` flow and the sim `nuttx-sim.sh all`.

### Engine — config and registry cleanup

- Removed the `system.defaultWapps` config field. It was parsed into the config struct but never started any wapp — all wapp lifecycle runs through the control plane. Drop the key from existing configs; `{"system": {}}` is a valid config.
- Reading `/dev/wanted/reg` as a file now returns `-EISDIR`. The registry is a directory; enumerate it with `readdir` (name:version entries). The former JSON-listing read had no consumer.
- Removed the duplicate `/dev/wanted/w/reg` mount; the registry is reachable only at `/dev/wanted/reg`.
- Dropped the `json-maker` dependency (no longer used by the engine).

### Test baseline

- ctest: **53/53**.
- selftest: **29/29** on Linux and the NuttX sim.
- `smoke-engine.sh`: green on Linux; run on the NuttX sim via `nuttx-sim.sh`.

0.6.0 (2026-06-04)
------------------

### Named pipes — inter-wapp IPC (breaking internal change)

- Promoted `/dev/pipe/<name>` from per-wapp scratch storage to a process-wide inter-wapp channel. The engine owns one shared `pipe_store_t` (lazily created on the supervisor's thread); every wapp's pipe driver references it, so a pipe opened in one wapp is visible in another. Previously each wapp got an independent ring buffer and pipes could not cross the boundary.
- Reads now **block** by default: with no data and a writer attached (or none yet seen) the read sleeps and retries instead of returning `EAGAIN`. `O_NONBLOCK` restores the non-blocking behaviour; a safety cap bounds the wait. EOF (`0`) is returned only once a writer has attached and all writers have closed (`writer_seen`). The blocking wait sleeps unlocked, so async thread cancellation cannot strand the lock.
- All shared-store access is guarded by a new platform mutex primitive (`PlatformMutexNew/Lock/Unlock/Free`): pthread-backed on linux, no-op on the single-threaded dummy.
- `_bDestroy` now closes a wapp's still-open pipe handles (decrementing the shared refcounts) before freeing its handle table, so an exiting writer no longer leaks its `writers` count and readers in other wapps reach EOF.
- `PipeDriverCreate()` takes a `pipe_store_t *`; added `PipeStoreNew()`/`PipeStoreFree()`.
- Fixed a pre-existing preopen bug surfaced by the test: `_OpenAt` joined the relative path against `rootPath` via `cwk_path_change_root`, which dropped the separator (`"/dir" + "file"` → `"/dirfile"`), so a launched wapp could never create a file in a preopen subdirectory. It now `openat()`s the path directly against the preopen directory fd.
- Extended `wapps/hello/` with `reader`/`writer` roles read from `/etc/role` in the wapp's own rootfs (each smoke image bakes its role there); added `test/smoke-pipe.sh` and `make smoke-pipe`, which exchange a payload between two `hello` instances through one named pipe and assert delivery via a host result file. Added an inter-driver `pipe_shared` unit-test group.

### Control plane — per-wapp namespace decomposition (breaking)

- Replaced the single multiplexed `/dev/wanted/ctrl` JSON-RPC node with a path-addressed per-wapp namespace under `/dev/wanted/wapps/` plus a root `/dev/wanted/ctl` create-and-launch node.
- `wapps/` enumerates known wapps (`ReadDir`); each `wapps/<name>/` exposes plain-text read nodes `state`, `version`, `id` and write nodes `ctl` (line verb `start`/`stop`) and `config` (JSON `{ console, drivers[], preopens }`). Wapp identity travels in the path, not a payload field.
- Root `ctl` carries the namespace-lifecycle verbs (`create`/`delete`) and engine power verbs (`poweroff`/`reboot`); it does not launch wapps — `start`/`stop` are per-wapp (see the lifecycle entry above).
- Removed the legacy `WantedControlDriver`, its `w/ctrl` alias, and the now-unused `WantedReadState`/`StateToJson` all-wapps JSON blob. `w/reg` is retained for the supervisor binary.
- Hardened the parse path: bounded on-stack JSON buffers (`WANTED_CTRL_JSON_MAX`) instead of variable-length arrays; per-fd read EOF state so concurrent readers keep independent cursors; oversized control writes rejected with `EMSGSIZE`.
- Added a controllable in-memory wapp-state mock to the dummy platform (`DummyWappStateSeed`/`DummyWappStateReset`).

### Control plane — multi-wapp launch lifecycle fixes

- Fixed a use-after-free on the control-plane start path: `StartWapp()` freed the `wapp_t` immediately after `PlatformWappStart()` while the just-spawned worker thread still dereferenced it (manifesting as a garbled wapp name stuck at `not_started`). Ownership now transfers to the platform thread slot and is released when that slot is later reused; the persistent supervisor image is never freed. The error path also unmaps the wapp image before freeing the struct.
- Fixed a teardown segfault during supervisor respawn: worker threads called `wasm_runtime_init_thread_env()` but never the matching destroy, leaving the stack guard pages mprotect'd `PROT_NONE` on a detached thread's (later glibc-reused) stack — the next worker's `init_thread_env` faulted touching them. `WantedWappStop()` now calls `wasm_runtime_destroy_thread_env()` on every worker exit (success or failure), restoring the guard pages and freeing the signal alternate stack.

### Tooling — sample wapp + multi-wapp smoke test

- Added `wapps/hello/` — a minimal WASI sample wapp (source + Makefile) used to exercise concurrent multi-wapp execution through wsh.
- Added `test/smoke-multiwapp.sh` — packages the sample into the registry (`REGISTRY_ROOT`), drives wsh to write a launch config and `start` the wapp, then asserts `status` reports it running alongside the supervisor.
- Added `make wapps` (compile sample wapp images) and `make smoke-multiwapp` targets.

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
