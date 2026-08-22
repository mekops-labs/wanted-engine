Changelog
=========

Unreleased
----------

### Fixed

- A sleep shorter than one tick of the scheduler waits a whole tick on
  ESP-IDF, where `usleep` below a tick spins on the CPU instead of blocking.
- The blocking-read cap of a named pipe is a deadline of 5 s, not 5000 passes
  of a 1 ms sleep.

0.13.0 (2026-08-17)
-------------------

### Added

- `/dev/ota` reports `pending_digest`: the staged image's build-time digest,
  compared against `/proc/wanted`'s `digest` once it boots. Absent while
  nothing is staged.
- An ESP-IDF boot-time Wi-Fi join (`CONFIG_WANTED_ESP_IDF_WIFI_BOOT_JOIN`)
  brings the radio up before the supervisor starts, from `/data/wifi.conf`
  or a console prompt; credentials stay in RAM.
- An ESP32-S3 board configuration running Sheriff: `OTA_PROFILE=s3-sheriff`,
  the `s3-wapps` A/B layout with the production supervisor and the
  boot-time join.
- The supervisor image may come from the wapp registry: `imagePath` of the
  form `registry:<name>[:<version>]` resolves like a launch config's `image`.
- A `gpio` grant entry takes `init=0|1`, the level an output holds from the
  moment the grant opens it. Default `0`; invalid on an input.
- ESP32-S3 A/B app slots are 1792 KiB, up from 1600, taken from `persist`. A
  slot size must be a multiple of 64 KiB.
- `CONFIG_WANTED_MAX_WAPP_IMAGE_KB` bounds one registry image, default 160 KiB.
  The Telegraph profiles take 256 KiB over 12 slots.
- A board profile can seed wapps built outside this repository:
  `WANTED_EXTRA_SEEDS` takes `<ref>=<path to .wasm>` entries.
- An ESP32-S3 board configuration for the Telegraph display:
  `OTA_PROFILE=s3-telegraph-sheriff`, `uart` built in, Sheriff as
  supervisor, wapps from the registry, a boot-time network join.
- The registry refuses to remove an image the firmware seeded (`-EPERM`); a
  supervisor may sweep everything else without knowing which images the
  board provides.
- The engine's own error channel is readable: `LOG_ERROR` output is captured
  under `.engine`, served by the log mount. A `mounts[]` grant of
  `name=.engine` gives that log and nothing else.
- The log store gains a reserved slot for `.engine` so wapp churn cannot
  evict it.
- A wake descriptor a blocking wait can watch, raised by a stop to end a
  wait parked in a host call. Backed by eventfd on ESP-IDF, a pipe on the
  test platform.
- The `socket`, `uart` and pipe drivers watch it: a blocked accept,
  connect, read or write ends on a stop. UART's transmit drain before a
  rate change is capped at 1 s.
- The engine's log survives a reset: mirrored into RTC memory on ESP-IDF
  (nothing on a host), served as `.engine.prev`. Sized by
  `WANTED_LOG_PERSIST_CAP`; lost on power loss.
- `/proc/wanted` and the log carry the reset reason (power-on, watchdog,
  panic, commanded reboot), read from `PlatformResetReason`.

### Changed

- `CONFIG_ESP_TASK_WDT_PANIC` is on: a wedged task now resets the board
  instead of only printing.
- The ESP-IDF firmware seed list drops `looper`, `devcheck` and `blink`;
  only `wifi-connect` and the firmware installer stay seeded on every
  board.

### Fixed

- `ENOTEMPTY`, `ELOOP`, `EOVERFLOW` and `ENOLCK` reached a wapp as `EINVAL`,
  having no WASI mapping. A non-empty directory delete reported a bad
  argument instead.
- A wapp start took the first slot holding a finished wapp, erasing its
  record. A finished job was then indistinguishable from one that never
  ran. Only an empty slot is free now.
- A directory read past its buffer's capacity reported the whole buffer
  filled, skipping the entry that did not fit and spinning a walker past
  ~10 entries. Every VFS driver carried the same code.
- The registry driver kept one set of state for every open descriptor, so a
  second opener disturbed an install and one close answered for another's writes.
- An ESP-IDF registry install could take a slot a loaded wapp still runs from,
  erasing flash under it. A mapped slot is never allocated or overwritten.
- A registry install whose start failed answered `-EBADF` on every later write,
  hiding the error that stopped it. A full registry read as a bad descriptor.
- `/dev/wanted/reg/<ref>` and `unlink` answered `-ENOENT` for an image the
  registry holds unless the root had been opened first.
- A registry lookup matched a name by prefix, so `app` answered for `app-v2`.
  Both halves of a ref are compared in full.
- ESP-IDF firmware reported `version: unknown` at `/proc/wanted`: nothing
  defined it. A checkout with no reachable tag now fails the build.
- The ESP-IDF build compiled the `gpio`, `uart` and `ota` drivers
  unconditionally, so deselecting any of them broke the build.
- A build with `CONFIG_WANTED_VFS_UART=y` failed on ESP-IDF: `driver/uart.h`
  was off the include path. Component requirements are now listed
  unconditionally.
- A VFS driver's unassigned vtable slot held heap garbage rather than
  `NULL`; a caller's `NULL` check passed on it and called garbage as a
  function.

0.12.0 (2026-08-05)
-------------------

### Added

- Classic ESP32 (Waveshare ESP32 One, quad PSRAM, 4 MB flash) support under ESP-IDF, alongside the ESP32-S3. `make esp32` builds it. Single factory app slot, no A/B OTA.
- `/dev/ota` takes an `abort` command, which discards a streaming image write and releases the session. A write that begins and never commits holds the slot until the board reboots.
- The firmware flasher wapp is factory-seeded into the registry as `flasher:<supervisor version>`, so it is present before any network is up. It installs an engine firmware image and exits.
- `/proc/wanted` reports a `digest` line: the running image's build-time digest, 64 lowercase hex characters. It names the exact bytes that booted, which a `version` string alone does not. ESP-IDF stamps one.
- `/proc/wanted` reports a `supervisor_abi` line: the version of the contract between the engine and a supervisor wapp. A supervisor reads it first and writes `rollback-supervisor` when it cannot support the value.
- `/dev/wanted/ctl` takes `rollback-supervisor`, which pins the compiled-in supervisor image and reloads it. `-EALREADY` when that image is what already runs.
- A wapp can serve a socket: a `sockets[]` entry with `"role": "listen"` binds its address. `tcp` accepts each connection onto an fd of its own; `udp` reads a datagram and answers its sender.
- Socket listeners are built in with `CONFIG_WANTED_VFS_SOCKET_LISTEN` (on for OpenWRT), and `backlog`/`max_conns` bound them. A secure transport cannot listen: TLS server credentials have no source.
- The `gpio` driver addresses any pin the launch config grants it, at `/dev/gpio/<name>/{value,direction}`. A pin is addressed by name, so one wapp image runs on boards with different wiring.
- Direction, pull and drive are fixed by the `gpio` grant, and `direction` is read-only. `readdir` lists exactly the granted pins; an ungranted one is unreachable.
- A `uart` driver: a serial port at `/dev/uart/<port>/`, with a `data` byte stream plus writable `baud` and `format` nodes, because one link can carry both a bootloader sync and a framed channel.
- A blocking `uart` `data` read returns on a byte or on `-EINTR`, with no wall-clock cap; `O_NONBLOCK` is how a wapp bounds a wait. One wapp holds a port exclusively. Built in with `CONFIG_WANTED_VFS_UART`.
- Linux stages an A/B firmware update for real: `/dev/ota` writes the inactive slot under `CONFIG_WANTED_OTA_SLOT_ROOT` (default `/boot`) and arms a trial boot at commit.
- `/proc/memory` reports a `wasm_pages_free` line: the sum, across every loaded wapp, of the WASM linear-memory headroom left before its own page ceiling.

### Removed

- The NuttX build path for the classic ESP32: `docker/Containerfile.esp32` and `configs/esp32-nuttx_defconfig`.

### Changed

- `ota` is a core driver behind `CONFIG_WANTED_VFS_OTA`, so a launch config granting it off ESP-IDF no longer fails with `-ENODEV`. Deselect it on a target with no A/B mechanism, as the OpenWRT defconfig does.
- `gpio` is a core driver, and only the line behind it stays per-platform (`PlatformGpio*`). **Breaking change to the `gpio` ABI:** a wapp opens `/dev/gpio/<name>/value`, and the single `/dev/gpio` node is gone.
- A missing, malformed or empty `gpio` `pins=` clause fails the launch. Every launch config granting `gpio` must carry one; the `blink` sample is migrated.
- A factory-seed image is written only when its registry ref is absent, so an image installed over a seeded ref survives the next boot.
- A staged supervisor image that starts and exits at once counts toward the rollback ceiling. An image can load, find it cannot work with the engine, and leave cleanly, which no launch check sees.

### Fixed

- A supervisor image that fails to load no longer leaves the engine holding freed layer memory: a failed reload presented a freed pointer as a valid image and the next start crashed the engine.
- PSRAM allocations are 8-byte aligned (`heap_caps_aligned_alloc`).
- The classic ESP32's UART console installs a blocking driver.

0.11.0 (2026-07-22)
-------------------

### Changed

- Build configuration moved to a Kconfig tree at the repo root (vendored kconfiglib in `tools/`); `.config` is per-build-dir, every limit is a bounded `CONFIG_WANTED_*` symbol, and `src/include/wanted-config.h` is gone.
- `cmake/profiles/*.cmake` replaced by `configs/*_defconfig` covering boards and capacity envelopes; the CMake-syntax-to-`-D` scrapers are gone.
- VFS drivers are Kconfig-selectable; deselecting drops source, factory row, declaration, and vendored deps (c9 for `9p`, uzlib for `inflate`). A minimal selection removes ~31% of engine `.text`.
- Supervisor variant (sheriff / wsh / selftest) is a Kconfig choice.
- Build target (linux / nuttx / esp-idf / openwrt) is a Kconfig choice; `just build` builds it and the per-target recipes are gone.
- Default launch config is a Kconfig choice (`WANTED_DEFAULT_CONFIG`), shipped per target as a file, an OpenWrt conffile, or a compiled-in blob, and parsed at build time.
- Debug traces, coverage, static-CLI linking, secure sockets (TLS), and the OpenWrt SDK (generic arch or a custom URL) are Kconfig options too.

### Added

- Every target stages a ready artifact under `dist/<target>/`.
- OpenWrt: deployment settings (endpoints, device identity, cadence) in UCI (`/etc/config/wanted`); the service stays down until a control-plane endpoint and Marshal key are set.
- Supervisor live update: `reload-supervisor` on the root `ctl` swaps the image at the next supervisor exit; child wapps keep running (staging is atomic-rename). A repeatedly failing staged image rolls back to the compiled-in one.
- 9P `unix://<socket-path>`: reach an on-box server over a filesystem socket.
- Out-of-tree drivers via `ExtraDriverTable()` / `WANTED_EXTRA_DRIVERS_DIR`, searched after the core tables so they cannot shadow a core name.
- `make <recipe> <arg>` forwards the argument to the `just` recipe; `just sizes current` prices the configured build for its ABI.
- Sheriff v0.3.2: env-only identity, one env var per Marshal key, configurable reconcile cadence.

### Fixed

- OpenWrt packaging builds the build dir's `.config` (supervisor variant included) instead of re-seeding a defconfig and hardcoding wsh.
- ESP-IDF embeds the configured supervisor, not a hardcoded wsh, and packages the factory-seed images and launch-config blob at configure time.
- ESP-IDF target builds again (`cfmakeraw` → POSIX termios, generated header on the component include path) and reports storage from the mounted littlefs, not spiffs.
- wasi-sdk 33 compatibility: `call_indirect` table index padded.
- Linux wapp stop is cooperative (WAMR terminate flag + `SIGUSR2`); a never-yielding wapp no longer crashes the engine on aarch64.
- `PlatformClockNanoSleep` propagates `EINTR`; 9P `Stat` copies the parsed stat out of the dead response frame.

### Build

- WAMR thread manager enabled (the interpreter's terminate check compiles in only with it; ~14 KB text).

### Testing

- `selftest-qemu`: cross-built engine under qemu user-mode (aarch64, mipsel), catching architecture-specific faults without hardware.
- `live-update`: asserts child-wapp continuity, armed-only adoption, and rollback.
- 9P against a live 9P2000 server over a local socket (walk/open/stat/read/write).
- `test-extra-drivers`: an out-of-tree driver resolves and cannot shadow a core name.

0.10.0 (2026-07-20)
-------------------

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

- Engine runs on the classic ESP32 (ESP32-WROVER) via the NuttX port — the first hardware bring-up of that port. Xtensa cross-build image (`docker/Containerfile.esp32`) and `esp32-build`/flash recipes.
- Supervisor boots from a firmware-bundled read-only ROMFS with a first-boot factory seed; installed wapps persist on a writable registry.
- In-RAM image cache preloaded at boot serves wapp launches RAM-to-RAM, so a flash read never corrupts live PSRAM (classic-ESP32 flash/PSRAM cache coexistence bug) — enables concurrent wapps.
- PSRAM external-RAM heap backs the image cache and WAMR runtime, freeing scarce internal RAM.
- Writable wapp registry on an SD card (FAT over SPI), whose reads don't disable the flash/PSRAM cache.
- `wifi` VFS driver + `wifi-connect` sample: `wlan0` scans, WPA2-associates, and reports its DHCP lease over a text node.

0.7.1 (2026-06-16)
------------------

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
