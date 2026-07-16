<!-- SPDX-License-Identifier: Apache-2.0 -->
# `platform/` — the porting layer

The engine core (`src/`) is OS-agnostic: every OS primitive it needs is declared
in [`platform/include/platform.h`](include/platform.h) and implemented once per
target here, selected at build time by CMake (and by the NuttX app build). Never
call an OS API from `src/` — go through this contract.

Porting the engine to a new OS/board means implementing `platform.h` for it.

## Existing implementations (worked examples)

| Directory | Target |
|-----------|--------|
| `linux/` | Production Linux — the reference implementation (POSIX + OpenSSL TLS). |
| `esp-idf/` | ESP-IDF for the ESP32 family (LittleFS, `esp_ota_ops`, lwIP, PSRAM). |
| `nuttx/` | Apache NuttX (sim + ESP32 + RP2350 boards). |
| `dummy/` | In-memory stub used only by the unit-test suite (no OS, no hardware). |
| `posix/` | Shared POSIX helpers (fs, socket, clock, mutex, TLS) reused by the Linux/NuttX ports rather than duplicated. |

`posix/` is the first place to look when porting a POSIX-ish target: much of the
contract is already implemented there and only the genuinely target-specific
pieces (registry storage, memory stats, OTA) remain.

## The contract to implement

Group by concern; a minimal bring-up implements the first four groups and stubs
the rest (return `-ENOSYS`) until the feature is needed.

| Group | Functions | Notes |
|-------|-----------|-------|
| **Clock** | `PlatformClockGetRes/GetTime/NanoSleep` | Monotonic + realtime; realtime is calibrated by the control plane. |
| **Threads/locks** | `PlatformMutexNew/Lock/Unlock/Free`, `PlatformWorkerStackSize` | The worker stack must fit the recursive interpreter (RTOS defaults are too small). |
| **Wapp lifecycle** | `PlatformWappLoad/Unload/Start/Stop/Release`, `PlatformWappLoop`, `PlatformWappGetState` | Owns the per-platform static slot table (`MAX_WAPPS`) and the run loop; drives `WantedWappRun`/`Stop`/`Terminate`. |
| **Memory** | `PlatformMemoryStats`, `PlatformExtramMalloc/Realloc/Free/EarlyInit` | Extram routes bookkeeping to external RAM (e.g. PSRAM) when present; stub to the normal heap otherwise. |
| **Drivers/identity** | `PlatformDriverTable`, `PlatformName`, `PlatformSetProcessArgs`, `PlatformRequestShutdown/Reboot` | The driver table exposes board devices (gpio, wifi) to wapps as VFS nodes. |
| **Registry** | `PlatformRegistryRead/Write/Remove/WappLoad/ReadImage` | The image store the supervisor launches from (filesystem, flash partition, or in-memory). |
| **Filesystem** | `PlatformOpenStateDir`, `PlatformVolumeRoot`, `PlatformFsRename`, `PlatformFsMkdir` | Backs `platform`/`volume` mounts and preopens; `posix/fs.c` implements these for POSIX targets. |
| **Network** | `PlatformNetOpen/Connect/Close/Recv/Send/Accept/Shutdown/Free` | The socket seam behind `vfs-socket.c`; TLS is layered on top in `posix/`. |
| **OTA** (optional) | `PlatformOtaInit/Confirm/GetBootState/BeginWrite/Write/Commit/Rollback` | A/B firmware update behind `/dev/ota`; leave stubbed on targets without it. |

## Build wiring

Each target has a `CMakeLists.txt` compiled into `platform_<target>`; the top-level
build selects one. `platform-config.h` holds the target-neutral defaults
(`DEFAULT_ROOT`, `VOLUME_ROOT`). See the [Platform Guide](../docs/platform-guide.md)
for the deployment-level view and the OpenWRT target plan for a router (musl,
aarch64/mips) porting example.
