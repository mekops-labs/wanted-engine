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

## Build and Verification

The development environment is standardized via Podman to ensure toolchain consistency.

### Build

```bash
podman run --rm -v "$PWD:/src:Z" --entrypoint=/bin/sh \
    registry.gitlab.com/wanted-project/wanted-engine/build \
    -c "mkdir -p /src/build && cd /src/build && cmake -G Ninja /src && ninja"
```

### Test

```bash
podman run --rm -v "$PWD:/src:Z" --entrypoint=/bin/sh \
    registry.gitlab.com/wanted-project/wanted-engine/build \
    -c "cd /src/build && ctest --output-on-failure"
```
