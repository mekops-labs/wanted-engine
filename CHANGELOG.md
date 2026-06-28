Changelog
=========

Unreleased
----------

### Added

- `gpio` device driver: a wapp granted `{ "name": "gpio" }` in its launch config's `drivers[]` gets `/dev/gpio`, a text level node — `write "1"/"0"` drives the pin high/low and `read` returns `"0\n"/"1\n"`. The engine performs the GPIO ioctl (`GPIOC_WRITE`/`GPIOC_READ`); the wapp uses only WASI. Backed by the host GPIO character device on NuttX (default `/dev/gpio0`, overridable via the driver options). Available only on platforms that implement it (NuttX); a config naming `gpio` on a platform without it (Linux) fails the launch with `-ENODEV`.
- `blink` sample wapp: toggles `/dev/gpio` in a 1 Hz loop.
- `/proc/wapps/<name>/` per-wapp observability namespace: a read-only directory tree with `state`, `image`, `version`, `id`, `exit_code`, and `memory`. `/proc/wapps` is now a directory enumerating the running wapps. `memory` reports per-wapp WASM linear-memory accounting (`linear_cur`/`linear_max` bytes, `pages_cur`/`pages_max`). The namespace is reachable without the `/dev/wanted` control mount, so an observability wapp can read it without control authority.
- `log` mount type: a read-only directory driver over the per-wapp log store, bound via `mounts[]` at an operator-chosen path. `<path>/<name>` reads wapp `<name>`'s captured stdout/stderr and the mount enumerates wapps with a live log slot; an optional `name=<wapp>` option scopes the view to one wapp. Grantable independently of `/dev/wanted`.
- `observer` sample wapp: enumerates `/proc/wapps`, tails logs through a `log` mount, and confirms the control plane is unreachable without the `wanted` mount.
- Registry descriptors (`/dev/wanted/reg/<name>:<version>`) now report each image's declared linear-memory profile — `init_pages`, `max_pages` (or `null`), `can_grow`, and `over_cap` when the build caps per-wapp memory — parsed from the image's wasm `(memory)` section without loading the module. A pre-flight check for whether an image fits the cap, instead of "start it and see if it's refused".

### Changed

- The per-wapp control namespace `/dev/wanted/wapps/<name>/` is reduced to `ctl`, `state`, and `config`. The pure-observability reads (`image`, `version`, `id`, `exit_code`) move to `/proc/wapps/<name>/`, and a wapp's captured log moves from `/dev/wanted/wapps/<name>/log` to the `log` mount. `state` stays on the control plane (it spans the pre-launch `created`/`not_started` lifecycle the `/proc` view cannot express) and is mirrored read-only at `/proc/wapps/<name>/state`.
- The NuttX built-in's compiled-in default config now runs the supervisor privileged (`system.privileged: true`), so the privileged `/proc` entries (e.g. `/proc/memory`) are readable by the control-plane supervisor. Launched wapps are configured separately and do not inherit it.
- The launch-config slot tables (the control-plane `pending` reservations and the parsed engine config) are now allocated on the heap on first use instead of living in static `.bss`. On constrained targets this moves the large per-`wapp_config_t` driver/mount/socket slot tables off limited internal RAM (and onto a heap that may extend into PSRAM); the slot-size limits can be raised without static-RAM pressure.
- Driver resolution is split into a platform-agnostic core table and a per-platform table: each platform registers only the drivers it implements (NuttX: `gpio`, `wifi`; Linux and the test platform: none). A launch config naming a driver the platform does not offer fails with `-ENODEV` instead of resolving to an in-memory no-op. Core driver names (`wanted`, `null`, ...) are reserved and resolved first, so a platform cannot shadow them. Driver name matching is now exact (was a prefix match, so `gpioX` matched `gpio`).
- `/proc/wanted` now reports `wasm_max_pages`, `log_slots`, `wasm_worker_stack` (the effective per-wapp worker thread stack), `max_drivers`, `max_options`, and `drivers` (the drivers available on this build).
- Worker threads are now created with an explicit native C stack (`WASM_WORKER_STACK_SIZE`) on every platform instead of the OS default, so the recursive WAMR interpreter cannot overflow a small RTOS default thread stack (NuttX defaults to ~2 KB).

### Build

- Centralized engine-wide resource limits into `src/include/wanted-config.h` (overridable via cmake profiles and `-D` in nuttx).
- Added resource-limit profiles (`tiny`, `constrained`, `small`, `big`) under `cmake/profiles/`.
- Made `MAX_DRIVERS_CNT` and `MAX_OPTIONS_SIZE` profile-tunable footprint knobs (moved to `wanted-config.h`); the constrained default shrinks the launch-config slot table so the engine's static `.bss` fits an ESP32's internal DRAM.
- Added the `tiny` profile for boards with no external PSRAM (e.g. ESP32-WROOM): the smallest limits, sized so both static `.bss` and the runtime WASM allocations fit internal RAM with no heap relocation.
- Added `WASM_WORKER_STACK_SIZE` as a profile-tunable knob; `make sizes` now includes the worker stack in the per-wapp footprint.
- Added `WASM_MAX_MEMORY_PAGES` to cap per-wapp linear memory (`make memcap` and some new selftests added for testing)
- Added `make sizes` to report memory footprint and struct sizes for each profile.

0.7.1 (2026-06-16)
----------

### Fixes

- 9P driver: off-by-one fd bounds check allowed an out-of-bounds slot at `fd == MAX_OPENED_FILES`.
- VFS readdir: initialise the dirent so an empty listing returns a valid cookie, not garbage (9P, virtual, registry, Linux, NuttX).
- Linux registry: check `fstatat` (was reporting garbage entry size on failure); replace a stack VLA with a fixed-size scan buffer.
- Launch config: bound string copies of JSON values into fixed-size fields (was unbounded `strcpy`).
- CLI: guard config-file size (avoid `malloc(0)`) and free the config buffer.
- Linux RNG: `PlatformRandom` uses seed-consistent `random()` (was unseeded `rand()`).
- wapp start: reject launch when the slot table is full instead of indexing past it.

### Build

- Re-homed to `gitlab.com/mekops/wanted/wanted-engine` (clone URL, CI/devcontainer image, badges).
- Renamed the wapp SDK image `wasm-sdk` → `wapp-sdk`.
- Pinned the WAMR classic interpreter explicitly (`WAMR_BUILD_FAST_INTERP=0`).

0.7.0 (2026-06-15)
------------------

### Launch config — `volume` mounts

- Engine-managed persistent store: the wapp names a volume, the engine owns the host path. `options`: `name=` (default `default`), `ro`/`rw`, `shared`.
- Private volumes are namespaced per wapp; shared volumes share a global namespace; the two never alias. `ro` enforced with `-EROFS`.
- Created on first use; persists across restarts, reboots, and `delete`. No locking or quotas.
- Invalid name (`/`, `.`, `..`) or unknown option rejected at install.
- New `PlatformVolumeRoot` primitive.

### Launch config — `platform` bind mounts

- `options`: `src=<hostpath>` (default: the mount `path`), `ro`/`rw`.
- `ro` enforced in the engine (writes/mkdir/rename → `-EROFS`); a `ro` mount's host dir must already exist.
- Malformed options rejected at install.
- Path resolution confined to the host dir via `openat2(RESOLVE_BENEATH)` (Linux ≥ 5.6; fails closed otherwise).

### Launch config — drivers / mounts / sockets split

- Flat `drivers[]` split into `drivers[]` (`/dev/<name>`), `mounts[]` (absolute path), `sockets[]` (`/net/<name>`); per-section install validation.
- `/dev/stdin|stdout|stderr` alias the wapp's console streams (fd 0/1/2).
- `preopens[]` removed; a host directory is a `platform` `mounts[]` entry.
- VFS router supports a single-driver mount at any absolute path.
- Socket and 9P addresses use `<scheme>://<host>:<port>` (`tcp`/`udp`/`tcps`/`udps`); `bus` transport removed.
- Config-exposed `virt` driver removed (the primitive stays internal).

### Images — manifest removed

- `manifest.json` gone; image identity (name + version) comes from the registry filename. `requirements[]` dropped.
- Instance identity decoupled from image: `create <name>` reserves; the image is resolved at start (`start` arg → config `image` → instance name). One image runs as N instances; recorded at `wapps/<name>/image`.
- Install by ref: the stored file is named by the write path; reading a registry entry returns a `{name,version,size}` descriptor.
- Supervisor TARs and sample wapps ship `app.wasm` only.

### Control plane — env/argv, lifecycle, exit codes, slot release

- Wapps get env/argv via WASI (`args[]`, `envs[]`); `environ_sizes_get`/`environ_get` implemented.
- Explicit lifecycle: `create <name>` → write `config` → bare `start`. An unknown name returns `-ENOENT`; the root `ctl` no longer launches.
- `delete <name>` frees a reservation or terminal slot (new `PlatformWappRelease`); a `running`/`starting` wapp returns `-EBUSY`.
- `wapps/<name>/exit_code` exposes the WASI exit code (valid only when `exited`); a trap resolves to `failure`.
- `wsh`: added `create`, `delete`, `set_config`.

### Engine — `/proc/wanted`

- Read-only node exposing platform, version, and compile-time resource ceilings as `key:\tvalue` lines.
- New `PlatformName()` accessor; `version` is the git SemVer baked in via `WANTED_VERSION`.

### Engine — default console backing

- Unset `console.in`/`out`/`err` default to `null`/`log`/`log`, so a wapp starts without an explicit console.

### Engine — supervisor launch failure fails loudly

- After three consecutive launch failures the engine reports the error and stops instead of respawning forever; a clean exit still respawns.

### Engine — log console driver

- Per-wapp log ring buffer at `/dev/wanted/wapps/<name>/log`; a `console:log` wapp is captured there. LRU eviction.
- `debug_trace` emits via a raw `write()`.

### Engine — supervisor priority and interruptible stop

- Supervisor thread runs one priority step above workers.
- NuttX interruptible stop via `SIGUSR2` + a per-worker `interrupted` flag; Linux keeps `pthread_cancel`.

### Engine — system control

- `poweroff`/`reboot` verbs on `/dev/wanted/ctl`; the run loop exits only on an explicit request (a self-exiting supervisor respawns).
- `wsh`: added `poweroff`/`reboot`.
- Platform: process exit / re-exec (host), `boardctl` (NuttX).
- Fix: console fds (0/1/2) are no longer closed on wapp teardown (they are shared across respawns).

### Engine — config and registry cleanup

- Removed `system.defaultWapps`.
- Reading `/dev/wanted/reg` as a file returns `-EISDIR` (enumerate with `readdir`).
- Removed the duplicate `/dev/wanted/w/reg` mount and the `json-maker` dependency.

### NuttX port (Linux sim, CI-gated)

- `platform/nuttx/` fully implemented (no `-ENOSYS` stubs): pthreads lifecycle, registry, VFS, clock, random, BSD sockets (no TLS), app entry, mutex.
- NuttX sim is a first-class target (`make nuttx-{deps,build,smoke,selftest,shell}`); NuttX forks vendored at `third_party/`. CI runs smoke + selftest on the sim.
- Fixes: `CONFIG_INTERPRETERS_WAMR_THREAD_MGR` for `wasm_runtime_terminate`; direct `WA_threadEnd` (no `CONFIG_PTHREAD_CLEANUP`); clean sim poweroff.

### Selftest

- `selftest` supervisor variant runs the suite from inside WASM on Linux and the NuttX sim (TAP, 29 scenarios). Shell smoke scripts retired (kept `smoke-engine.sh`).

### Licensing

- Relicensed MIT → Apache-2.0; added `NOTICE`, `CONTRIBUTING.md`, and SPDX headers on all first-party files.

### Test baseline

- ctest 53/53; selftest 29/29 (Linux and NuttX sim).

0.6.0 (2026-06-04)
------------------

### Named pipes — inter-wapp IPC

- `/dev/pipe/<name>` promoted to a process-wide channel backed by one shared store; a pipe opened in one wapp is visible in another.
- Reads block by default (writer-aware); `O_NONBLOCK` restores non-blocking; EOF once all writers close.
- New `PlatformMutex*` primitive guards the shared store.
- Closing a wapp releases its open pipe handles so writer counts don't leak.
- `PipeDriverCreate()` takes a store; added `PipeStoreNew`/`PipeStoreFree`.
- Fix: `_OpenAt` dropped the path separator, so a wapp could not create files in a preopen subdirectory.

### Control plane — per-wapp namespace

- Replaced the multiplexed `/dev/wanted/ctrl` JSON-RPC node with a path-addressed `/dev/wanted/wapps/` namespace plus a root `ctl`.
- Per-wapp nodes: `state`/`version`/`id` (read), `ctl`/`config` (write); root `ctl` carries `create`/`delete`/`poweroff`/`reboot`.
- Removed `WantedControlDriver`, the `w/ctrl` alias, and the all-wapps JSON blob.
- Bounded JSON buffers (no VLAs); per-fd EOF; oversized writes rejected with `EMSGSIZE`.
- Dummy platform gains a wapp-state mock.

### Control plane — lifecycle fixes

- Fix use-after-free: `StartWapp()` freed the `wapp_t` while the worker thread still used it; ownership now transfers to the slot.
- Fix respawn segfault: workers never called `wasm_runtime_destroy_thread_env()`, leaving mprotected guard pages on reused stacks.

### Tooling

- Added the `wapps/hello/` sample, `test/smoke-multiwapp.sh`, and `make wapps`/`smoke-multiwapp`.

0.5.0 (2026-05-19)
------------------

### WAMR migration

- Replaced `wasm3` with WAMR 2.4.4 (classic interpreter), vendored at `vendor/wamr`.
- `platform/esp-idf` removed; `platform/nuttx/` stub added.
- Full wapp lifecycle ported; native functions via `NativeSymbol`; idempotent `EnsureWamrInit()`.
- Per-thread signal env per worker; engine keeps a writable copy of the wasm bytes; `proc_exit` modelled as an exception.

### WASI bridge

- `wasi-vfs.c` rewritten to WAMR conventions; all 24 preview1 handlers; `vaddr()` validates linear-memory addresses.
- Local `wasi_types.h` added (WAMR's WASI headers excluded); both `wasi_unstable` and `wasi_snapshot_preview1` registered.

### TarFS — OCI-layered filesystem

- `wapp_t` carries an OCI layer stack; O(log N) lookup, boot pre-fetch, `.wh.` whiteout shadowing.
- PAX and GNU long-name support; file/dir ops; boot wired through TarFS. Legacy romfs path retired.

### Supervisor — TAR image loading

- Supervisor loaded from a ustar TAR (`PlatformWappLoad`); default variant renamed **sheriff** (`wsh` remains).
- Added `supervisor.imagePath` config override.
- Fix: sheriff boot failure from stale startup state.

### VFS — router and fixes

- Stateless prefix router; `DevFs`/`NetFs` on direct registration; typed FD table; `cwk_path_normalize`.
- Added ProcFS (`/proc/wapps`, `/proc/memory`), named pipes, stdio stub devices, `system.privileged` + `VfsSetPrivileged()`.
- Fix: errno propagation from `route_open`/`DevFs_Open`/`NetFs_Open`; stale `devfs` readdir entries.

### Manifest

- `requirements` field added to `manifest.json`; sheriff validates capabilities before start.

### wsh

- Fix `ls` path resolution (a relative `lstat` broke listings outside the working directory).

### CLI and tooling

- `wanted_cmd` → `wanted-cli`; added devcontainer config, LLVM `clang-format`, and a smoke suite.

### Test baseline

- 20/20 ctest, 16/16 smoke.

0.4.0
-----

- Initial ESP32 support (sockets not yet supported).
- Reduced stack usage.

0.3.0
-----

- Added the 9P2000 client driver (external driver plugins; usable, error handling WIP).
- Prepared the Docker build/CI environment.

0.2.1
-----

- First working version; can run multiple wapps.

0.2.0
-----

- Most of the VFS, WASM, and WASI implementation done (not yet usable).
