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
| In-WASM selftest | `just selftest` / `just nuttx-selftest` | 29 functional + robustness scenarios from inside WASM; TAP |
| Smoke | `just smoke-engine` | The production sheriff supervisor instantiates cleanly |

## Unit suite (ctest)

```bash
just test                    # full suite
```

Each `test/test-*.c` file is one group exercising a subsystem directly in C: `test-vfs*` (router, mount table, ops, the per-namespace drivers), `test-tarfs` (layer merge, whiteouts), `test-pipe`, `test-procfs`, `test-platform-clock` / `test-platform-registry`, `test-vfs-wanted-*` (control-plane drivers), `test-api`. Tests are built into one `tests` binary and registered with CTest per group, so you can run one:

```bash
cd build && ctest -R test-tarfs --output-on-failure
```

To add a group: drop a `test/test-<thing>.c` using the Unity assertions (see `test/test-utils.h`); the CMake glob picks it up and registers it as `test-<thing>`.

## Selftest suite

`just selftest` (Linux) and `just nuttx-selftest` (NuttX sim) run an identical suite from **inside WASM**, driven by the `selftest` supervisor variant (`wapps/selftest/`). Because it uses only the WASI and WANTED control-plane ABI, it runs unchanged on both targets — no platform-specific scripting. Results are reported as TAP (_Test Anything Protocol_); the runner asserts a plan line and the absence of `not ok`.

The suite covers four categories:

| Category | What it exercises |
|----------|-------------------|
| VFS / namespace | Path normalisation, read-only TarFS, parent-traversal denial, `/proc` access, control-plane enumeration |
| Inter-wapp IPC | A two-wapp `/dev/pipe` round-trip: `preader` blocks while `pwriter` writes from a separate namespace |
| Concurrency and stop | `looper` running alongside the supervisor, stopped via the control plane; stop of a dead or unknown wapp |
| Negative / robustness | Trap containment (`trapper`), CPU runaway (`cpuhog`), memory exhaustion (`membomb`), stack overflow (`stackbomb`), blocking-syscall stop (`blocker`, `pblock`), sandbox-escape denial (`escaper`), fd-table bounds (`fdhog`), crash-loop stability (`crasher`), and a malformed-image battery (no `app.wasm` entrypoint, invalid WASM, truncated TAR) |

Each scenario is a small purpose-built wapp under `wapps/` that the supervisor launches and then checks via the control plane — a misbehaving wapp must be contained without taking down the engine or its neighbours.

A companion recipe, `just syscontrol` (Linux) / `just nuttx-syscontrol` (sim), runs `test/syscontrol.sh`, which drives the `wsh` supervisor through poweroff / reboot / exit and asserts the engine-process lifecycle the in-WASM suite cannot observe — including that a respawned supervisor keeps a working console.

## Smoke test

```bash
just smoke-engine
```

`test/smoke-engine.sh` boots the real production sheriff supervisor and asserts it instantiates cleanly — the regression guard for the out-of-repo supervisor blob. It can fail on a corrupt or missing supervisor image, or a WAMR opcode mismatch between the blob and the bundled runtime.

## Writing a test wapp

To add a scenario:

1. Create `wapps/<name>/` with the wapp source and a `Makefile` (copy `wapps/hello/Makefile`) - just an `app.wasm`.
2. Make the wapp exercise one behaviour — reach a VFS path, misbehave in one specific way, or talk over a pipe.
3. Stage it: add `<name>:<version>` to `TEST_WAPPS` in `test/selftest.sh` (and `stage_test_wapp` in `test/nuttx-sim.sh` for the sim). The runner packages each into the registry as `<name>:<version>.wapp`.
4. Add the supervisor-side check in `wapps/selftest/main.c`: launch the wapp via the control plane and assert the expected `state` / `log` outcome, emitting a TAP line.
5. Re-run `just selftest`.

The suite is the reference for how to drive the control plane from a wapp; new platform work must keep it green on both Linux and the sim.

## Continuous integration

GitLab CI (`.gitlab-ci.yml`) runs:

- **`build-gcc` / `build-clang`** — the engine under both compilers; **`build-wsh`** the debug-supervisor build; **`build-coverage`** the instrumented build.
- **`unit-test-gcc` / `unit-test-clang`** — the ctest suite under each; **`coverage`** reports it.
- **`integration-tests`** — the selftest + smoke suites on Linux.
- **`nuttx-integration-tests`** — the engine built as a NuttX built-in, running `smoke-engine` + selftest on the sim.

## See also

- [Quick Start](quickstart.md) — packaging and launching a wapp by hand.
- [Platform Guide](platform-guide.md) — the same suites across Linux and the NuttX sim.
