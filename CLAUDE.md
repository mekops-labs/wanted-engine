# WANTED Engine — AI Agent Instructions

WANTED (Web Assembly Nanocontainer Technology for Embedded Devices) is a Cloud-Native VFS Router written in C that executes WebAssembly applications ("wapps") in strict memory isolation on Linux and embedded targets (ESP32). It uses the `wasm3` interpreter, a stateless prefix-based VFS router, and OCI-compatible TarFS layering.

Public API surface is a single function: `int WantedStart(const char *cfg, size_t cfgLen)` in `include/wanted.h`.

## Build Environment

All builds run inside the standardized Podman container. Do **not** build natively — the toolchain requires WASI SDK v16, clang 14, CMake, and Ninja.

**Interactive shell (for iterative work):**

```bash
./run-podman.sh
```

**Full build (supervisor TAR images + engine):**

```bash
# Step 1: build supervisor TAR images (required before cmake)
make -C wasm/supervisor

# Step 2: build the engine
podman run --rm -v "$PWD:/src:Z" --entrypoint=/bin/sh \
    registry.gitlab.com/wanted-project/wanted-engine/build \
    -c "mkdir -p /src/build && cd /src/build && cmake -G Ninja /src && ninja"
```

**Build with debug supervisor (`wsh`) instead of production (`sheriff`):**

```bash
podman run --rm -v "$PWD:/src:Z" --entrypoint=/bin/sh \
    registry.gitlab.com/wanted-project/wanted-engine/build \
    -c "mkdir -p /src/build && cd /src/build && \
        cmake -G Ninja /src \
              -DWANTED_SUPERVISOR_IMAGE_PATH=../wasm/supervisor/wsh/supervisor.tar \
        && ninja"
```

**Run unit tests:**

```bash
podman run --rm -v "$PWD:/src:Z" --entrypoint=/bin/sh \
    registry.gitlab.com/wanted-project/wanted-engine/build \
    -c "cd /src/build && ctest --output-on-failure"
```

**Run a single test group:**

```bash
# Inside the container or after build:
cd build && ctest -R test-tarfs --output-on-failure
```

### Key CMake Options

| Option | Default | Effect |
|---|---|---|
| `-DBUILD_EXECUTABLE=ON` | ON | Build standalone CLI (`cmd/wanted-cli`) |
| `-DBUILD_TESTING=ON` | ON | Build unit test suite |
| `-DWANTED_DEBUG_TRACES=ON` | OFF | Verbose WANTED engine output |
| `-DM3_DEBUG_TRACES=ON` | OFF | Verbose wasm3 interpreter output |
| `-DENABLE_CODE_COVERAGE=ON` | OFF | Gcovr coverage instrumentation |
| `-DSECURE_SOCKETS=ON` | auto | OpenSSL TLS support |
| `-DWANTED_SUPERVISOR_IMAGE_PATH=<path>` | sheriff tar | Compiled-in supervisor image path |

## Running

```bash
./build/cmd/wanted-cli                           # built-in default config
./build/cmd/wanted-cli docs/example_config.json  # explicit config file
```

## Directory Structure

### `include/`

Single public header: `wanted.h`. Exposes only `WantedStart()`. This is the integration boundary — anything consuming WANTED as a library links against this.

### `src/`

Platform-independent core. All files here must remain portable (no Linux-specific syscalls).

- `wanted.c` — engine initialization, supervisor loading, wapp lifecycle orchestration
- `wanted_malloc.c` — memory allocation wrapper (single place to swap allocators)
- `wanted_wasm_api.c` — wasm3 M3 environment setup and WASM host function bindings
- `wanted-vfs-api.c` — VFS calls exposed to the WASM/WASI layer
- `default_supervisor_cfg.json.h` — compiled-in default config (generated header)
- `include/` — internal headers; `wanted-api.h` is the primary internal API surface

#### `src/vfs/`

The VFS router. Routes WASI syscalls to the correct driver via prefix matching:

- `vfs.c` — core router: `open`, `close`, `read`, `write`, `stat`, `seek`, `readdir`
- `vfs-tarfs.c` — OCI layer merging; supports shadowing and `.wh.` whiteout semantics; O(log N) lookup with zero-copy indexing
- `vfs-devfs.c` — `/dev/` dispatcher
- `vfs-netfs.c` — `/net/` dispatcher (TCP/UDP sockets)
- `vfs-virtual.c` — platform-independent virtual drivers (`/dev/null`, `/dev/config`, etc.)
- `vfs-socket.c` — raw socket I/O
- `vfs-9p.c` — 9P protocol driver (host communication)
- `vfs-wanted.c`, `vfs-wanted-config.c`, `vfs-wanted-ctrl.c`, `vfs-wanted-registry.c` — supervisor control-plane drivers

When adding a new VFS driver: implement the driver in `vfs/`, register it in `vfs-devfs.c` or `vfs-netfs.c`, add the header to `src/include/`.

#### `src/wasi/`

WASI-to-VFS bridge. `wasi-vfs.c` translates WASI syscall numbers into VFS router calls. This is the only translation layer — do not add platform logic here.

### `cmd/`

CLI entry point only. `main.c` loads a JSON config file (or uses the compiled-in default) and calls `WantedStart()`. Keep this thin — no business logic belongs here.

### `platform/`

Platform-specific implementations. Swapped at build time via CMake. Never call platform functions directly from `src/` — go through the declared platform API in `platform/include/`.

#### `platform/linux/`

Production Linux target. Implements:
- `api/socket.c` / `api/ssocket.c` — TCP/UDP and TLS (OpenSSL) sockets
- `api/wapps.c` — wapp loading and thread lifecycle
- `api/clock.c`, `api/random.c`, `api/registry.c` — system primitives
- `vfs/vfs-linux.c` — Linux VFS integration

#### `platform/esp-idf/`

ESP32 target (FreeRTOS). Contains its own `CMakeLists.txt` structured as an IDF component. See `platform/esp-idf/README.md` for ESP-IDF build instructions. Flash partition layout is in `partition_table.csv`.

#### `platform/dummy/`

Stub platform used exclusively by the unit test suite. Provides no-op or minimal implementations so tests can run without hardware.

### `wasm/`

WebAssembly toolchain, test apps, and supervisor variants.

- `Makefile` — compiles `.c` → `.wasm` → `.wasm.h` (C header via `xxd`); targets: `NAME=<file>` with optional `WASI=1`
- `build_all.sh` — rebuilds all WASM targets
- `wasm3_libc.h` — libc stubs for bare (non-WASI) WASM apps
- `test_prog.c`, `test_wasi.c`, `test_wasi_read_sleep.c` — minimal test apps

#### `wasm/supervisor/`

Two supervisor variants. Each contains `app.wasm` + `manifest.json` + generated `supervisor.tar`.

| Variant | Purpose |
|---|---|
| `sheriff/` | Production control-plane agent — default |
| `wsh/` | Interactive debug shell |

Rebuild after any supervisor source change:

```bash
make -C wasm/supervisor
```

### `test/`

Unity-based unit test suite. Each `test-*.c` maps to a CTest test group.

- `test-tarfs.c` — TarFS layer merging, shadowing, whiteout
- `test-vfs.c` — VFS router path resolution
- `test-vfs_virtual.c` — virtual device drivers
- `test-api.c` — WANTED core API
- `test-wanted-vfs-api.c` — VFS API

Tests use `platform/dummy/` — no hardware or network required. `generate_runner.sh` creates Unity test runners; do not edit generated files.

### `vendor/`

All external dependencies as git submodules. Do not modify these directly.

| Submodule | Purpose |
|---|---|
| `wasm3` | WebAssembly interpreter (core runtime) |
| `tiny-json` | JSON parsing |
| `json-maker` | JSON generation |
| `cwalk` | Cross-platform path manipulation |
| `romfs-lib` | ROM filesystem |
| `c9` | Internal utility library |

After cloning: `git submodule update --init --recursive`

### `docs/`

- `example_config.json` — fully annotated runtime config; use as reference for config schema
- `secure_socket_test.md` — TLS/secure socket testing procedure

### `docker/`

Dockerfile and entry point for the build image. The image is published to the GitLab registry. Rebuild only when changing toolchain versions.

### `cmake/`

CMake helper modules. `VersionFromGit.cmake` derives version from git tags; `CodeCoverage.cmake` wires gcovr into the build.

## Architecture Constraints

- **Platform boundary is strict.** `src/` must not call OS primitives directly. All platform-specific operations go through `platform/include/` headers.
- **VFS is the only I/O path.** Wapps interact with the outside world exclusively through VFS. Adding direct syscalls from WASM host functions bypasses isolation and is not allowed.
- **Fixed resource limits.** MAX_WAPPS=3, WAPP_MAX_NAME_LEN=15, MAX_PATH_LEN=256, M3_STACK_SIZE=8192. Changes require audit of all array-sized structures.
- **No dynamic allocation in wapp context after init.** Memory budget is constrained on embedded targets.

## Key Constants and Status Codes

Defined in `src/include/wanted-api.h`:

- Wapp states: `NOT_STARTED`, `STARTING`, `RUNNING`, `EXITED`, `FAILURE`
- `MAX_WAPPS` = 3
- `WAPP_MAX_NAME_LEN` = 15
- `MAX_PATH_LEN` = 256
- `M3_STACK_SIZE` = 8192

## CI/CD

GitLab CI (`.gitlab-ci.yml`) runs two compilers (gcc, clang) and a coverage build. The build image is `registry.gitlab.com/wanted-project/wanted-engine/build`. Test reports are JUnit XML; coverage is Cobertura XML.
