---
title: "Testing Guide"
date: 2026-06-08T17:30:00+01:00
weight: 80
toc: true
description: "The three test tiers — unit, in-WASM selftest, and smoke — how to run them, and how to add a new test wapp."
---

The engine has three test tiers, each catching a different class of failure. All run inside the build container.

| Suite | Command | Scope |
|-------|---------|-------|
| Unit (ctest) | `just test` | C unit tests: VFS, TarFS, WASI, registry, API parsing |
| In-WASM selftest | `just selftest` / `just nuttx-selftest` | The functional + robustness scenario suite, run from inside WASM; TAP |
| Cross-arch selftest | `just selftest-qemu-aarch64` / `-mipsel` | The same suite against a cross-built engine under qemu |
| Smoke | `just smoke-engine` | The production sheriff supervisor instantiates cleanly |
| Live update | `just live-update` | The supervisor image is swapped under a running engine |

## Unit suite (ctest)

```bash
just test                    # full suite
```

Each `test/test-*.c` file is one group exercising a subsystem directly in C: `test-vfs*` (router, mount table, ops, and the per-namespace drivers — including the `sha256`/`ed25519`/`inflate` offload devices and the `log` mount), `test-tarfs` (layer merge, whiteouts), `test-pipe`, `test-procfs` / `test-procfs-wapps`, `test-platform-clock` / `test-platform-registry`, `test-vfs-wanted-*` (control-plane drivers), `test-wasm-meta` (the wasm `(memory)`-section parser), `test-vendor-ed25519` (the vendored verifier against RFC 8032 vectors), `test-api`. Tests are built into one `tests` binary and registered with CTest per group, so you can run one:

```bash
cd build && ctest -R test-tarfs --output-on-failure
```

To add a group: drop a `test/test-<thing>.c` using the Unity assertions (see `test/test-utils.h`); the CMake glob picks it up and registers it as `test-<thing>`.

`test-vfs-9p-local` is the one group that talks to a live peer: it forks a minimal 9P2000 server onto a filesystem socket and drives the `9p` driver against it over a `unix://` mount, so the protocol round trips are exercised rather than stubbed.

## Selftest suite

`just selftest` (Linux) and `just nuttx-selftest` (NuttX sim) run an identical suite from **inside WASM**, driven by the `selftest` supervisor variant (`wapps/selftest/`). Because it uses only the WASI and WANTED control-plane ABI, it runs unchanged on both targets — no platform-specific scripting. Results are reported as TAP (_Test Anything Protocol_); the runner asserts a plan line and the absence of `not ok`.

The suite covers four categories:

| Category | What it exercises |
|----------|-------------------|
| VFS / namespace | Path normalisation, read-only TarFS, parent-traversal denial, `/proc` access, control-plane enumeration |
| Inter-wapp IPC | A two-wapp `/dev/pipe` round-trip: `preader` blocks while `pwriter` writes from a separate namespace |
| Concurrency and stop | `looper` running alongside the supervisor, stopped via the control plane; stop of a dead or unknown wapp |
| Observability | The `observer` reference wapp: granted a `log` mount and `/proc/wapps` but **not** the control plane — it enumerates the fleet and tails logs while every control-plane access is denied (observe-without-control) |
| Args / env / volumes / memory | `argv`/`environ` delivery (`argenv`), instance-vs-image resolution (`duplex`), private/shared volume semantics (`volcheck`), the per-wapp linear-memory cap (`bigmem`, `biginit`) |
| Negative / robustness | Trap containment (`trapper`), CPU runaway (`cpuhog`), memory exhaustion (`membomb`), stack overflow (`stackbomb`), blocking-syscall stop (`blocker`, `pblock`), sandbox-escape denial (`escaper`), fd-table bounds (`fdhog`), crash-loop stability (`crasher`), and a malformed-image battery (no `app.wasm` entrypoint, invalid WASM, truncated TAR) |

Each scenario is a small purpose-built wapp under `wapps/` that the supervisor launches and then checks via the control plane — a misbehaving wapp must be contained without taking down the engine or its neighbours.

A companion recipe, `just syscontrol` (Linux) / `just nuttx-syscontrol` (sim), runs `test/syscontrol.sh`, which drives the `wsh` supervisor through poweroff / reboot / exit and asserts the engine-process lifecycle the in-WASM suite cannot observe — including that a respawned supervisor keeps a working console.

A standalone script, `test/devcheck.sh`, boots the `devcheck` wapp as the supervisor and round-trips the `sha256` / `ed25519` / `inflate` offload devices end to end (WASI → VFS → driver), powering the engine off after one pass.

## Cross-architecture selftest (qemu)

```bash
just selftest-qemu-aarch64    # aarch64 (musl)
just selftest-qemu-mipsel     # mipsel (musl)
just selftest-qemu <sdk-url-or-dir>   # any OpenWRT target
```

`test/selftest-qemu.sh` cross-builds the engine from an OpenWRT SDK — the same toolchain the `.ipk` uses — and runs the selftest suite against it under qemu user-mode emulation, with the SDK's target rootfs as the loader root. The suite itself is unchanged: wapps and the supervisor are WASM loaded by path at runtime, so only the engine binary differs.

This is the lane for faults that are invisible on x86. Engine code that is undefined-behaviour-clean on x86_64 can fault on another architecture's calling convention, alignment rules, or signal handling, and emulation reproduces that faithfully enough to catch it — without a router on the bench. The SDK is downloaded and cached under `.openwrt-sdk/` on first use; TLS is off in this lane, so it skips the SDK's one-time OpenSSL stage.

A faulting guest leaves a `qemu_*.core` dump in the repo root (git-ignored).

## Smoke test

```bash
just smoke-engine
```

`test/smoke-engine.sh` boots the real production sheriff supervisor and asserts it instantiates cleanly — the regression guard for the out-of-repo supervisor blob. It can fail on a corrupt or missing supervisor image, or a WAMR opcode mismatch between the blob and the bundled runtime.

## Supervisor live update

```bash
just live-update
```

`test/live-update.sh` swaps the supervisor image under a running engine: a child wapp started beforehand is still running afterwards, an image staged while the engine runs is adopted only once `reload-supervisor` is armed, and an image that cannot launch is rolled back to the compiled-in one with the engine still serving.

The armed and unarmed cases are both checked on purpose — a respawn with no reload armed must keep the image in use, which is what proves adoption is caused by the reload rather than by the respawn.

It builds its own engine (`build-wsh`) because the supervisor image path is compiled in, and uses a config whose staged image path differs from the compiled-in one, which is what makes the rollback observable.

## Writing a test wapp

To add a scenario:

1. Create `wapps/<name>/` with the wapp source and a `Makefile` (copy `wapps/hello/Makefile`) - just an `app.wasm`.
2. Make the wapp exercise one behaviour — reach a VFS path, misbehave in one specific way, or talk over a pipe.
3. Stage it: add `<name>:<version>` to `TEST_WAPPS` in `test/selftest.sh` (and `stage_test_wapp` in `test/nuttx-sim.sh` for the sim). The runner packages each into the registry as `<name>@<version>.wapp`.
4. Add the supervisor-side check in `wapps/selftest/main.c`: launch the wapp via the control plane and assert the expected `state` / `log` outcome, emitting a TAP line.
5. Re-run `just selftest`.

The suite is the reference for how to drive the control plane from a wapp; new platform work must keep it green on both Linux and the sim.

## Continuous integration

GitLab CI (`.gitlab-ci.yml`) runs the same `just` recipes you run locally:

- **`build-gcc` / `build-clang`** — the engine under both compilers; **`build-wsh`** the debug-supervisor build; **`build-wasm`** every wasm artifact (supervisor TARs, sheriff, and the test wapps) in the wapp SDK image; **`build-nuttx`** the sim build.
- **`unit-test-gcc` / `unit-test-clang`** — the ctest suite under each; **`coverage`** the instrumented build + report.
- **`integration-tests`** — `smoke-engine` + `selftest` + `syscontrol` on Linux.
- **`nuttx-selftest` / `nuttx-syscontrol`** — the same selftest and system-control suites on the NuttX sim, built in-job from a clean tree.
- **Static analysis** — `format-check`, `shell-check`, `clang-tidy`, `cppcheck`, `semgrep`, `trivy`.

## See also

- [Quick Start](quickstart.md) — packaging and launching a wapp by hand.
- [Platform Guide](platform-guide.md) — the same suites across Linux and the NuttX sim.
