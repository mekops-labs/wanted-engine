# Web Assembly Nanocontainer Technology for Embedded Devices

[![pipeline status](https://gitlab.com/wanted-project/wanted-engine/badges/main/pipeline.svg)](https://gitlab.com/wanted-project/wanted-engine/-/commits/main)
[![coverage report](https://gitlab.com/wanted-project/wanted-engine/badges/main/coverage.svg)](https://gitlab.com/wanted-project/wanted-engine/-/commits/main)

> [CHANGELOG](CHANGELOG.md)

- **Interpreter:** Uses `wasm3` as the high-performance WebAssembly interpreter.
- **Concurrency:** Runs multiple [wapps](#wapp-overview) simultaneously as isolated threads.
- **Isolation:** Strict memory isolation via WebAssembly; external interactions are mediated exclusively through the VFS.
- **Stateless Prefix Router:** High-speed native routing for `/dev/` and `/net/` namespaces.
- **Layered TarFS:** OCI-compatible filesystem supporting multiple TAR layers with shadowing and whiteout (`.wh.`) semantics.
- **Efficient Indexing:** Zero-copy filesystem indexing in memory with O(log N) lookup performance and boot-time pre-fetching.

## General architecture

WANTED implements a Cloud-Native VFS Router. Path resolution is split into three primary domains:

1. **Device Namespace (`/dev/`):** Routes to platform-specific and virtual drivers (e.g., `9p`, `config`, `platform`, `null`).
2. **Network Namespace (`/net/`):** Routes to the socket driver for TCP/UDP operations.
3. **Application Space (Root):** Handled by **TarFS**, which merges up to 4 OCI layers into a single unified view.

```text
WAPP -> WASI -> VFS ROUTER -> [/dev/ | /net/ | TarFS]
```

## Wapp overview

A Wapp is a collection of OCI-compatible TAR layers.

1. **Required Files:** At least the following must exist in the layer stack:
    - `app.wasm`: The WebAssembly application binary.
    - `manifest.json`: Application metadata and requirement definitions.

2. **Manifest Schema:** A JSON file containing:
    - `name`: Unique application identifier.
    - `version`: SemVer-compatible version string.
    - `chksum`: SHA256 of the WASM binary.
    - `drivers`: Array of required capabilities.

3. **Layering:** Supports OCI-style overlays. Newer layers shadow files in older layers. Whiteout files (`.wh.<filename>`) can be used to "delete" files from underlying layers.

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

The default image path is `./wasm/supervisor/sheriff/supervisor.tar`, resolvable relative to the working directory of the process. Override at CMake configure time:

```bash
cmake -DWANTED_SUPERVISOR_IMAGE_PATH=../wasm/supervisor/wsh/supervisor.tar ..
```

## Running

```bash
./build/cmd/wanted                        # run with built-in default config
./build/cmd/wanted docs/example_config.json  # run with explicit config file
```

The config file is JSON. The `supervisor` block overrides driver and console settings for the supervisor wapp; if absent, the compiled-in defaults apply (TCP socket at `localhost:8888`, TLS socket at `localhost:8889`, 9P at `localhost:5640`).

See [`docs/example_config.json`](docs/example_config.json) for a fully annotated example.

## Build and Verification

The development environment is standardized via Podman to ensure toolchain consistency.

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
podman run --rm -v "$PWD:/src:Z" --entrypoint=/bin/sh \
    registry.gitlab.com/wanted-project/wanted-engine/build \
    -c "cd /src/build && ctest --output-on-failure"
```
