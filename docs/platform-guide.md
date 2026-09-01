---
title: "Platform Guide"
date: 2026-06-08T17:30:00+01:00
weight: 70
toc: true
description: "Building for and porting to each target: how the build target is configured, the Platform seam, Linux, the NuttX simulator, the RP2350 and ESP32 hardware targets, OpenWrt, and what a new port must implement."
---

The engine core is platform-agnostic; everything OS-specific lives behind the `Platform*` seam. This guide covers the seam, the shared POSIX core the production targets build on, each target, how the build is configured, and the checklist for a new port.

**Which target gets built is configuration, not a choice of recipe.** `just build` reads the target from this build directory's `.config` and dispatches — see [Target selection](#target-selection). The per-target build recipes it replaced are gone.

## The platform seam

Every platform implements the contract in `platform/include/platform.h`. A conforming port must provide a working body for all of it — there are no optional symbols:

| Area | Symbols |
|------|---------|
| Wapp lifecycle | `PlatformWappLoad` / `Unload` / `Start` / `Stop` / `Release` / `Loop` / `GetState`, `PlatformWorkerStackSize` |
| Registry backend | `PlatformRegistryRead` / `Write` / `Remove` / `WappLoad` / `ReadImage` |
| Filesystem | `PlatformOpenStateDir`, `PlatformFsRename`, `PlatformFsMkdir`, `PlatformFsRmdir`, `PlatformVolumeRoot` |
| Network | `PlatformNetOpen` / `Connect` / `Recv` / `Send` / `Accept` / `Shutdown` / `Close` / `Free` |
| Clock | `PlatformClockGetRes` / `GetTime` / `NanoSleep` |
| Random | `PlatfromGetRandom` |
| Storage | `PlatformStorageStats` — free/total bytes of the store backing the registry and volumes, reported at `/proc/memory`; zeroes where the platform cannot answer |
| Identity | `PlatformName`, `PlatformFirmwareDigest` — the build-time image digest reported at `/proc/wanted`; `-ENOSYS` where the platform stamps none |
| Memory | `PlatformMemoryStats`; `PlatformExtram*` (`Malloc` / `Realloc` / `Free` / `EarlyInit`) — the external-RAM (PSRAM) heap backing the engine's large allocations (image cache, WAMR runtime) where one exists |
| Concurrency | `PlatformMutexNew` / `Lock` / `Unlock` / `Free` |
| Drivers | `PlatformDriverTable` — the platform's additions to the launch-config driver names (`wifi` on NuttX and ESP-IDF, none on Linux). `gpio`, `uart` and `ota` are core drivers instead: their tree and grant grammar are identical everywhere, and only the line, port or slot behind them is per-platform, behind `PlatformGpio*`/`PlatformUart*`/`PlatformOta*`. A build may add a third table from a tree outside this repo; see [Out-of-tree drivers](#out-of-tree-drivers) |
| Crypto | `PlatformSha256New` / `Update` / `Final` / `Free` — streaming digest behind `/dev/sha256` (software body in `posix/sha256.c`; ESP32-S3 uses the SHA peripheral; no `-ENOSYS` path). `PlatformEd25519Verify` — the one seam symbol allowed to report an absent backend (`-ENOSYS`); the `/dev/ed25519` verdict read surfaces it to the wapp. Real on Linux (OpenSSL), and on NuttX and ESP-IDF through the vendored verify-only `orlp/ed25519` |
| Firmware update | `PlatformOtaInit` / `Confirm` / `GetBootState` / `BeginWrite` / `Write` / `Commit` / `Abort` / `Rollback` — the A/B OTA seam behind `/dev/ota`; real on ESP-IDF (`esp_ota_ops`) and Linux (slot directories, see below), `-ENOSYS` on NuttX |
| Power / process | `PlatformSetProcessArgs`, `PlatformRequestShutdown`, `PlatformRequestReboot` |

The invariants every platform must honour: a wapp runs on its own thread; `PlatformWappStop` must interrupt a wapp that is blocked in a host syscall (not merely set a flag and wait); `PlatformWappLoop` blocks until an explicit shutdown/reboot request and respawns a supervisor that exits on its own; memory stats report heap usage; the registry resolves a wapp image by name.

### Seam contracts worth knowing

A few symbols carry a contract the signature does not show. A port that gets these wrong compiles and then misbehaves at runtime, so they are written out here.

**`PlatformGpio*` and `PlatformUart*` back a core driver.** The driver owns the wapp-facing tree, the grant grammar, the blocking policy and the line-setting text format; the platform owns only the line or the port. `plat_gpio_cfg_t.address` is the grant's middle field (`pins=boot0:4:out` → `4`) and is interpreted in the backing and nowhere else — a decimal pin number on ESP-IDF, a character-device path such as `/dev/gpio0` on NuttX. A wapp never sees it, which is what lets one wapp image run on boards with different wiring. `plat_uart_cfg_t.options` carries every grant key the driver did not consume, comma-separated in the order written: `tx=1,rx=2` on ESP-IDF, `dev=/dev/ttyUSB0` on Linux. **Reject an unknown key** rather than ignoring it, so a grant that is meaningless on this target fails the launch instead of half-applying.

**A UART port must be exclusive, and not every OS gives that for free.** ESP-IDF does — `uart_driver_install` refuses a port that is already installed. Linux does not: two opens of one tty both succeed, so the Linux backing takes `TIOCEXCL` itself. `PlatformUartConfigure` drains the transmit buffer before applying new line settings, so a reconfiguration cannot truncate a byte already on the wire, then discards the receive buffer, because bytes received under the old settings cannot be decoded under the new ones. It must never substitute the nearest achievable baud rate: that produces a link that looks configured and corrupts data.

**A supervisor that manages firmware needs the `ota` driver in its own launch
config.** It opens `/dev/ota` to read the boot state an update is judged
against — which slot is active, whether the boot is still provisional, and the
digest of a staged image. Without the grant that read fails and the supervisor
falls back to comparing version strings, which cannot separate two builds of
one release. The driver is reserved: a Desired State may not grant it to an
arbitrary wapp, and it reaches the installer only through the launch config the
supervisor mints for it.

**`PlatformOta*` slots are always named `a` and `b`** — the first and second physical app slots (ESP-IDF's `ota_0`/`ota_1`, a boot-root subdirectory on Linux) — so the `/dev/ota` wire text reads the same whatever bootloader backs it. `PlatformOtaRollback` may reboot the board during the call rather than scheduling the revert, so a caller must not assume control returns.

**`PlatformExtramEarlyInit` exists because of allocation order, not laziness.** On a target where PSRAM shares one merged heap with internal RAM, the pool needs a large contiguous block, and any earlier allocation can fragment the region it would come from. Call it as early as boot allows; it is idempotent and harmless where PSRAM is a separate heap, since `PlatformExtramMalloc` lazy-initialises anyway.

**`PlatformSha256*` has no `-ENOSYS` path**, unlike `PlatformEd25519Verify`. `/dev/sha256` has no other digest source, so a target with no crypto peripheral compiles the portable body in `posix/sha256.c` rather than declining the request.

**`PlatformFirmwareDigest` identifies bytes, not source.** Two builds of one source tree share a `version` string; the digest is stamped into the image at build time, so a control plane confirming a firmware update compares this instead.

**A read-only preopen will not create its directory.** `PlatformOpenStateDir` creates a missing directory for a read-write mount, but answers `-ENOENT` for a read-only one — creating a directory only to deny writes to it is incoherent. The returned fd is owned by the VFS layer and closed at `VfsDestroy`.

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
just build                                  # build the configured target
just supervisor-variant wsh && just build   # ...with the wsh debug supervisor
```

- **Threads** — pthreads.
- **Stop mechanism** — cooperative: `PlatformWappStop` sets the WAMR terminate flag and sends `SIGUSR2` to the worker, so a wapp spinning in the interpreter and one blocked in a host call are both reaped promptly.
- **Registry** — a host-filesystem directory (`./registry/`) scanned for `<name>@<version>.wapp` images.
- **TLS** — OpenSSL-backed secure sockets (`T`/`U` socket options).
- **Memory stats** — `mallinfo2`.
- **Firmware update** — A/B slot directories on a filesystem, see below.

### Firmware slots on a host

`/dev/ota` stages into a directory tree the bootloader reads, rooted at
`CONFIG_WANTED_OTA_SLOT_ROOT` (default `/boot`):

```
<root>/
  active_slot.txt      # os_prefix=slot_a/  — the committed slot
  tryboot.txt          # os_prefix=slot_b/  — present only while a trial boot is armed
  slot_a/<image>
  slot_b/<image>       # <image> is CONFIG_WANTED_OTA_IMAGE_NAME, default zImage
```

`begin` opens the inactive slot's image under a `.staging` name and `commit`
renames it into place, so a crash mid-write cannot leave a short image where
the bootloader looks. `commit` then writes `tryboot.txt`; the staged slot does
not become active until `confirm` flips `active_slot.txt`. `rollback` removes
`tryboot.txt`, cancelling a trial that has not been confirmed.

Two limits are worth stating plainly:

- **Nothing here reboots into the staged slot.** Arming the trial boot is the
  engine's part; performing it belongs to the board's bootloader integration.
- **`last_failed_slot` and `boot_attempts` are always empty.** No host
  bootloader reports a failed trial back to the engine, so a caller cannot use
  them to detect a reverted boot on this platform.
- **`pending_digest` is always empty**, matching `PlatformFirmwareDigest`:
  nothing stamps a build-time digest into a host image, so a staged image is
  identified by its version string here.


CMake options of note: `WANTED_PLATFORM` (the platform layer) and `WANTED_DEFCONFIG` (seed the configuration from `configs/<name>_defconfig`). Everything else is Kconfig — see [Build configuration](#build-configuration). TLS is `CONFIG_WANTED_VFS_SOCKET_TLS`: the engine states the intent and the host supplies the backend (OpenSSL here, mbedTLS on NuttX and ESP-IDF), so a host that cannot provide one fails the build rather than silently handing back a binary whose secure sockets are rejected at launch.

## NuttX simulator

A first-class, CI-gated target: the full engine running as a NuttX application on the host-stack simulator. The `platform/nuttx/` layer is complete — every `Platform*` symbol has a working body, with no remaining stubs.

```bash
just nuttx-deps      # init the nuttx + nuttx-apps submodules, link the app package
just target nuttx && just build     # configure + build the sim
just nuttx-selftest  # run the in-WASM selftest suite on the sim
make nuttx-shell     # boot the sim to an interactive wsh prompt (host wrapper)
```

- **Board config** — `sim:wanted`, a native defconfig in the NuttX fork. The engine runs as the NuttX init task via `wanted_sim_main`, which `chdir`s to `/data` (so `./registry` resolves against the sim's hostfs) and powers the board off cleanly when `wanted_main` returns.
- **Console** — raw `write(1/2)` for output.
- **Stop mechanism** — cooperative: `PlatformWappStop` sets the WAMR terminate flag and sends `SIGUSR2` to the worker so a wapp blocked in a host call is interrupted and checks the flag on return. A per-worker `interrupted` flag bridges `clock_nanosleep`'s success-on-signal quirk (Linux reports `EINTR` directly and needs no such bridge).
- **Submodules** — `third_party/nuttx` and `third_party/nuttx-apps` are shallow submodules pinned to the `wanted` branch of the mekops forks; `just nuttx-deps` initialises them (idempotent); `just build` runs it for you on the nuttx target.

**Differences from Linux.** The `sim:wanted` board routes sockets to the host through usrsock (`CONFIG_SIM_NETUSRSOCK`), so `/net` sockets reach the host network; TLS comes from the shared raw-mbedTLS layer (`CONFIG_SYSTEM_WANTED_TLS` selects `apps/crypto/mbedtls` — same no-CA-bundle caveat as ESP-IDF: encrypted but unauthenticated). Neither target's cooperative stop can pre-empt a bare native call that never checks `EINTR`.

## Hardware targets

Two hardware target families live in the tree. The **RP2350** is a **NuttX** target that shares `platform/nuttx/` with the simulator — only the board config, the registry backend, and the entry point differ. The **ESP32 family** (classic ESP32 + ESP32-S3) is a native **ESP-IDF** port under `platform/esp-idf/`.


### RP2350 (NuttX) — reference embedded target

The reference constrained target, and the one the control-plane story is proven on.

- **Core / boards** — ARM Cortex-M33 (RP2350). Two boards carry WANTED configs in the pinned NuttX fork: `adafruit-feather-rp2350:wanted` (the `wsh` debug supervisor) and `pimoroni-pico-2-plus-w:sheriff` (the Sheriff control-plane agent, with the onboard CYW43439 radio). A RISC-V/Hazard3 build of the silicon runs (PSRAM + `ostest` clean) but a WANTED board config for it is a deprioritized stretch.

  ```bash
  make build DEFCONFIG=pimoroni_pico2_plus_w
  make rp2350-flash-swd    # flash over SWD via a Raspberry Pi Debug Probe (no BOOTSEL)
  ```

- **Registry** — a LittleFS volume on a reserved region of the internal QSPI flash (the flash-MTD backend), written through the RP2350 ROM flash routines. Full wapp lifecycle (`create → start → running → stop → exited`) is hardware-verified.
- **PSRAM** — 8 MB external PSRAM on QMI CS1 (GPIO8), merged with internal SRAM into one ~8.5 MiB heap (`RP23XX_PSRAM_HEAP_SINGLE`). The large engine buffers (WAMR linear memory, the wapp image cache) live in PSRAM while worker stacks stay in scarce internal SRAM. Measured ceiling: ~38 concurrent wapps. Because flash program/erase and PSRAM share the QMI hardware, the internal-flash MTD driver cleans the XIP cache and saves/restores the CS1 registers around every flash op — without which a flash write silently corrupts PSRAM.
- **Crypto** — a **real Ed25519 verify**: NuttX's vendored mbedTLS has no Ed25519, so the port vendors `orlp/ed25519` (verify-only) behind `PlatformEd25519Verify`. The ESP-IDF port uses the same vendored backend.
- **Wi-Fi (CYW43439)** — on the Pico Plus 2 W the pinned fork drives the onboard CYW43439 radio: the `wifi` driver is available to wapps, and the `:sheriff` boot path joins Wi-Fi before Sheriff's manager loop starts — SSID and passphrase are read interactively from the console (never baked into firmware), association goes through the NuttX WAPI library with DHCP retry. On a CYW43439 board Sheriff's manager socket is `tcp://`; a board without the radio uses the `serial://` USB-CDC link below. The RP2350 board configs do not enable `CONFIG_SYSTEM_WANTED_TLS` (the mbedTLS layer is proven on the sim; its flash/RAM cost on this board is unmeasured), so the control-plane transport is plain TCP or the wired serial link.
- **Console + flashing** — with the `:sheriff` config the console moves to UART0 (read it over the Debug Probe's UART bridge), which frees the native USB-CDC for the control plane. Flash over SWD with `make rp2350-flash-swd` (no button dance) or over USB in BOOTSEL with `make rp2350-flash`; `make rp2350-reset` resets a running board over SWD.
- **Control plane over USB-CDC** — on a board with no radio, Sheriff reaches a host Deputy over the native USB-CDC using the engine's `serial://` socket scheme (a device path in place of `host:port`). The full reconcile loop runs on real hardware (verified on the Feather RP2350): State Report uplink → Ed25519-verified signed Desired State → wapp `RUNNING`, and a wrongly-signed Desired State is rejected. This is the ecosystem's first genuine (not demo-stubbed) signed-workload verification on embedded hardware.
- **Secure boot** — validated entirely offline via `picotool seal --sign` (`make rp2350-sign`); the one-way OTP `SECURE_BOOT_ENABLE` fuse is deliberately never burned.

### ESP32 family (ESP-IDF)

The whole ESP32 family runs a native ESP-IDF port (`platform/esp-idf/`, `app_main`) — not NuttX — across two chips: the **ESP32-S3** (e.g. S3R8, octal PSRAM, 8 MB flash) and the **classic ESP32** (Waveshare ESP32 One, quad PSRAM, 4 MB flash). The project is multi-chip: `sdkconfig.defaults` holds the chip-independent settings, `sdkconfig.defaults.<chip>` (`esp32`, `esp32s3`) the per-chip PSRAM/console/flash-size differences, applied automatically by chip.

```bash
make build DEFCONFIG=xiao_esp32s3-sheriff       # a specific S3 board
make build DEFCONFIG=esp32-esp-idf              # classic ESP32
make esp32-flash                                # esptool over ESP32_PORT (default /dev/ttyUSB0)
```

Inside the devcontainer or CI (already in a build environment, no host container to pick), the equivalent is `just target esp-idf && just build` for the default S3 board, or `DEFCONFIG=<board> just build` for any other.

`just build` for esp-idf produces `dist/esp-idf/wanted-<chip>-merged.bin` (flashable at offset 0). Runs in the ESP-IDF toolchain image (`docker/Containerfile.esp-idf`), built locally rather than published. `just` must be the container *command*, not an `--entrypoint` override — the base image's entrypoint sources `export.sh`.

- **Threads / stop** — FreeRTOS via the ESP-IDF pthread wrapper; cooperative stop (the WAMR terminate flag aborts the in-flight call).
- **Registry / PSRAM** — flash-backed LittleFS registry (`registry_flash.c`); PSRAM via `extram.c` (8-byte-aligned allocations — WAMR's GC heap requires it).
- **Flash layout** — derived from chip by `platform/esp-idf/board-defconfig.cmake`: S3 gets A/B (`ota_0`/`ota_1`), classic ESP32 a single `factory` app slot, no A/B. The board defconfig (`DEFCONFIG=<name>`, e.g. `xiao_esp32s3-sheriff`) fixes `CONFIG_WANTED_MAX_WAPPS`, which sizes the generated partitions.
- **Registry slot geometry is an on-flash format.** Image bytes sit at `slot * WAPP_IMAGE_SLOT_SIZE` in the raw `wapps` partition, while the record naming that slot lives in a LittleFS index on `persist` — a different partition, which a firmware update does not touch. `CONFIG_WANTED_MAX_WAPPS` and `CONFIG_WANTED_MAX_WAPP_IMAGE_KB` therefore cannot be changed freely: their product can hold the partition size constant while the stride moves, leaving every surviving record pointing at the wrong offset with nothing about the partition table to show for it. Each record stamps the stride it was written under and one naming another reads as absent, so a layout change re-seeds rather than resolving to the wrong bytes. The supervisor is not among the firmware's factory seeds, so a board loading it with `registry:supervisor` falls back to the built-in image after the launch failures instead.
- **Worker stacks** — S3: PSRAM. Classic ESP32: internal DRAM only — a flash op fully disables the classic part's cache, so a PSRAM stack is unreachable during it.
- **OTA** — A/B firmware update through `esp_ota_ops` (`ota.c`) on the S3, with a pending-verify / rollback seam. The classic part's single-factory layout has no A/B slot to roll back to.
- **Secure sockets** — raw mbedTLS with ESP32-S3 hardware AES/SHA/ECC acceleration. No CA bundle is provisioned (`MBEDTLS_SSL_VERIFY_NONE`), so `tcps://` here is encrypted but **unauthenticated** — a demo transport, not production TLS.
- **Crypto** — SHA-256 is hardware-backed; Ed25519 verify uses a vendored portable `orlp/ed25519` backend (`crypto.c`) on both chips.

### Factory-seeded registry images

A board that has never been online still needs wapps in its registry, so an image can be **seeded from the firmware**. The mechanism differs by host but the contract does not: a seed image is written into the writable registry only when its ref is absent there, so an image installed over a seeded ref survives the next boot rather than being overwritten on every start.

On ESP-IDF the seed images are embedded into the app binary at configure time; `platform/esp-idf/registry-seed.sh` packages each `wapps/<name>/<name>.wasm` as `<out>/<name>.wapp`, and `make wapps` builds the inputs. A board whose wapps live in another repository names them, in an optional `platform/esp-idf/board-extras/<defconfig>.cmake` keyed to its own defconfig, as `WANTED_EXTRA_SEEDS` entries of the form `<ref>=<path to .wasm>`; the extern declarations and the seed calls are generated from that list, so `app_main.c` names no board's wapp. `xiao_esp32s3-telegraph-sheriff_defconfig.cmake` is the worked example — it reads `TELEGRAPH_WAPPS` for the directory holding them, and a build without it carries the engine's own fixtures alone. On the NuttX boards the firmware carries them in the read-only ROMFS at `/rom/registry/*.wapp` and the boot shim copies them across on first boot.

The firmware flasher wapp ships this way. It is seeded as `flasher:<supervisor version>` — the version of the supervisor tree it was built from, a bare semver at a tag (`flasher:0.3.3`) or the tag plus a short commit past one (`flasher:0.3.3-abc123`). It installs an engine firmware image and exits, which is why it must be present before any network is: the thing that would otherwise fetch it is what it exists to update. A version too long for a registry version field fails the build rather than being truncated.

### Publishing a firmware image

A built board binary is published to the OCI registry the same way the toolchain images are — `docker/publish-images.sh`, driven by a Containerfile:

```bash
docker/publish-images.sh -b pimoroni_pico2_plus_w -i dist/nuttx/wanted.bin firmware          # build + verify
docker/publish-images.sh -a ~/auth.json -b pimoroni_pico2_plus_w -i dist/nuttx/wanted.bin firmware
docker/publish-images.sh -b pimoroni_pico2_plus_w -i dist/nuttx/wanted.bin -c nowifi firmware
```

The reference is `registry.gitlab.com/mekops/wanted/wanted-engine/firmware/<board>:<version>`, one repository per board. Runners grant no privileged mode, so this runs on a developer machine, not in CI.

`docker/Containerfile.firmware.in` is a template rendered per build, since the version changes every build. Three labels are stamped from the artifact itself:

| Label | Value |
|---|---|
| `version` | The release tag without its leading `v`, which is also the version the engine reports and the tag the image publishes under. |
| `firmware.digest` | `sha256:` plus the hash of the `.bin`. |
| `firmware.size` | The `.bin`'s length in bytes. |
| `firmware.config` | SHA-256 of the `.config` the binary was built from. |

The version is a release tag and nothing else: an untagged or dirty tree stamps itself `X.Y.Z+<timestamp>`, which is not a legal registry tag and would never match what the device reports, so the script refuses to publish one.

### Two builds of one release

A board whose `.config` changed is a different image, but the engine's version comes from `git describe` and so is unchanged. `-c VARIANT` names the configuration and tags the image `<release>-<variant>` — `0.15.0-nowifi` against `0.15.0`. The device compares tags, so two builds of one release must not share one.

The variant must match `[A-Za-z0-9._]+`. A `-` or `+` inside it would move the **release core** — everything before the first `-` — which is what the device converges on and what `verify` re-checks against the release tag. `firmware.config` records which configuration the variant name stands for, so two images tagged `cfg1` can be told apart if they were never the same build.

Publishing a variant is not a substitute for a release. A code change gets a new version; a variant says the code is the same and the configuration is not.

The two firmware labels exist because `podman` always writes the layer as a **gzipped tar**, whose digest measures the archive rather than the image inside it. A control plane reads the labels off the image config to learn the image's own digest and size, and the device unwraps the layer as it flashes. The `verify` step re-hashes the source `.bin` and fails if the label disagrees.

## OpenWrt

Routers, built as an `.ipk` package from an OpenWrt SDK. Unlike NuttX and ESP-IDF, OpenWrt builds the engine as an **external package** — it compiles no engine sources into its own tree and shares no symbol namespace.

```bash
just target openwrt && just build    # -> dist/wanted-engine_<ver>_<arch>.ipk
```

The **OpenWrt SDK** choice picks which SDK cross-builds the package:

| Choice | OpenWrt target | Architecture |
|---|---|---|
| `WANTED_OPENWRT_SDK_AARCH64` | `armsr/armv8` | 64-bit ARM |
| `WANTED_OPENWRT_SDK_MIPSEL` | `malta/le` | 32-bit little-endian MIPS |
| `WANTED_OPENWRT_SDK_CUSTOM` | any — you name it | auto-detected |

**The first two are generic, board-independent SDKs on purpose** — the engine is built for an architecture, not for a particular router.

For any other target, choose **custom** and give `CONFIG_WANTED_TARGET_OPENWRT_SDK_URL` either a URL to an SDK archive (`.tar.zst` / `.tar.xz`, downloaded and cached) or a path to one already extracted:

```bash
just target openwrt
just setconfig WANTED_OPENWRT_SDK_CUSTOM=y
just setconfig 'WANTED_TARGET_OPENWRT_SDK_URL="https://downloads.openwrt.org/releases/24.10.0/targets/ath79/generic/openwrt-sdk-24.10.0-ath79-generic_gcc-13.3.0_musl.Linux-x86_64.tar.zst"'
just build
```

The target, architecture, and toolchain prefix are auto-detected from the SDK's own `staging_dir` layout, so a custom SDK needs nothing else stated — which is why the engine carries no table of OpenWrt targets to fall out of date.

The SDK is downloaded and cached under `.openwrt-sdk/` on first use, and OpenSSL is staged into it once (skipped when TLS is off, which is why the qemu lane below is faster). The cache is extracted with `--no-same-owner`: the tarballs carry the build farm's uid, and preserving it leaves the SDK owned by a user that does not exist locally.

The same SDK toolchain backs the cross-architecture test lane — see [Testing Guide](testing-guide.md#cross-architecture-selftest-openwrt-qemu). Running the suite under qemu is the cheap way to catch faults that are invisible on x86: alignment, calling convention, signal handling.

## Build configuration

The engine is configured through a **Kconfig** tree at the repository root, read by the vendored kconfiglib in `tools/kconfiglib`. Configuring produces `.config` and a generated `wanted-autoconf.h` that every engine source compiles against; there is no second place a value can come from.

Each build directory owns its own `.config`, so a debug build, a cross build, and the extra-drivers lane can differ without fighting each other:

```bash
just menuconfig                  # edit this build dir's configuration
just defconfig small             # seed it from a named envelope
DEFCONFIG=openwrt just build     # seed on first configure, then build
just savedefconfig my_board      # write the minimal diff back
```

`DEFCONFIG` and the `defconfig` / `savedefconfig` recipes name the envelope **without** the `_defconfig` suffix; the file is `configs/<name>_defconfig`.

Editing `.config` re-runs the configure step and regenerates the header, so a changed configuration cannot leave stale values compiled in.

### Target selection

**Which target gets built is configuration, not a choice of recipe.** The `Target` menu selects linux, nuttx, esp-idf or openwrt and carries that target's build inputs; `just build` reads it and dispatches, echoing the target before anything runs.

```bash
just target nuttx                                          # linux | nuttx | esp-idf | openwrt
just setconfig 'WANTED_TARGET_NUTTX_BOARD="sim:wanted"'    # that target's input
just build
```

| Target | Input | Default | Reaches |
|---|---|---|---|
| `linux` | — | — | cmake + ninja |
| `nuttx` | `CONFIG_WANTED_TARGET_NUTTX_BOARD` | `sim:wanted` | NuttX `tools/configure.sh` |
| `esp-idf` | `CONFIG_WANTED_TARGET_ESP_IDF_CHIP` | `esp32s3` | `idf.py set-target` |
| `openwrt` | the **OpenWrt SDK** choice — see below | `aarch64` | the OpenWrt SDK packaging script |

**Boards and SDKs are pass-through strings, never enumerated here.** `sim:wanted` reaches `configure.sh` verbatim and the engine holds no opinion about which boards exist — modelling them would be a second source of truth that drifts from the vendor tree. The same string selects the simulator or real hardware; what differs for hardware is the toolchain container, which the host `Makefile`'s `build` target picks on its own by reading the seeded `.config` (`CONFIG_WANTED_TARGET_ESP_IDF_CHIP` / `CONFIG_WANTED_TARGET_NUTTX_BOARD`) — no per-board `make` target.

Because `.config` is per build directory, two targets can be configured side by side:

```bash
BUILD_DIR=build-mips just target openwrt
BUILD_DIR=build-mips just setconfig WANTED_OPENWRT_SDK_MIPSEL=y
BUILD_DIR=build-mips just build      # .ipk, while build/ stays on linux
```

The cost of configuring rather than naming the target is that a stale `.config` builds something other than what you expected. That is why the target is echoed at the top of every build.

### Selecting the compiled-in launch config

`WANTED_DEFAULT_CONFIG` names the launch config the build embeds — see [Configuration Reference → the compiled-in default](configuration-reference.md). A board defconfig may point this at a **bring-up config**: fixed placeholder identity for early board bring-up, not a specific deployment. `xiao_esp32s3-telegraph-sheriff_defconfig` does this — its own default is `configs/bringup-esp32s3-telegraph-sheriff.json`.

A bring-up config's fields are structurally valid and pass every parser check. A placeholder device ID is a valid string. A placeholder trusted key is 64 valid hex characters. Nothing rejects it at build time.

Before you publish an image for a specific deployed device, override `WANTED_DEFAULT_CONFIG` to that device's own launch config:

```bash
just setconfig 'WANTED_DEFAULT_CONFIG="configs/<device>.json"'
just build
```

Confirm the override took, before you publish the image:

```bash
just _cfg CONFIG_WANTED_DEFAULT_CONFIG
```

Check the built binary for the deployed identity, not the bring-up one:

```bash
strings dist/<target>/wanted-*.bin | grep -E '<device ID>|<manager address>'
```

A device that boots a bring-up config trusts a placeholder key. See [Security Model → device identity and key custody](security-model.md#2-device-identity-and-key-custody) for why a low-order key (the all-zero placeholder is one) does not fail closed: the engine's verifier accepts it, so the device does not refuse a signed Desired State — it trusts one no one else can produce a legitimate signature for.

### How the tree is split

One menu, three files:

```
Kconfig            top level — sources both; what `just menuconfig` opens
├─ Kconfig.target  how to build it: target, board, SDK
└─ Kconfig.engine  what the binary IS — host-agnostic, sourced by hosts
```

`wanted-autoconf.h` is generated from `Kconfig.engine` alone while `.config` spans the whole tree. So an SDK URL or a board string never reaches a translation unit, and no engine source can test a build-orchestration symbol; the build system reads those off `.config`, which it already parses.

The split exists for the hosts. NuttX and ESP-IDF compile engine sources into their own trees and source `Kconfig.engine`, not the top level — they have answered the target question by existing, and asking again would give the build two answers. A `Target` menu in the shared file would have contradicted that.

### General and host-build options

Two menus hold the compilation knobs, split by the same rule as the files:

**General** (`Kconfig.engine`) is for options that change the binary, so every host gets them:

| Symbol | Default | Effect |
|---|---|---|
| `CONFIG_WANTED_DEBUG_TRACES` | `n` | Compiles in the `DEBUG_TRACE()` call sites — VFS routing, wapp lifecycle, driver open/close. Off, they expand to nothing. A development aid, not an operational log: the traces bypass the per-wapp log capture and are verbose enough to change timing on a constrained target. |

**Host build options** (`Kconfig.target`, shown for the linux and openwrt targets) drive CMake and never reach a translation unit — NuttX and ESP-IDF run no CMake of ours and could not honour them:

| Symbol | Default | Effect |
|---|---|---|
| `CONFIG_WANTED_BUILD_COVERAGE` | `n` | `--coverage -O0` across every target, so `just coverage` can report. Applied before any target is defined, so the engine library is instrumented too — not just the test binary. |
| `CONFIG_WANTED_BUILD_STATIC_CLI` | `n` | Links `wanted-cli` with `-static`. Needs a static OpenSSL if secure sockets are on, and glibc still wants its shared NSS modules to resolve hostnames — see the symbol's help. |
| `CONFIG_WANTED_EXTRA_DRIVERS_DIR` | `""` | An out-of-tree driver tree; see [Out-of-tree drivers](#out-of-tree-drivers). |

### Resource limits

The static memory envelope is set here. Every symbol is prefixed `CONFIG_WANTED_` without exception — engine and host symbols reach the same translation unit on NuttX and ESP-IDF, and an unprefixed name is a collision waiting to happen.

| Symbol | Default | Sizes |
|---|---|---|
| `CONFIG_WANTED_MAX_WAPPS` | 3 | concurrent wapp instances (and, via the log slots, the per-wapp log rings) |
| `CONFIG_WANTED_WASM_STACK_SIZE` | 8192 | per-instance operand (interpreter) stack |
| `CONFIG_WANTED_WASM_HEAP_SIZE` | 8192 | per-instance app heap |
| `CONFIG_WANTED_WASM_MAX_MEMORY_PAGES` | 1 | per-instance linear-memory ceiling, in 64 KiB pages (`0` = uncapped) |
| `CONFIG_WANTED_MAX_PATH_LEN` | 256 | VFS path buffers |

Driver allocation sizes that size a static structure are configurable too, under their own submenu: the pipe ring and pipe count, the per-wapp log capacity, and the log mount's open-handle table. Driver *behavioural* knobs (poll intervals and the like) stay local to their driver — the criterion for appearing in the configuration is whether the value sizes an allocation.

### Selectable drivers

Drivers a launch config reaches by name can be deselected, which drops their source, their factory-table row, and their declaration together. `9p` and `inflate` also drag the vendored c9 and uzlib libraries out of the build, so they are the two largest wins. The VFS core — `tarfs`, `devfs`, `netfs`, `procfs`, plus `null`/`log`/`pipe`/`stdio`/`virtual` — is mandatory and carries no symbols.

The hardware and update drivers are core code behind their own symbols, so a target compiles in only what it can serve:

| Symbol | Default | Deselect when |
|---|---|---|
| `CONFIG_WANTED_VFS_GPIO` | off | the target drives no pins, or nothing granted needs them |
| `CONFIG_WANTED_VFS_UART` | off | no wapp is granted a serial port |
| `CONFIG_WANTED_VFS_OTA` | on | the host has no A/B mechanism to update into — the OpenWrt defconfig deselects it, since that host updates through its package manager |
| `CONFIG_WANTED_VFS_SOCKET_LISTEN` | off (on for OpenWrt) | no wapp serves a socket |

A launch config that names a driver the build did not compile in fails the launch rather than resolving to a stub, which is the point of the gate.

### A wapp's memory

Three engine-controlled regions are passed to WAMR per instance:

- **Operand stack** (`WASM_STACK_SIZE`) — the interpreter's evaluation stack, in host memory, **outside** linear memory. Distinct from the wapp's C aux stack, which lives inside linear memory and is fixed by the wapp's own linker (`wasm-ld -z stack-size`).
- **App heap** (`WASM_HEAP_SIZE`) — a host-managed heap for `wasm_runtime_module_malloc`, **outside** linear memory. WAMR disables it when the module exports its own `malloc`/`free`, so a WASI wapp (which allocates from its libc heap at the top of linear memory) usually does not use it.
- **Linear memory** (`WASM_MAX_MEMORY_PAGES`) — the memory the wapp actually addresses: its data, C aux stack, and libc heap. Enforced two ways: WAMR bounds `memory.grow` to the cap at runtime, and the engine refuses at load any image whose declared *initial* memory exceeds it (otherwise WAMR clamps the cap up to the module's initial, letting a large initial bypass the runtime bound). `0` disables both. (A module containing no `memory.grow` is collapsed by WAMR to a single fixed page - `WAMR_BUILD_SHRUNK_MEMORY` flag is on by default.)

### Defconfigs

`configs/` holds two kinds of defconfig. **Envelopes** describe a capacity class; **boards** describe a specific target and additionally pick its supervisor and install paths.

| Envelope | Target class | wapps | stack / heap | linear cap |
|---|---|---|---|---|
| `tiny` | no-PSRAM (ESP32-WROOM, ~180 KB internal RAM) | 2 | 4 KiB / 4 KiB | 1 page |
| `constrained` | ~512 KB RAM, PSRAM (ESP32-WROVER/NuttX) — the defaults | 3 | 8 KiB / 8 KiB | 1 page |
| `small` | routers (128 MB–1 GB) | 16 | 64 KiB / 256 KiB | 16 pages |
| `big` | Linux / cloud | 64 | 128 KiB / 1 MiB | uncapped |

| Board | Host | Notes |
|---|---|---|
| `xiao_esp32s3` | ESP-IDF | ESP32-S3, octal PSRAM, app heap off, linear memory capped at 2 pages so a full house fits the 8 MB part; `-storage` variant trades wapp slots for persist space |
| `xiao_esp32s3-telegraph-sheriff` | ESP-IDF | The same board driving the Telegraph display: the `uart` driver built in for the wapp that brokers the link to the display's MCU, Sheriff as supervisor, wapps from the control plane |
| `esp32-esp-idf` | ESP-IDF | classic ESP32, quad PSRAM, 4 MB flash, single-factory-app layout, internal-RAM worker stacks |
| `openwrt` | OpenWrt | packaged `.ipk`; supervisor read from its install path |

A board defconfig exists only where the board needs something an envelope does not give it — a launch config, an install path, a supervisor choice, or a limit the envelope gets wrong.

| Board | Host | Notes |
|---|---|---|
| `rp2350_feather` | NuttX | Adafruit Feather RP2350, 8 MB PSRAM; PSRAM backs the large allocations, but worker stacks and slot tables still come out of internal SRAM, which is what bounds the wapp count |
| `pimoroni_pico2_plus_w` | NuttX | RP2350 + PSRAM; the same limits as `small`, carried for its launch config |

A defconfig seeds a build directory that has no `.config` yet; it never overwrites an existing one, so a configuration you edited is not silently replaced by a rebuild.

**Keep the target out of a defconfig.** `just savedefconfig` records whatever the build directory holds, including `CONFIG_WANTED_TARGET_*` if you changed it — but an envelope describes a capacity class, not a target, and the two are independent axes. A stray target line is harmless in practice (a host reading `Kconfig.engine` drops it silently, which is the right answer there) but it makes the file claim something it does not mean. Save envelopes from a directory left at the default target, or delete the line.

### Supervisor variant

Which supervisor the engine boots is part of the configuration rather than a build flag:

```bash
just supervisor-variant wsh      # sheriff | wsh | selftest
```

The choice sets the image path. A package that installs the image elsewhere sets `CONFIG_WANTED_SUPERVISOR_IMAGE_PATH` to an absolute path, which wins — that is for where the image *lives*, not which one it is.

### Hosts that build the engine themselves

NuttX and ESP-IDF compile engine sources into their own trees. They read **`Kconfig.engine`**, not the top-level `Kconfig`, and generate the engine's header from an engine `.config`; their own Kconfig files carry only the **edges** that cross into host symbols (`depends on NET`, `select CRYPTO_MBEDTLS`). The engine's own tree must never reference a host symbol: it is also read standalone, and kconfiglib resolves an unknown symbol to `n` with at most a warning — a feature would disappear with nothing to show why.

The host also decides what only it can know. NuttX's `CONFIG_SYSTEM_WANTED_TLS` sets the engine's `WANTED_VFS_SOCKET_TLS` for it, so a board built without mbedTLS cannot meet an engine that assumed TLS was there — rather than each board defconfig restating the answer.

OpenWrt instead builds the engine as an external package and shares no symbol namespace at all. `src/include/wanted-host-guard.h` catches what a preprocessor can there — a self-contradictory configuration, or a build that never ran the Kconfig step. A missing host library still surfaces at link time.

### Measuring the footprint

- `just sizes` reports each defconfig's per-wapp and worst-case memory — plus the build dir's own `.config`, as a final row, so a configuration you have edited is measured alongside the envelopes it derives from. `just sizes current` reports that row alone, and is what `menuconfig`, `defconfig` and `olddefconfig` run once they have written a `.config`, so a limit change is priced as it is made. It also narrows to the ABI the configured target actually builds for, derived from the target half of the tree — an extracted OpenWrt SDK is asked directly for its toolchain triple, and when neither that nor the SDK/board name settles it, both ABIs are shown with the reason. The full survey always shows both, since a defconfig carries no target. Both cover the host (LP64) and 32-bit embedded (ILP32) ABIs, measured from the real engine structs, but it's just approximate value (e.g. wamr overhead is arbitrary worst case value), it doesn't actually measure the whole runtime overhead on specific hardware, using specifc compiler, just the struct sizes.
- `just memcap` is a negative test that verifies the `WASM_MAX_MEMORY_PAGES` cap actually bounds a wapp's `memory.grow`. It builds the engine at a one-page and a four-page cap and drives two wapps over the console: `bigmem` grows past one page and reports whether the runtime growth cap refused it, and `biginit` declares four *initial* pages, which the engine must refuse at load time rather than at growth.

What the report's three figures mean:

| Figure | Definition |
|---|---|
| per-wapp footprint | the per-wapp structs (`wapp_t`, its slot, the VFS/WASI/WAMR contexts, the log ring) plus the WASM stack, the WASM app heap, the worker thread's native stack, one linear-memory page, and an approximate WAMR overhead |
| engine overhead | the fixed boot and config structures (`wantedConfig_t`) |
| worst case | engine overhead + `MAX_WAPPS` × per-wapp footprint |

The struct and limit sizes are exact, measured from the real headers. Two addends are deliberately estimates, so the total is a usable ballpark rather than a floor that ignores the runtime: the linear-memory floor is one 64 KiB page, the minimum an instance reserves, so scale it up for a module declaring N initial pages; and the WAMR overhead is an order-of-magnitude figure for per-instance bookkeeping (module instance, function/global/table instances, exec-env struct) that grows with module complexity. Still excluded: the per-image writable module copy, which is the size of the `.wasm` itself.

## Out-of-tree drivers

A driver written for one deployment — a router's config store, a site-specific sensor bus — does not have to live in this repo to be linked into a target. Point the build at a source tree that supplies `ExtraDriverTable()` and its entries join the launch-config driver names:

```bash
just setconfig 'WANTED_EXTRA_DRIVERS_DIR="/path/to/tree"'
just build
```

The tree's `CMakeLists.txt` defines a library target named `wanted_extra_drivers`; the engine adds its headers to that target. The coupling is source-level — the tree is compiled as part of this build against `vfs-drivers.h`, so there is no binary ABI to keep stable, and no runtime loader. `test/extra-drivers/` is a minimal working example. With the option unset, a default `ExtraDriverTable()` returning `NULL` is compiled in.

Two properties are worth stating plainly:

- **The extra table is searched last** — core names first, then the platform's, then the extra tree's. A tree claiming `wanted`, `socket`, or any other core name cannot shadow it.
- **An extra driver runs at full engine privilege.** Living outside this repo keeps it out of core review; it does not put it outside the trust boundary. A fault in it faults the engine. For a driver that should be isolated instead, run it as a 9P server process and grant the wapp a `9p` mount — `unix://<socket-path>` reaches one on the same box.

## Target quirks worth knowing

Constraints found on real hardware, each of which a port can otherwise rediscover the slow way.

### Stopping a wapp is cooperative everywhere

No target uses `pthread_cancel` — it is unreliable on NuttX and leaves WAMR state half-torn-down elsewhere. `PlatformWappStop` sets the per-instruction terminate flag (`WantedWappTerminate`) and then signals the worker so any host call it is parked in returns early; the interpreter sees the flag at the next instruction boundary, aborts the in-flight WASM call, and the thread unwinds through `WA_threadEnd` on its own stack. That terminates a wapp spinning in pure compute and one blocked indefinitely in I/O alike.

Two details are not obvious. The signal is **`SIGUSR2`**: WAMR reserves `SIGUSR1` for its own blocking-op wakeup and keeps it masked on every wasm thread, so a `SIGUSR1` sent to a worker is never delivered. And on **NuttX the signal wakes a sleep but the sleep still reports success**, so the timer return alone cannot distinguish an interrupt from an elapsed wait — the handler records the interrupt on the worker's own slot and `PlatformClockNanoSleep` consumes it to report `EINTR`. ESP-IDF wires no signal wakeup at all, so a worker blocked in a host call unwinds once that call returns.

### Worker stacks and scheduling

Every platform sets the worker's native C stack **explicitly** from `CONFIG_WANTED_WASM_WORKER_STACK_SIZE`, floored at `PTHREAD_STACK_MIN`, rather than taking the host default: the classic WAMR interpreter is recursive and the WASI/VFS host calls add frames, so an RTOS default (NuttX's `CONFIG_PTHREAD_STACK_DEFAULT` is ~2 KB) overflows the moment real wasm runs, while glibc's 8 MB is wasteful.

The supervisor runs **one scheduling step above** the wapps it manages, so it can always preempt and terminate a runaway. Priorities are set explicitly rather than inherited — a wapp is launched from the supervisor's own elevated thread, and inheriting would lift it to the supervisor's priority and defeat exactly that. A host that forbids real-time scheduling (Linux without `CAP_SYS_NICE`) returns `EPERM`; every thread then falls back to default scheduling, where the host scheduler time-slices anyway.

### The classic ESP32 cannot read flash while a wapp holds PSRAM

On the classic ESP32 an SPI-flash read disables the flash/PSRAM cache globally, and a read issued while another task holds live PSRAM returns corrupt data — LittleFS reports `LFS_ERR_CORRUPT`. This single hardware behaviour shapes three decisions:

- **An in-RAM image cache** (`platform/nuttx/api/registry.c`) reads every registry image into RAM at boot, while only the supervisor is live and flash reads are still safe, and serves every later launch RAM-to-RAM. Masters live for the device lifetime; each launch gets its own copy. An image installed *after* the first launch and started while another wapp runs still falls back to a flash read.
- **A bounce buffer** in `platform/posix/wapps-image.c`, static so it lives in `.bss` and therefore internal RAM: the SPI-flash MTD read target cannot itself be PSRAM, so bytes are read into internal RAM and copied into the (possibly PSRAM) image buffer by the CPU.
- **The registry moves off internal flash** on that board — onto an SD card over a separate SPI peripheral, whose reads never disable the cache. The supervisor image stays in a read-only ROMFS, which is a cache-window read and coherent with PSRAM.

ESP-IDF's `esp_ota_*` calls hit the same class of problem from the other side: the cache-freeze safety check aborts if it observes a PSRAM-stacked caller. Both the OTA path and the flash registry proxy their calls through a dedicated helper thread with an ordinary internal-DRAM stack, so it does not matter which thread invoked the entry point.

### PSRAM alignment and heap fragmentation

Allocations from the classic ESP32's PSRAM heap are made **explicitly 8-byte aligned** (`heap_caps_aligned_alloc`): plain `heap_caps_malloc` guarantees only that heap's block granularity, which is not a multiple of 8, and WAMR's GC allocator requires 8-byte alignment on its heap-struct buffer.

On RP2350 the SRAM and PSRAM share one merged segregated-fit heap, which starts fragmenting the instant any other subsystem allocates — chipping the one giant PSRAM free node into pieces well under 1 MiB before a multi-megabyte probe ever runs. `PlatformExtramEarlyInit` exists to grab the pool before that happens. The ESP32 needs no such call, since its PSRAM is a separate `heap_caps` pool from boot.

### Linux confines a preopen with `openat2`, or not at all

`platform/linux/vfs/vfs-linux.c` opens beneath a preopen with `openat2(RESOLVE_BENEATH)`, which rejects absolute paths, escaping `..`, and — the case a read-only flag cannot close — a symlink inside the host directory pointing outside it. There is deliberately **no plain-`openat` fallback**: on a kernel without `openat2` (< 5.6) the syscall returns `ENOSYS` and the open fails loudly. A sandbox that cannot be enforced must deny, not degrade.

### ESP-IDF filesystem and radio quirks

The `joltwallet/littlefs` port **hard-asserts and aborts the device** when `lfs_file_read_`/`lfs_file_write_` is called on a handle opened without the matching access bit, where a POSIX filesystem would return `EBADF` — so the driver tracks the access mode itself and rejects a mismatch before it reaches the filesystem. Its `fstat` also reads the on-disk directory entry rather than the open handle's live size, so a size check against a handle with unflushed writes sees the last-synced size until something flushes littlefs's write cache.

On the radio, a WPA2/WPA3-transition AP commonly expects a **PMF-capable** client even when it does not require one; leaving `pmf_cfg` zeroed made a real AP reject the very first 802.11 open-auth frame with `AUTH_EXPIRE`, before the WPA2 handshake started. The `/dev/wifi` status read is also a latch **per connection state, not per fd**: a poll loop that writes `connect` once and then only reads must still observe the disconnected→connected transition, which a plain one-shot-per-write latch misses (that shape works on NuttX only because its connect blocks synchronously).

The firmware digest is read straight out of the image descriptor rather than through `esp_app_get_elf_sha256()`, whose RAM copy is sized by `CONFIG_APP_RETRIEVE_LEN_ELF_SHA` — 9 of the 64 hex digits by default — and truncates silently. A truncated prefix compares cleanly and would confirm the wrong image.

`/dev/ota`'s `pending_digest` comes from the same field of the *staged* slot's descriptor, so it is the exact value `/proc/wanted`'s `digest` reports after that image boots. **It is not the digest of the downloaded bytes**: that hashes the `.bin` an updater fetched, this hashes the ELF the image was built from, and comparing one against the other can only ever fail. Reported only while a swap is scheduled — the inactive slot otherwise holds whatever an older or abandoned write left behind.

**Probation is the bootloader's to grant, and OTA never replaces the bootloader.** `esp_ota_set_boot_partition()` writes the staged slot as `ESP_OTA_IMG_NEW`, and it is the bootloader that turns that into `ESP_OTA_IMG_PENDING_VERIFY` on the first boot — but only when it was itself built with `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`. A board first flashed from a build without it keeps that bootloader for life: the option in a later app's `sdkconfig` changes nothing, the staged image comes up `ESP_OTA_IMG_VALID`, and an image that hangs is committed permanently with no way back but USB. Because the state cannot be told apart from a normally confirmed image, the engine does not rely on it: `PlatformOtaCommit` records the slot awaiting its first boot under an NVS key, and the revert timer arms on that record as well as on `PENDING_VERIFY`. Where there is no probation to cancel, the revert sets the boot partition to the displaced slot by hand. The record names the slot, so booting the displaced image never arms a revert against a working slot. **NVS, not RTC memory:** one image writes the record and the next reads it, and a `RTC_NOINIT_ATTR` address is assigned per link — the two builds an update spans need not agree on it, whereas a key does. It also survives power loss, so an image left unconfirmed when power is pulled still gets its revert on the way back up. The seam initialises NVS itself, since `PlatformOtaInit` runs before whoever else would.

Finally, the console VFS is routed through the interrupt-driven driver so `read(stdin)` blocks. With the default non-blocking console, a supervisor shell's `getline()` spins returning nothing and never assembles a command line. The peripheral differs by board — USB-Serial/JTAG on the S3, UART on the classic part — but both need their blocking driver installed.

### TLS is client-side only

Neither the OpenSSL nor the mbedTLS body branches on direction: both unconditionally run a client-mode handshake, so a secure transport cannot serve. `PlatformNetListen` rejects one for want of a server certificate and key. mbedTLS is used raw rather than through ESP-IDF's `esp-tls`, because `esp_tls_conn_new_sync` unconditionally opens and connects its own socket and cannot wrap an fd it did not open. No CA bundle is provisioned — verification is `MBEDTLS_SSL_VERIFY_NONE`, which proves the handshake and record layer and nothing about peer identity.

## Porting to a new platform

1. Create `platform/<name>/`. If the target is POSIX-like, link `platform/posix/` and implement only the deltas (thread/stop model, memory stats, secure sockets if any, registry-backend glue, entry point); otherwise implement every `Platform*` symbol from `platform.h` yourself, with `platform/dummy/` as the model for a from-scratch port. Either way, no stubs.
2. Provide a stop mechanism that **interrupts a blocked host syscall**, not just a cooperative flag.
3. Implement a registry backend (filesystem, flash blob, or in-memory) behind `PlatformRegistry*`.
4. Wire the build (`WANTED_PLATFORM=<name>`) and a board/app entry point.

The NuttX port is the reference for a constrained target. Its findings carry over: keep `wamrData_t` opaque and reach it through the `WantedWappTerminate` accessor; avoid VLAs; avoid `scandir` and GNU extensions (use `opendir`/`readdir`/`qsort`); confirm `CONFIG_INTERPRETERS_WAMR_THREAD_MGR` so `wasm_runtime_terminate` takes effect, and that thread cleanup does not rely on `pthread_cleanup_push`.

## See also

- [Architecture](architecture.md) — where the platform seam sits in the system.
- [Testing Guide](testing-guide.md) — the same suites run on Linux and the NuttX sim.
