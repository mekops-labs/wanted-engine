Changelog
=========

Unreleased
----------

### Fixed

- Linux wapp stop is cooperative: `PlatformWappStop` sets the WAMR terminate
  flag and signals the worker with `SIGUSR2` to interrupt a blocked host call.
  Stopping a never-yielding wapp crashed the engine on aarch64.
- `PlatformClockNanoSleep` on Linux reports `clock_nanosleep`'s error, so an
  interrupted sleep surfaces `EINTR` instead of reporting success.

### Build

- WAMR thread manager enabled: the interpreter's terminate check compiles in
  only with it (~14 KB text).

### Testing

- `selftest-qemu` recipes run the selftest suite against a cross-built engine
  under qemu user-mode emulation (aarch64, mipsel), catching architecture-
  specific faults without target hardware.

0.10.0 (2026-07-20)
------------------

### Added

- Persistent-store free space reported at `/proc/memory` (`store_free`/`store_total`).
- Config and launch faults surfaced in production builds (always-on error log; mount, socket, image-load, trap, dropped args/envs).
- WASI `path_remove_directory` implemented (`VfsRmdir` + driver `Rmdir` slot).
- Engine prefers an overlay-staged supervisor image, falling back to the built-in.
- Sheriff v0.3.1.

### Fixed

- Launch-config env buffer sized for full key+value material (provisioning values were silently dropped).
- Stored registry images named with `REGISTRY_VERSION_SEPARATOR` (writer used raw `<name>:<ver>`; registry-installed images could not be started).
- Per-wapp linear-memory cap clamped to the module's declared max.

### Build

- OpenWRT cross-build: musl toolchain, OpenSSL linking (TLS + Ed25519), `.ipk` packaging with procd service, `openwrt-package` recipe, SDK-prerequisite image.
- Linux platform builds against musl (`<limits.h>`, `mallinfo2` gated).
- `PROFILE` resolution and `CMAKE_EXTRA_ARGS` quoting fixed in the build container.
- Sheriff build moved to a host `make` target (needs the Zig `wapp-sdk` image).

0.9.0 (2026-07-17)
------------------

### Added

- ESP-IDF platform port (ESP32-S3): native `app_main`, flash-backed LittleFS registry, octal PSRAM, A/B OTA, mbedTLS sockets (unauthenticated), `wifi` driver.
- NuttX on RP2350: flash-MTD LittleFS registry, 8 MB PSRAM heap, `PlatformEd25519Verify` (vendored `orlp/ed25519`); `wsh` and `sheriff` board configs.
- RP2350 Wi-Fi (CYW43439): `wifi` driver; `:sheriff` joins Wi-Fi before the manager loop (SSID/passphrase read interactively, never baked in).
- `serial://` socket scheme: drive a UART/USB-CDC as a byte stream; Sheriff↔Deputy reconcile loop verified over USB-CDC on RP2350.
- TLS sockets on the NuttX sim: `tcps://` over raw-mbedTLS on a host-backed usrsock netstack (CI-testable, unauthenticated).
- Internal & API docs: header doc comments, `src/`/`platform/` READMEs, error-code reference, troubleshooting guide, architecture diagrams.
- `sha256` driver (`/dev/sha256`): streaming digest, 64-hex-char read; two streams per wapp; `PlatformSha256*` seam (ESP32-S3 uses SHA hardware).
- `ed25519` driver (`/dev/ed25519`): signature verification (pubkey + sig + message → `ok`/`fail`); `PlatformEd25519Verify` seam (`-ENOSYS` without a crypto backend).
- `inflate` driver (`/dev/inflate`): streaming gzip decompression (4-byte LE size prefix); 32 KiB DEFLATE window in engine memory; vendors uzlib 2.9.5.
- `ota` driver (`/dev/ota`): A/B firmware update — control/status node + write-only `/dev/ota/slot`; `PlatformOta*` seam (ESP-IDF only, `-ENODEV` elsewhere).
- `wifi` driver (ESP-IDF): `/dev/wifi` station control — `scan`/`connect`/`disconnect`; `esp_wifi` on ESP-IDF, NuttX WAPI on RP2350.
- `psram-s3` capacity profile: ESP32-S3 + octal PSRAM, `MAX_WAPPS=20`, 64 KiB stack, 16 memory pages.
- Sheriff built from source (`wapps/sheriff` submodule, Zig); `make -C wasm/supervisor sheriff`.
- `wifi-connect` boot-time helper supervisor; `make -C wasm/supervisor wifi-connect`.
- `devcheck` wapp + `test/devcheck.sh`: end-to-end round-trip of `sha256`/`ed2551919`/`inflate` offload devices.
- `configs/sheriff-deputy.json`: plain-TCP Sheriff config for the local Deputy demo.
- RP2350 RISC-V (Hazard3) NuttX cross-build image (`docker/Containerfile.rp2350-riscv`).
- RP2350 secure boot validation: `picotool seal --sign` (`make rp2350-sign`); `SECURE_BOOT_ENABLE` OTP fuse never burned.
- `make rp2350-flash-swd` / `rp2350-reset`: SWD flash/reset via a Raspberry Pi Debug Probe.
- New platform seams: `PlatformSha256*`, `PlatformEd25519Verify`, `PlatformOta*`, `PlatformExtramEarlyInit`.
- Per-preopen WASI rights: `fd_fdstat_get` advertises capabilities; over-grant `path_open` refused with `ENOTCAPABLE`.

### Fixed

- `convertVfsFlags`: explicit read-only host access mode (was relying on `O_RDONLY == 0`), fixing `EACCES` on NuttX reads through WASI preopens.
- `serial://` sockets: raw mode + RX flush on open, fixing a USB-CDC framing desync.
- Registry install-by-ref: namespace separator `:` (was `@`); image ref parses `<name>:<tag>` (was `@`); filenames still `@`.
- NuttX `PlatformWappStart`: `pthread_create` failure surfaced instead of proceeding on an invalid handle.
- ESP-IDF `psram-s3`: WAMR host-managed heap disabled (fragmented octal PSRAM).
- ESP-IDF `esp_partition_mmap`: proxied through an internal-stack helper thread.

0.8.0 (2026-06-29)
------------------

### Added

- `gpio` driver: `/dev/gpio`, a text level node — `write "1"/"0"` drives the pin, `read` returns the level. NuttX only (`-ENODEV` elsewhere).
- `blink` sample wapp: toggles `/dev/gpio` at 1 Hz.
- `/proc/wapps/<name>/` observability namespace: read-only `state`, `image`, `version`, `id`, `exit_code`, `memory`; reachable without the control mount. `memory` reports per-wapp WASM linear-memory accounting.
- `log` mount: read-only view of per-wapp captured logs, bound via `mounts[]`; `name=<wapp>` scopes it. Grantable independently of `/dev/wanted`.
- `observer` sample wapp: enumerates `/proc/wapps`, tails logs, confirms the control plane is unreachable without `wanted`.
- `pipe` console backing: a stdio slot backed by a named pipe a peer reads at `/dev/pipe/<wapp>.<slot>`; `out`/`err` lossy, `in` reads a peer's writes.
- Registry descriptors report each image's declared linear-memory profile (`init_pages`, `max_pages`, `can_grow`, `over_cap`), parsed from the wasm `(memory)` section without loading.

### Changed

- Per-wapp control namespace reduced to `ctl`, `state`, `config`; observability reads move to `/proc/wapps/<name>/` and logs to the `log` mount.
- NuttX built-in default config runs the supervisor privileged (`system.privileged: true`).
- Launch-config slot tables heap-allocated on first use (was static `.bss`).
- Driver resolution split into core + per-platform tables; an unimplemented driver fails with `-ENODEV` (was a no-op). Driver name matching is now exact (was prefix).
- `/proc/wanted` now reports `wasm_max_pages`, `log_slots`, `wasm_worker_stack`, `max_drivers`, `max_options`, `drivers`.
- Worker threads created with an explicit native C stack (`WASM_WORKER_STACK_SIZE`) on every platform.

### Build

- Centralized resource limits into `src/include/wanted-config.h` (cmake/`-D` overridable).
- Added resource-limit profiles (`tiny`, `constrained`, `small`, `big`) under `cmake/profiles/`.
- Made `MAX_DRIVERS_CNT`/`MAX_OPTIONS_SIZE` profile-tunable footprint knobs.
- Added `WASM_WORKER_STACK_SIZE` and `WASM_MAX_MEMORY_PAGES` knobs.
- Added `make sizes` (footprint/struct sizes) and `make memcap`.

### ESP32 (NuttX, Xtensa)

- Engine runs on the classic ESP32 (ESP32-WROVER) via the NuttX port — the first hardware bring-up of that port (the esp-idf ESP32 path was removed in 0.5.0). Xtensa cross-build image (`docker/Containerfile.esp32`) and `esp32-build`/flash recipes.
- Supervisor boots from a firmware-bundled read-only ROMFS with a first-boot factory seed; installed wapps persist on a writable registry.
- In-RAM image cache preloaded at boot serves wapp launches RAM-to-RAM, so a flash read never corrupts live PSRAM (classic-ESP32 flash/PSRAM cache coexistence bug) — enables concurrent wapps.
- PSRAM external-RAM heap backs the image cache and WAMR runtime, freeing scarce internal RAM.
- Writable wapp registry on an SD card (FAT over SPI), whose reads don't disable the flash/PSRAM cache.
- `wifi` VFS driver + `wifi-connect` sample: `wlan0` scans, WPA2-associates, and reports its DHCP lease over a text node.

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
