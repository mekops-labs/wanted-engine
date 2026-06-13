# WebAssembly Nanocontainer Technology for Embedded Devices

[![pipeline status](https://gitlab.com/wanted-project/wanted-engine/badges/main/pipeline.svg)](https://gitlab.com/wanted-project/wanted-engine/-/commits/main)
[![coverage report](https://gitlab.com/wanted-project/wanted-engine/badges/main/coverage.svg)](https://gitlab.com/wanted-project/wanted-engine/-/commits/main)

> [CHANGELOG](CHANGELOG.md)

- **Interpreter:** Uses [WAMR 2.4.4](https://github.com/bytecodealliance/wasm-micro-runtime) (WebAssembly Micro Runtime) in fast interpreted mode (`WAMR_BUILD_INTERP=1`, `WAMR_BUILD_FAST_INTERP=1`, `WAMR_BUILD_AOT=0`).
- **Concurrency:** Runs multiple [wapps](#wapp-overview) simultaneously as isolated threads.
- **Isolation:** Strict memory isolation via WebAssembly; all external interactions are mediated exclusively through the VFS.
- **Mount-table VFS Router:** Path normalization (`..`, `.`, double-slash), typed FD table, and a mount table routing `/dev/`, `/net/`, `/proc/`, and `/` independently.
- **Layered TarFS:** OCI-compatible filesystem supporting multiple TAR layers with shadowing and whiteout (`.wh.`) semantics.
- **Efficient Indexing:** Zero-copy filesystem indexing in memory with O(log N) lookup performance and boot-time pre-fetching.

## Documentation

Full developer and user documentation lives in [`docs/`](docs/):

- [Quick Start](docs/quickstart.md) — build, package, and launch a wapp in about ten minutes.
- [Architecture](docs/architecture.md) — the VFS router, wapp model, supervisor, platform seam, and WAMR runtime.
- [VFS Reference](docs/vfs-reference.md) — every `/dev`, `/net`, `/proc`, and TarFS path and its semantics.
- [Control Plane Reference](docs/control-plane-reference.md) — the `/dev/wanted` contract: nodes, verbs, state machine.
- [Configuration Reference](docs/configuration-reference.md) — the engine JSON config, field by field.
- [Platform Guide](docs/platform-guide.md) — Linux, the NuttX simulator, and the porting checklist.
- [Testing Guide](docs/testing-guide.md) — the unit, in-WASM selftest, and smoke tiers.

## General architecture

WANTED implements a Cloud-Native VFS Router. Path resolution is split into four primary namespaces:

1. **Device Namespace (`/dev/`):** Routes to registered sub-drivers — `null`, `pipe/<name>`, `stdin`, `stdout`, `stderr`, `platform`, `wanted`, and any wapp-configured drivers.
2. **Network Namespace (`/net/`):** Routes to the socket driver for TCP/UDP operations.
3. **Process Namespace (`/proc/`):** Read-only flat namespace exposing system state (`wapps`, `memory`, `clock_quality`, `wanted`). Privileged entries (`wapps`, `memory`) are hidden when `system.privileged` is false; `wanted` exposes engine identity and resource ceilings unprivileged.
4. **Application Space (Root `/`):** Handled by **TarFS**, which merges up to 4 OCI layers into a single unified view.

```text
WAPP -> WASI -> VFS ROUTER -> [/dev/ | /net/ | /proc/ | TarFS(/)]
```

## Wapp overview

A Wapp is a collection of OCI-compatible ustar TAR layers.

1. **Required File:** Only one entry must exist in the layer stack:
    - `app.wasm`: The WebAssembly application binary.

2. **Image identity:** Name and version come from the registry filename `<name>:<version>-<package>.wapp`, not from in-image metadata. The loader reads them back from the registry entry. A running wapp is an *instance* (named by `create <name>`); the *image* it runs is named by its launch config's `image` field (or `start <image>`), defaulting to the instance name — so one image can back many instances.

3. **Layering:** Supports OCI-style overlays. Newer layers shadow files in older layers. Whiteout files (`.wh.<filename>`) delete files from underlying layers.

## Built-in VFS devices

The following devices are always registered in every wapp's `/dev/` namespace, regardless of wapp configuration:

| Path | Purpose |
|------|---------|
| `/dev/null` | Reads return 0; writes are no-ops |
| `/dev/pipe/<name>` | Inter-wapp named pipe IPC over a process-wide shared store; 4096-byte ring buffer; up to 8 concurrent pipes; blocking reads by default (`O_NONBLOCK` opts out) |
| `/dev/stdin` | Stub; reads return EOF |
| `/dev/stdout` | Stub; writes are no-ops |
| `/dev/stderr` | Stub; writes are no-ops |

## Supervisor

WANTED boots a privileged wapp called the **supervisor** (Sheriff role). The supervisor image is a standard ustar TAR archive bundling `app.wasm`, loaded at runtime via `PlatformWappLoad`.

Two variants ship under `wasm/supervisor/`:

| Variant | Path | Source | Purpose |
|---|---|---|---|
| `sheriff` | `wasm/supervisor/sheriff/` | prebuilt `app.wasm` (separate repo) | Production control-plane agent |
| `wsh` | `wasm/supervisor/wsh/` | compiled from `wapps/wsh/` | Debug shell for interactive inspection |

Build both TAR images before running (this compiles `wsh` from `wapps/wsh/` and
bundles each variant's `app.wasm`):

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
./build/cmd/wanted-cli configs/example_config.json  # run with explicit config file
```

The config file is JSON. Relevant top-level fields:

- **`system.privileged`** — boolean; enables privileged `/proc` entries (wapp state, memory stats). Defaults to `false`.
- **`supervisor.imagePath`** — path to the supervisor TAR image; overrides the compiled-in default when set.
- **`supervisor.params`** — driver and console settings; if absent, compiled-in defaults apply (TCP socket at `localhost:8888`, TLS socket at `localhost:8889`, 9P at `localhost:5640`).

See [`configs/example_config.json`](configs/example_config.json) for a reference example.

## Build and Verification

The development environment is standardized via Podman to ensure toolchain consistency. A devcontainer configuration (`.devcontainer/`) is also provided for VS Code and JetBrains remote development.

The root `Makefile` wraps the containerized build and test commands — every target runs inside the build container, so the host only needs a container runtime. Run `make help` to list them. Override the runtime or image when needed, e.g. `make test RUNNER=docker` or `make build IMAGE=localhost/wanted-build:dev`.

### Build

```bash
make build        # supervisor TAR images + engine (production sheriff supervisor)
make wsh          # engine with the wsh debug supervisor compiled in
make wapps        # compile the sample wapp images under wapps/ (e.g. wapps/hello/)
```

### Test

```bash
make test            # unit suite via ctest
make selftest        # in-WASM functional/robustness suite (TAP) + system-control checks on Linux
make nuttx-selftest  # the same in-WASM suite on the NuttX simulator
make smoke-engine    # boot the production supervisor; assert a clean instantiate
```

`make selftest` also runs `test/syscontrol.sh`, which drives the `wsh` supervisor through poweroff / reboot / exit and asserts the engine-process lifecycle the in-WASM suite cannot observe — including that a respawned supervisor keeps a working console. The sim counterpart runs via `test/nuttx-sim.sh all`.

#### Selftest suite

`make selftest` and `make nuttx-selftest` run an identical suite from inside WASM using the `selftest` supervisor variant (built from `wapps/selftest/`). Because the suite uses only the WASI and WANTED VFS/control-plane ABI, it runs unchanged on Linux and the NuttX simulator — no platform-specific shell scripting.

Results are reported as TAP (Test Anything Protocol) on the supervisor's stdout; the runner checks for a plan line and the absence of `not ok` entries.

The suite covers four categories:

| Category | What is tested |
|----------|---------------|
| **VFS / namespace** | Path normalization, read-only TarFS, parent-traversal denial (`..` blocked at root), `/proc` access, control-plane node enumeration |
| **Inter-wapp IPC** | Two-wapp `/dev/pipe` round-trip: `preader` blocks on the channel while `pwriter` writes from a separate namespace; the supervisor verifies the payload via the log console |
| **Concurrency and stop** | `looper` wapp running concurrently with the supervisor; stopped via the control plane and confirmed terminated; edge cases (stop of a dead or unknown wapp) |
| **Negative / robustness** | WASM trap containment (`trapper`), CPU runaway (`cpuhog`), memory exhaustion (`membomb`), stack overflow (`stackbomb`), blocking-syscall stop (`blocker`, `pblock`), sandbox escape denial (`escaper`), fd table bounds (`fdhog`), crash-loop stability (`crasher`), malformed-image loader battery (no `app.wasm` entrypoint, invalid WASM, truncated TAR) |

### NuttX simulator

```bash
make nuttx-deps      # clone NuttX + apps submodules; link the engine app package
make nuttx-build     # configure and build the NuttX sim from a clean tree
make nuttx-selftest  # run the selftest suite on the sim
make nuttx-shell     # boot the sim to an interactive wsh prompt
```

The simulator uses the `sim:wanted` board config from the NuttX fork. NuttX and nuttx-apps are vendored as shallow git submodules at `third_party/`. `make nuttx-deps` is idempotent and must run once before `nuttx-build`.

### Interactive shell

```bash
make shell        # interactive shell in the build container (equivalent to run-podman.sh)
```

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for build/test expectations, code
style, and commit conventions.

## License

WANTED Engine is licensed under the [Apache License, Version 2.0](LICENSE).
Bundled third-party components retain their own licenses — see
[NOTICE](NOTICE).
