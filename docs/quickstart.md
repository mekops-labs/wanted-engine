---
title: "Quick Start"
date: 2026-06-08T17:30:00+01:00
weight: 10
toc: true
description: "Build the engine, package a wapp, and launch it from the wsh debug shell in about ten minutes."
---

This guide takes you from a clean checkout to a running wapp. It uses the `wsh` debug supervisor — an interactive shell wired to the engine's control plane — so every step is a command you type yourself.

## Prerequisites

- A container runtime: **Podman** (default) or **Docker**.
- **`make`**.

Nothing else. The toolchain (CMake, Ninja, the WASI SDK, the WAMR runtime) lives in the build container; the host only invokes `make`, which wraps every command in that container. To use Docker instead of Podman, append `RUNNER=docker` to any `make` command.

## Build

Compile the sample wapps and the engine with the `wsh` supervisor:

```bash
make wapps   # compile the sample wapps under wapps/ (produces wapps/hello/hello.wasm)
make wsh     # build the engine + CLI with the wsh debug supervisor
```

`make wapps` compiles each sample to a `.wasm` binary. `make wsh` builds `build/cmd/wanted-cli` with `wsh` as its boot supervisor.

## Package a wapp into the registry

The engine starts wapps by name from a **registry** — on Linux, the `./registry/` directory scanned for `<name>:<version>.wapp` images. A `.wapp` is an OCI-style ustar TAR holding `app.wasm` (and any optional data files). The compiled samples are not packaged automatically, so package `hello` once:

```bash
mkdir -p registry
stage=$(mktemp -d)
cp wapps/hello/hello.wasm "$stage/app.wasm"
tar --format=ustar --owner=0 --group=0 --mtime='1970-01-01 00:00:00 UTC' \
    -C "$stage" -cf registry/hello:0.0.1-1.wapp app.wasm
rm -rf "$stage"
```

The **registry filename is the image's identity**: `hello`, version `0.0.1`, package `1` → `hello:0.0.1-1.wapp`. The engine reads the name and version from the filename. The wasm binary is renamed to `app.wasm` inside the TAR — that is the fixed entrypoint name the loader expects.

`app.wasm` is the only required member. A wapp image can carry any additional files — config, certificates, static assets, data — and they become visible to the wapp as a read-only filesystem at `/`, served by TarFS. Add such files to the TAR alongside `app.wasm` and they appear at the matching path inside the wapp. (Small runtime knobs, like the `hello` sample's `ROLE`, are better passed as launch-config env vars or args than baked into the image — see [Control Plane Reference](control-plane-reference.md).)

### Image identity

The image's name and version come entirely from its registry filename `<name>:<version>-<package>.wapp` — for `hello`, that is name `hello`, version `0.0.1`, package `1`. The engine reports them at `/proc/wapps` and the per-instance `version` node. Capabilities are not declared in the image; they are exactly what the launch config grants at start (consoles, drivers, mounts, sockets, the control plane). One image can run as several independent **instances** — see [Control Plane Reference](control-plane-reference.md).

## Run

Boot the engine to an interactive `wsh` prompt:

```bash
make wsh-shell
```

You land at the shell:

```
Wsh v 0.5.0
> 
```

`wsh` is itself a wapp: a privileged supervisor granted the `wanted` control-plane driver. Its commands drive `/dev/wanted` the same way a production supervisor does.

## Launch your first wapp

List the running wapps. The supervisor is itself a wapp, so it appears in the list:

```
> status
supervisor      running
```

Start `hello` by name. `wsh start` writes `start hello` to the root control node `/dev/wanted/ctl`, which resolves the name in the registry, loads the image, and launches the wapp as its own thread. Check its state — it is running:

```
> start hello
> status hello
hello:
  state    running
  version  0.0.1-1
  id       1
```

The `hello` sample (launched with no `ROLE` set) writes an alive marker, lives for two seconds, then exits. A moment later its state is `exited`. By default a wapp's stdout and stderr are captured to a per-wapp **log console**, readable at its `log` node:

```
> status hello
hello:
  state    exited
  version  0.0.1-1
  id       1
> cat /dev/wanted/wapps/hello/log
hello-wapp: alive
hello-wapp: exit
```

A wapp that runs indefinitely is stopped explicitly — `wsh stop` writes `stop` to that wapp's own control node `/dev/wanted/wapps/<name>/ctl`:

```
> stop hello
```

To override a wapp's console — share the engine's terminal, discard output, or point a slot elsewhere — write a launch config to its `config` node before `start`. The console schema and every control-plane node are documented in the [Control Plane Reference](control-plane-reference.md).

## Next steps

- [Architecture](architecture.md) — how the VFS router, wapp model, and supervisor fit together.
- [Wapp Authoring](wapp-authoring.md) — write and package your own wapp.
- [Control Plane Reference](control-plane-reference.md) — the complete `/dev/wanted/*` contract.
