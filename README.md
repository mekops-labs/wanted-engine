# WebAssembly Nanocontainer Technology for Embedded Devices

[![pipeline status](https://gitlab.com/mekops/wanted/wanted-engine/badges/main/pipeline.svg)](https://gitlab.com/mekops/wanted/wanted-engine/-/commits/main)
[![coverage report](https://gitlab.com/mekops/wanted/wanted-engine/badges/main/coverage.svg)](https://gitlab.com/mekops/wanted/wanted-engine/-/commits/main)

> [CHANGELOG](CHANGELOG.md)

- **Interpreter:** Uses [WAMR 2.4.4](https://github.com/bytecodealliance/wasm-micro-runtime) in classic interpreted mode (`WAMR_BUILD_INTERP=1`, no AOT/JIT).
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

The environment is standardized via Podman/Docker. Commands are [`just`](https://just.systems) recipes that run inside the build container (`just --list` shows them all). On a bare host the root `Makefile` is a thin wrapper that runs the same recipe in the container — `make build` is just `just build` in the image — so either works. Inside the devcontainer or CI, call `just` directly.

```bash
just menuconfig      # configure this build dir (Kconfig; optional)
just build           # build whatever target is configured (default: linux)
just supervisor-variant wsh && just build   # swap in the wsh debug supervisor
just test            # run unit tests
just selftest        # run in-WASM functional suite
```

Configuration is a Kconfig tree; an unconfigured build directory takes the
defaults, so `just build` works with no configure step. `configs/` holds
capacity envelopes and per-board defconfigs — `DEFCONFIG=small just build`
seeds one. See the [Platform Guide](docs/platform-guide.md).

**Which target gets built is configuration too.** The `Target` menu selects
linux, nuttx, esp-idf or openwrt along with that target's board or SDK, and
`just build` dispatches on it — so there is no build recipe per target and
architecture. Each build directory carries its own `.config`, so two targets
can be configured side by side:

```bash
BUILD_DIR=build-mips just target openwrt
BUILD_DIR=build-mips just setconfig 'WANTED_TARGET_OPENWRT_SDK="mipsel"'
BUILD_DIR=build-mips just build      # .ipk, while build/ stays on linux
```

See the [Quick Start](docs/quickstart.md) and [Testing Guide](docs/testing-guide.md) for details.

## NuttX simulator

WANTED runs as a first-class NuttX application.

```bash
just nuttx-deps      # init submodules
just target nuttx && just build     # build the sim
just nuttx-selftest  # run the suite on the sim
```

See the [Platform Guide](docs/platform-guide.md) for sim usage and the hardware roadmap.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for code style and commit conventions.

## License

WANTED Engine is licensed under the [Apache License, Version 2.0](LICENSE).
Bundled third-party components retain their own licenses — see [NOTICE](NOTICE).
