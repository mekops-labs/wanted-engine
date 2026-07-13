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
- **`make`** (the host wrapper) — or nothing extra if you work inside the devcontainer.

Nothing else. The toolchain (CMake, Ninja, the WASI SDK, the WAMR runtime) lives in the build container. Commands are [`just`](https://just.systems) recipes that run in that container; on a bare host the `make` wrapper runs each one for you (`make build` is `just build` in the image), so the examples below use `just` and `make <recipe>` works identically. Inside the devcontainer or CI, call `just` directly. To use Docker instead of Podman, append `RUNNER=docker` to any `make` command.

## Build

Compile the sample wapps and the engine with the `wsh` supervisor:

```bash
just wapps   # compile the sample wapps under wapps/ (produces wapps/hello/hello.wasm)
just wsh     # build the engine + CLI with the wsh debug supervisor
```

`just wapps` compiles each sample to a `.wasm` binary. `just wsh` builds `build/cmd/wanted-cli` with `wsh` as its boot supervisor.

## Package a wapp into the registry

The engine starts wapps by name from a **registry** — on Linux, the `./registry/` directory scanned for `<name>@<version>.wapp` images. A `.wapp` is an OCI-style ustar TAR holding `app.wasm` (and any optional data files). The compiled samples are not packaged automatically, so package `hello` once:

```bash
mkdir -p registry
stage=$(mktemp -d)
cp wapps/hello/hello.wasm "$stage/app.wasm"
tar --format=ustar --owner=0 --group=0 --mtime='1970-01-01 00:00:00 UTC' \
    -C "$stage" -cf registry/hello@0.0.1-1.wapp app.wasm
rm -rf "$stage"
```

The **registry filename is the image's identity**: `hello`, version `0.0.1`, package `1` → `hello@0.0.1-1.wapp`. The engine reads the name and version from the filename. The wasm binary is renamed to `app.wasm` inside the TAR — that is the fixed entrypoint name the loader expects.

`app.wasm` is the only required member. A wapp image can carry any additional files — config, certificates, static assets, data — and they become visible to the wapp as a read-only filesystem at `/`, served by TarFS. Add such files to the TAR alongside `app.wasm` and they appear at the matching path inside the wapp. (Small runtime knobs, like the `hello` sample's `ROLE`, are better passed as launch-config env vars or args than baked into the image — see [Control Plane Reference](control-plane-reference.md).)

### Image identity

The image's name and version come entirely from its registry filename `<name>@<version>-<package>.wapp` — for `hello`, that is name `hello`, version `0.0.1`, package `1`. The engine reports them at `/proc/wapps` and the per-instance `version` node. Capabilities are not declared in the image; they are exactly what the launch config grants at start (consoles, drivers, mounts, sockets, the control plane). One image can run as several independent **instances** — see [Control Plane Reference](control-plane-reference.md).

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

Launching a wapp is three deliberate steps — reserve the instance name, give it a JSON launch config, then issue the launch verb:

```
> create hello
> set_config hello {"console":{"in":{"name":"null"},"out":{"name":"platform"},"err":{"name":"platform"}}}
> start hello
```

- **`create hello`** reserves the instance namespace `/dev/wanted/wapps/hello/`. Its `config`, `ctl`, and read nodes exist only after this, and its `state` reads `created`.
- **`set_config hello <json>`** buffers the launch config at `wapps/hello/config`, moving the instance to `not_started`. The config above gives the wapp a null stdin and routes its stdout/stderr to the **platform** console — the engine's own terminal — so its output prints straight into this shell session. A wapp's capabilities are exactly what this config grants — consoles, drivers, mounts, sockets, args, and envs (see the [Control Plane Reference](control-plane-reference.md)). Keep the JSON free of spaces: `wsh` splits a line on whitespace, so a compact object stays one token.
- **`start hello`** writes the bare verb `start` to `wapps/hello/ctl`. The engine resolves the image (no `image` in the config, so the instance name `hello` → `hello@0.0.1-1.wapp`), loads it, and runs the wapp as its own thread. A `start` on a reservation with no config is rejected — the `set_config` must come first.

The `hello` sample (launched with no `ROLE` set) writes an alive marker, lives for two seconds, then exits — its output appears directly at the prompt:

```
> hello-wapp: alive
hello-wapp: exit
```

Check its state while it lives — it is running; a moment after the exit marker it reads `exited`:

```
> status hello
hello:
  state    exited
  version  0.0.1-1
  id       1
```

Had the config left `out`/`err` unset, they would default to the **log** console: output is then captured to a per-wapp ring buffer instead of a terminal, read back through a `log` mount granted in a reader's `mounts[]` (the stock `wsh` config does not include one). See the [Control Plane Reference](control-plane-reference.md) and [VFS Reference](vfs-reference.md).

A wapp that runs indefinitely is stopped explicitly — `stop` writes `stop` to that wapp's own control node `/dev/wanted/wapps/<name>/ctl`:

```
> stop hello
```

Once a wapp has reached a terminal state, `delete hello` releases the name so it leaves `wapps/`. Richer launch configs — drivers, mounts, sockets, args, and environment variables — go in the same `set_config` JSON; the full console schema and every control-plane node are documented in the [Control Plane Reference](control-plane-reference.md).

## Next steps

- [Architecture](architecture.md) — how the VFS router, wapp model, and supervisor fit together.
- [Wapp Authoring](wapp-authoring.md) — write and package your own wapp.
- [Control Plane Reference](control-plane-reference.md) — the complete `/dev/wanted/*` contract.
