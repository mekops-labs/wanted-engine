# WebAssembly Nanocontainer Technology for Embedded Devices

[![pipeline status](https://gitlab.com/wanted-project/wanted-engine/badges/main/pipeline.svg)](https://gitlab.com/wanted-project/wanted-engine/-/commits/main)
[![coverage report](https://gitlab.com/wanted-project/wanted-engine/badges/main/coverage.svg)](https://gitlab.com/wanted-project/wanted-engine/-/commits/main)

> [CHANGELOG](CHANGELOG.md)

- **Interpreter:** Uses [WAMR 2.4.4](https://github.com/bytecodealliance/wasm-micro-runtime) in fast interpreted mode (`WAMR_BUILD_INTERP=1`).
- **Concurrency:** Runs multiple isolated threads (wapps).
- **Isolation:** Strict memory isolation via WebAssembly; external interactions mediated exclusively through the VFS.
- **Mount-table VFS Router:** Plan 9-inspired path routing for devices, network, and process state.
- **Layered TarFS:** OCI-compatible filesystem supporting multiple TAR layers with shadowing and whiteouts.

## Documentation

Full developer and user documentation lives in [`docs/`](docs/):

- [Quick Start](docs/quickstart.md) — build, package, and launch a wapp in ten minutes.
- [Architecture](docs/architecture.md) — the VFS router, wapp model, supervisor, and platform seam.
- [VFS Reference](docs/vfs-reference.md) — every `/dev`, `/net`, `/proc`, and TarFS path.
- [Control Plane Reference](docs/control-plane-reference.md) — the `/dev/wanted` contract: nodes, verbs, state machine.
- [Configuration Reference](docs/configuration-reference.md) — engine JSON config schema.
- [Platform Guide](docs/platform-guide.md) — Linux, the NuttX simulator, and the porting checklist.
- [Testing Guide](docs/testing-guide.md) — the unit, in-WASM selftest, and smoke tiers.

## Architecture

WANTED implements a VFS Router. Path resolution is split into four primary namespaces:

1. **Device Namespace (`/dev/`):** Routes to registered sub-drivers (pipes, stdio, control plane).
2. **Network Namespace (`/net/`):** Routes to the socket driver for TCP/UDP.
3. **Process Namespace (`/proc/`):** Read-only system state and metrics.
4. **Application Space (Root `/`):** **TarFS** merged read-only OCI layers.

```text
WAPP -> WASI -> VFS ROUTER -> [/dev/ | /net/ | /proc/ | TarFS(/)]
```

See [Architecture](docs/architecture.md) for the full conceptual overview.

## Build and Run

The environment is standardized via Podman/Docker. The root `Makefile` wraps all containerized commands.

```bash
make build           # engine + sheriff supervisor
make wsh             # engine + wsh debug supervisor
make test            # run unit tests
make selftest        # run in-WASM functional suite
```

See the [Quick Start](docs/quickstart.md) and [Testing Guide](docs/testing-guide.md) for details.

## NuttX simulator

WANTED runs as a first-class NuttX application.

```bash
make nuttx-deps      # init submodules
make nuttx-build     # build the sim
make nuttx-selftest  # run the suite on the sim
```

See the [Platform Guide](docs/platform-guide.md) for sim usage and the hardware roadmap.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for code style and commit conventions.

## License

WANTED Engine is licensed under the [Apache License, Version 2.0](LICENSE).
Bundled third-party components retain their own licenses — see [NOTICE](NOTICE).
