# WANTED Engine — AI Agent Instructions

WANTED (Web Assembly Nanocontainer Technology for Embedded Devices) is a Cloud-Native VFS Router written in C that executes WebAssembly applications ("wapps") in strict memory isolation on Linux and embedded targets (NuttX/ESP32). It uses WAMR 2.4.4 (WebAssembly Micro Runtime) in classic interpreted mode, a stateless prefix-based VFS router, and OCI-compatible TarFS layering.

Public API surface is a single function: `int WantedStart(const char *cfg, size_t cfgLen)` in `include/wanted.h`.

## First Steps

Before executing any build or test command, read `README.md`. It is the authoritative source for build instructions, test procedures, and run commands. Do not guess or reconstruct commands from memory.

## Build Environment

All builds run inside the standardized build container — do **not** build natively. The root `Makefile` wraps the containerized commands (run `make help` for the full list); the container provides the toolchain (CMake, Ninja, host clang/gcc, and the bundled wasi-sdk for wapps).

```bash
make build         # supervisor TAR images + engine (sheriff supervisor)
make wsh           # engine with the wsh debug supervisor
make test          # unit + smoke suite via ctest
make smoke         # VFS/control-plane smoke via wsh
make shell         # interactive shell in the build container
```

Override the container runtime or image with `RUNNER=docker` / `IMAGE=...`.

**Run a single test group** (from `make shell` or any container shell):

```bash
cd build && ctest -R test-tarfs --output-on-failure
```

### Key CMake Options

| Option | Default | Effect |
|---|---|---|
| `-DBUILD_EXECUTABLE=ON` | ON | Build standalone CLI (`cmd/wanted-cli`) |
| `-DBUILD_TESTING=ON` | ON | Build unit test suite |
| `-DWANTED_DEBUG_TRACES=ON` | OFF | Verbose WANTED engine output |
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
- `wanted_wasm_api.c` — WAMR NativeSymbol registration for the `wanted` host module
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
- `vfs-wanted.c`, `vfs-wanted-config.c`, `vfs-wanted-wapps.c`, `vfs-wanted-registry.c` — supervisor control-plane drivers (`vfs-wanted-wapps.c` is the per-wapp `wapps/` namespace plus the root `ctl` node)

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

#### `platform/nuttx/`

NuttX target stub — skeleton for the embedded port. Not yet functional; exists to validate the platform boundary contract without ESP-IDF.

#### `platform/dummy/`

Stub platform used exclusively by the unit test suite. Provides no-op or minimal implementations so tests can run without hardware.

### `wasm/`

WebAssembly toolchain, test apps, and supervisor variants.

- `Makefile` — compiles `.c` → `.wasm` → `.wasm.h` (C header via `xxd`); targets: `NAME=<file>` with optional `WASI=1`
- `build_all.sh` — rebuilds all WASM targets
- `wanted_libc.h` — host function import declarations for bare (non-WASI) wapps
- `test_prog.c`, `test_wasi.c`, `test_wasi_read_sleep.c` — minimal test apps

#### `wasm/supervisor/`

Two supervisor variants, each bundled into `supervisor.tar` from `app.wasm` + `manifest.json`.

| Variant | Purpose | `app.wasm` source |
|---|---|---|
| `sheriff/` | Production control-plane agent — default | prebuilt blob (separate repo) |
| `wsh/` | Interactive debug shell | compiled from `wapps/wsh/` |

Rebuild after any supervisor source change (compiles `wsh` from `wapps/wsh/`,
re-tars both variants):

```bash
make -C wasm/supervisor
```

`wapps/wsh/` holds the wsh shell source (built with `/opt/wasi-sdk/bin/clang`); its `start`/`stop`/`status` builtins exercise the `/dev/wanted` control plane.

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
| `wamr` | WebAssembly Micro Runtime 2.4.4 (core interpreter) |
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

## C Standards Compliance

All code in this repository must conform to the **ISO C standard** (C99 or later). Compiler-specific and GNU extensions are forbidden.

- **No GNU extensions.** Do not use `__attribute__`, `__typeof__`, statement expressions, or any other GCC/Clang extension.
- **No compiler-specific built-ins.** `__builtin_*` functions are forbidden; use standard library equivalents or explicit implementations.
- **No non-standard library functions.** Functions that require `_GNU_SOURCE`, `_POSIX_C_SOURCE`, or any other feature-test macro are forbidden. Use only functions guaranteed by the C standard (e.g. use `memcmp` in a loop instead of `memmem`).
- **No VLAs.** Variable-length arrays are absent on many embedded toolchains; use fixed-size arrays or explicit allocation instead.

This applies to all of `src/`, `platform/`, `cmd/`, and `test/`. Vendor code under `vendor/` is exempt.

## Architecture Constraints

- **Platform boundary is strict.** `src/` must not call OS primitives directly. All platform-specific operations go through `platform/include/` headers.
- **VFS is the only I/O path.** Wapps interact with the outside world exclusively through VFS. Adding direct syscalls from WASM host functions bypasses isolation and is not allowed.
- **Fixed resource limits.** MAX_WAPPS=3, WAPP_MAX_NAME_LEN=15, MAX_PATH_LEN=256, WASM_STACK_SIZE=8192, WASM_HEAP_SIZE=8192. Changes require audit of all array-sized structures.
- **No dynamic allocation in wapp context after init.** Memory budget is constrained on embedded targets.

## Key Constants and Status Codes

Defined in `src/include/wanted-api.h`:

- Wapp states: `NOT_STARTED`, `STARTING`, `RUNNING`, `EXITED`, `FAILURE`
- `MAX_WAPPS` = 3
- `WAPP_MAX_NAME_LEN` = 15
- `MAX_PATH_LEN` = 256
- `WASM_STACK_SIZE` = 8192, `WASM_HEAP_SIZE` = 8192

## CI/CD

GitLab CI (`.gitlab-ci.yml`) runs two compilers (gcc, clang) and a coverage build. The build image is `registry.gitlab.com/wanted-project/wanted-engine/build`. Test reports are JUnit XML; coverage is Cobertura XML.

## Git Conventions

- **Commit message format:** `{area}: {title}\n\n{bullet-pointed description}`
  - Areas: `feat`, `fix`, `refactor`, `test`, `docs`, `build`, `platform`
  - Example: `feat(vfs): add /dev/pipe named pipe support`
- **One logical change per commit.** Do not mix unrelated areas in the same commit.
- **No `Co-Authored-By` trailers** in commit messages.
- **Never skip CI hooks** (`--no-verify`) or force-push `main`.
