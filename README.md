# Web Assembly Nanocontainer Technology for Embedded Devices

[![pipeline status](https://gitlab.com/wanted-project/wanted-engine/badges/main/pipeline.svg)](https://gitlab.com/wanted-project/wanted-engine/-/commits/main)
[![coverage report](https://gitlab.com/wanted-project/wanted-engine/badges/main/coverage.svg)](https://gitlab.com/wanted-project/wanted-engine/-/commits/main)

> [CHANGELOG](CHANGELOG.md)

- **Interpreter:** Uses [WAMR 2.4.4](https://github.com/bytecodealliance/wasm-micro-runtime) (WebAssembly Micro Runtime) in classic interpreted mode (`WAMR_BUILD_INTERP=1`, `WAMR_BUILD_AOT=0`).
- **Concurrency:** Runs multiple [wapps](#wapp-overview) simultaneously as isolated threads.
- **Isolation:** Strict memory isolation via WebAssembly; all external interactions are mediated exclusively through the VFS.
- **Mount-table VFS Router:** Path normalization (`..`, `.`, double-slash), typed FD table, and a mount table routing `/dev/`, `/net/`, `/proc/`, and `/` independently.
- **Layered TarFS:** OCI-compatible filesystem supporting multiple TAR layers with shadowing and whiteout (`.wh.`) semantics.
- **Efficient Indexing:** Zero-copy filesystem indexing in memory with O(log N) lookup performance and boot-time pre-fetching.

## General architecture

WANTED implements a Cloud-Native VFS Router. Path resolution is split into four primary namespaces:

1. **Device Namespace (`/dev/`):** Routes to registered sub-drivers — `null`, `pipe/<name>`, `stdin`, `stdout`, `stderr`, `platform`, `wanted`, and any wapp-configured drivers.
2. **Network Namespace (`/net/`):** Routes to the socket driver for TCP/UDP operations.
3. **Process Namespace (`/proc/`):** Read-only flat namespace exposing system state (`wapps`, `memory`). Privileged entries are hidden when `system.privileged` is false.
4. **Application Space (Root `/`):** Handled by **TarFS**, which merges up to 4 OCI layers into a single unified view.

```text
WAPP -> WASI -> VFS ROUTER -> [/dev/ | /net/ | /proc/ | TarFS(/)]
```

## Wapp overview

A Wapp is a collection of OCI-compatible ustar TAR layers.

1. **Required Files:** At least the following must exist in the layer stack:
    - `app.wasm`: The WebAssembly application binary.
    - `manifest.json`: Application metadata.

2. **Manifest Schema:**
    ```json
    { "name": "my-app", "version": [1, 2, 3], "package": 0, "requirements": ["console", "network_tcp"] }
    ```
    - `name`: Unique application identifier.
    - `version`: Integer array `[major, minor, patch]`.
    - `package`: Integer package revision.
    - `requirements` *(optional)*: Array of abstract capability names the wapp needs. The engine parses and stores the list; the Sheriff supervisor validates requirements against its capability registry before issuing a start action.

3. **Layering:** Supports OCI-style overlays. Newer layers shadow files in older layers. Whiteout files (`.wh.<filename>`) delete files from underlying layers.

## Built-in VFS devices

The following devices are always registered in every wapp's `/dev/` namespace, regardless of wapp configuration:

| Path | Purpose |
|------|---------|
| `/dev/null` | Reads return 0; writes are no-ops |
| `/dev/pipe/<name>` | Named pipe IPC; 4096-byte ring buffer; up to 8 concurrent pipes |
| `/dev/stdin` | Stub; reads return EOF |
| `/dev/stdout` | Stub; writes are no-ops |
| `/dev/stderr` | Stub; writes are no-ops |

## Supervisor

WANTED boots a privileged wapp called the **supervisor** (Sheriff role). The supervisor image is a standard ustar TAR archive bundling `app.wasm` and `manifest.json`, loaded at runtime via `PlatformWappLoad`. It is **not** compiled into the binary.

Two variants ship under `wasm/supervisor/`:

| Variant | Path | Purpose |
|---|---|---|
| `sheriff` | `wasm/supervisor/sheriff/` | Production control-plane agent |
| `wsh` | `wasm/supervisor/wsh/` | Debug shell for interactive inspection |

Build both TAR images before running:

```bash
make -C wasm/supervisor
```

The image path is resolved in priority order:

1. **`supervisor.imagePath` in the JSON config** — runtime override, no recompile needed.
2. **`WANTED_SUPERVISOR_IMAGE_PATH` CMake option** — compile-time override, defaults to `./wasm/supervisor/sheriff/supervisor.tar`.

To select the `wsh` debug supervisor at compile time:

```bash
cmake -DWANTED_SUPERVISOR_IMAGE_PATH=../wasm/supervisor/wsh/supervisor.tar ..
```

## Running

```bash
./build/cmd/wanted-cli                           # run with built-in default config
./build/cmd/wanted-cli docs/example_config.json  # run with explicit config file
```

The config file is JSON. Relevant top-level fields:

- **`system.privileged`** — boolean; enables privileged `/proc` entries (wapp state, memory stats). Defaults to `false`.
- **`supervisor.imagePath`** — path to the supervisor TAR image; overrides the compiled-in default when set.
- **`supervisor.params`** — driver and console settings; if absent, compiled-in defaults apply (TCP socket at `localhost:8888`, TLS socket at `localhost:8889`, 9P at `localhost:5640`).

See [`docs/example_config.json`](docs/example_config.json) for a fully annotated example.

## Build and Verification

The development environment is standardized via Podman to ensure toolchain consistency. A devcontainer configuration (`.devcontainer/`) is also provided for VS Code and JetBrains remote development.

### Build

```bash
# 1. Build supervisor TAR images
make -C wasm/supervisor

# 2. Build the engine
podman run --rm -v "$PWD:/src:Z" --entrypoint=/bin/sh \
    registry.gitlab.com/wanted-project/wanted-engine/build \
    -c "mkdir -p /src/build && cd /src/build && cmake -G Ninja /src && ninja"
```

To use the `wsh` debug supervisor instead of `sheriff`:

```bash
podman run --rm -v "$PWD:/src:Z" --entrypoint=/bin/sh \
    registry.gitlab.com/wanted-project/wanted-engine/build \
    -c "mkdir -p /src/build && cd /src/build && \
        cmake -G Ninja /src \
              -DWANTED_SUPERVISOR_IMAGE_PATH=../wasm/supervisor/wsh/supervisor.tar \
        && ninja"
```

### Test

```bash
# Unit tests (20 tests via ctest)
podman run --rm -v "$PWD:/src:Z" --entrypoint=/bin/sh \
    registry.gitlab.com/wanted-project/wanted-engine/build \
    -c "cd /src/build && ctest --output-on-failure"

# Smoke tests (16 end-to-end tests via wsh)
podman run --rm -v "$PWD:/src:Z" --entrypoint=/bin/sh \
    registry.gitlab.com/wanted-project/wanted-engine/build \
    -c "cd /src && test/smoke.sh"
```
