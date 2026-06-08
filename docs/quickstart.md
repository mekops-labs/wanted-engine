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

The engine starts wapps by name from a **registry** — on Linux, the `./registry/` directory scanned for `<name>:<version>.wapp` images. A `.wapp` is an OCI-style ustar TAR holding `app.wasm` and `manifest.json`. The compiled samples are not packaged automatically, so package `hello` once:

```bash
mkdir -p registry
stage=$(mktemp -d)
cp wapps/hello/hello.wasm    "$stage/app.wasm"
cp wapps/hello/manifest.json "$stage/manifest.json"
tar --format=ustar --owner=0 --group=0 --mtime='1970-01-01 00:00:00 UTC' \
    -C "$stage" -cf registry/hello:0.0.1-1.wapp app.wasm manifest.json
rm -rf "$stage"
```

The image name encodes the manifest's identity: `hello`, version `[0, 0, 1]`, package `1` → `hello:0.0.1-1.wapp`. The wasm binary is renamed to `app.wasm` inside the TAR — that is the fixed entrypoint name the loader expects.

`app.wasm` and `manifest.json` are the bare minimum. A wapp image can carry any additional files — config, certificates, static assets, data — and they become visible to the wapp as a read-only filesystem at `/`, served by TarFS. The `hello` sample, for instance, reads an optional `/etc/role` from its own image. Add such files to the TAR alongside `app.wasm` and they appear at the matching path inside the wapp.

### The manifest

`manifest.json` declares the wapp's identity and the capabilities it expects. `hello`'s is:

```json
{
    "name": "hello",
    "version": [0, 0, 1],
    "package": 1,
    "requirements": ["console"]
}
```

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `name` | string | yes | Unique wapp identifier (≤15 characters). Also the name you `start`. |
| `version` | `[major, minor, patch]` | yes | Integer triple. |
| `package` | integer | yes | Package revision; the `-N` suffix in the image name. |
| `requirements` | string array | no | Abstract capability names the wapp needs. The engine parses and stores them; a supervisor validates them against policy before launching. An absent or empty array declares no requirements. |

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

List the running wapps — none yet:

```
> status
```

Start `hello` by name. `wsh start` writes `start hello` to the root control node `/dev/wanted/ctl`, which resolves the name in the registry, loads the image, and launches it as its own thread:

```
> start hello
> status hello
hello:
  state    running
  version  0.0.1-1
  id       1
```

The `hello` sample, launched with no role file, writes an alive marker, lives for two seconds, then exits. Read its state again and it has finished:

```
> status hello
hello:
  state    exited
  version  0.0.1-1
  id       1
```

For a wapp that runs indefinitely, stop it explicitly — `wsh stop` writes `stop` to that wapp's own control node `/dev/wanted/wapps/<name>/ctl`:

```
> stop hello
```

### Capturing a wapp's output

`status` reports lifecycle state, not stdout. To capture what a wapp prints, launch it with a **log console**: write a one-line config to its `config` node before starting it, then read the buffered output from its `log` node.

```
> write /dev/wanted/wapps/hello/config {"console":{"out":{"name":"log"},"err":{"name":"log"}}}
> start hello
> cat /dev/wanted/wapps/hello/log
hello-wapp: alive
hello-wapp: exit
```

The config schema and every control-plane node are documented in the [Control Plane Reference](control-plane-reference.md).

## Next steps

- [Architecture](architecture.md) — how the VFS router, wapp model, and supervisor fit together.
- [Wapp Authoring](wapp-authoring.md) — write and package your own wapp.
- [Control Plane Reference](control-plane-reference.md) — the complete `/dev/wanted/*` contract.
