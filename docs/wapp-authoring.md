---
title: "Wapp Authoring"
date: 2026-06-09T20:00:00+01:00
weight: 30
toc: true
description: "Writing a wapp: package layout, image identity, the WASI ABI, filesystem and IPC, and building the image."
---

A wapp is a WebAssembly module (`app.wasm`) packaged into a TAR image. It runs in its own WASM linear memory and reaches the outside world **only** through the VFS the engine grants it — there is no ambient host access. This page is the reference for authoring one. See [Architecture](architecture.md) for the runtime model and [VFS Reference](vfs-reference.md) for the exhaustive path list.

The `hello` sample is the running example throughout:

```c
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

int main(void) {
    const char *msg = "hello-wapp: alive\n";
    write(STDOUT_FILENO, msg, strlen(msg));
    return 0;
}
```

## Package layout

A wapp image is a POSIX **ustar** TAR. The engine's TarFS mounts it read-only as the wapp's root filesystem (`/`), and the loader looks for the entrypoint by exact name at the archive root:

```
hello:0.0.1-1.wapp           # the TAR (registry filename: <name>:<version>-<package>.wapp)
├── app.wasm                 # required — the compiled module (the only mandatory member)
└── assets/logo.png          # optional — any data files become the read-only rootfs
```

- **`app.wasm`** is the only mandatory entry; it is always named `app.wasm` *inside* the package regardless of its source filename. An image is just code plus optional data.
- Every other entry appears to the wapp at its archived path. An asset at `assets/logo.png` is readable as `/assets/logo.png`.
- The root is **read-only** (`EROFS` on write). Writable storage is a [preopen](#preopens), not a packaged file.
- Multiple stacked layers merge newest-over-oldest with `.wh.<name>` whiteout deletion; see [VFS Reference → TarFS](vfs-reference.md#--tarfs-application-space). A single-layer image — one TAR — is the common case.

## Image identity

A wapp image has **no embedded metadata** — its identity is the registry filename. Installing the TAR as `registry/<name>:<version>-<package>.wapp` is what gives the image its name and version; the loader reads both back from the registry entry, and `/proc/wapps` / the `version` control node report them. A missing or unreadable `app.wasm` rejects the image at load.

**Instance identity is separate from image identity.** A running wapp is an *instance*, created by `create <instance>` on the control plane; the *image* it runs is named by the instance's launch config (`"image": "<name>"`, defaulting to the instance name) or by an explicit `start <image>`. One image can therefore run as N independent instances — the engine reports each under its instance name and records the image it runs on the per-instance `image` node. See [Control Plane Reference](control-plane-reference.md).

## The WASI ABI

Wapps target **`wasm32-wasi`** and link against WASI `snapshot_preview1`. The engine implements the bridge itself — it does not embed WAMR's libc-wasi — and registers the host functions below. Anything outside this set is unresolved at instantiation.

| Host function | Status | Notes |
|---------------|--------|-------|
| `fd_read`, `fd_write`, `fd_close`, `fd_seek` | full | Core I/O, routed through the VFS. |
| `fd_readdir` | full | Directory enumeration on preopens. |
| `path_open` | full | Opens any VFS path the wapp is granted. |
| `path_create_directory`, `path_rename`, `path_unlink_file` | full on preopens | Mutating paths require a writable [preopen](#preopens); the TARFS root returns `EROFS`. |
| `fd_prestat_get`, `fd_prestat_dir_name` | full | Preopen discovery (how libc finds your mounted dirs). |
| `fd_filestat_get`, `path_filestat_get`, `fd_fdstat_get` | partial | Basic type/size/flags; not every POSIX stat field is populated. |
| `fd_fdstat_set_flags` | partial | Supports toggling `O_NONBLOCK` (used by pipes). |
| `clock_time_get`, `clock_res_get` | full | Backed by the platform clock. |
| `random_get` | full | Backed by the platform RNG. |
| `args_get`, `args_sizes_get` | full | `argv[0]` is the wapp name; `argv[1..]` come from the launch config's `args[]`. |
| `environ_get`, `environ_sizes_get` | full | The environment is the launch config's `envs[]` (POSIX `KEY=VALUE` entries). |
| `poll_oneoff` | restricted | **Clock subscriptions only.** An `fd_read`/`fd_write` subscription returns `ENOSYS`; poll the fd directly instead. |
| `fd_datasync` | stub | Returns success without doing anything (the root is read-only). |
| `proc_exit` | full | Terminates the wapp with the given exit code. |
| `sock_accept`, `sock_recv`, `sock_send`, `sock_shutdown` | full (Linux) | Sockets are reached through `/net/`; see [VFS Reference → /net/](vfs-reference.md#net--network-namespace). |

Practical consequences for a wapp author:

- **Environment variables and argv come from the launch config.** A wapp's `argv` and `environ` are set from the `args[]` and `envs[]` arrays in its launch config (`getenv`/`argc` work normally). `argv[0]` is always the wapp name. The `hello` sample selects its behaviour from a `ROLE` env var passed this way. See [Control Plane Reference → Launch-config schema](control-plane-reference.md). Larger or writable configuration still belongs in a packaged file or a [preopen](#preopens).
- **`poll_oneoff` is a timer, not a readiness selector.** A `sleep()` works; an event loop that selects across file descriptors does not.
- **`stdout`/`stderr` are not files you open.** Writing to fd 1/2 reaches a console only if the launch config gives the wapp one; see [Filesystem access](#filesystem-access) and [Control Plane Reference](control-plane-reference.md).

## Filesystem access

What a wapp sees under `/` is assembled by the VFS router from three sources:

1. **TarFS root (`/`)** — the read-only contents of the image (above). Your `app.wasm` and any packaged data files.
2. **Preopens** — host directories bound into the namespace as read-write storage. See below.
3. **Device, network, and process namespaces** — `/dev/`, `/net/`, `/proc/`, overlaid by the router. The full path list is the [VFS Reference](vfs-reference.md).

Five `/dev/` entries are always present (`/dev/null`, `/dev/pipe/<name>`, and the `stdin`/`stdout`/`stderr` stubs). Everything else — a console, a `platform` mount, a socket at `/net/...`, the `/dev/wanted` control plane — is granted only when the launch config asks for it.

## Inter-wapp IPC

`/dev/pipe/<name>` is a process-wide named pipe: a pipe one wapp opens is visible to another, making it a wapp-to-wapp channel. It is always available (no config needed).

```c
/* writer */
int fd = open("/dev/pipe/smoke", O_WRONLY);
write(fd, "inter-wapp-pipe-ok", 18);
close(fd);                              /* close signals EOF to the reader */
```

```c
/* reader */
int fd = open("/dev/pipe/smoke", O_RDONLY);
char buf[64];
int n = read(fd, buf, sizeof buf);      /* blocks until the writer produces data */
close(fd);
```

Semantics to design around:

- **Ring buffer of 4096 bytes**; up to 8 named pipes exist concurrently.
- **Reads block by default.** With no data buffered and a writer attached (or none seen yet), a read sleeps and retries up to a bounded safety cap. Open with `O_NONBLOCK` to get `EAGAIN` instead.
- **EOF** is delivered only after a writer has attached and all writers have closed — so a reader that starts first waits for the writer rather than seeing a premature EOF.

## Preopens

A preopen is a host directory the engine binds into the wapp's namespace as **read-write** storage that survives restarts — the wapp's only writable filesystem. Declare it as a `mounts[]` entry with the `platform` backend, giving the path it should appear at:

```json
{ "mounts": [ { "name": "platform", "path": "/tmp/wanted-smoke-pipe" } ] }
```

The engine creates the host directory if it is absent and binds it as a WASI preopen at that **same path** inside the wapp. The wapp then uses ordinary POSIX calls against it:

```c
int fd = open("/tmp/wanted-smoke-pipe/result", O_WRONLY | O_CREAT | O_TRUNC, 0644);
write(fd, buf, len);
close(fd);
```

`path_open`, `path_create_directory`, `path_rename`, and `path_unlink_file` operate here; the same calls against the read-only TarFS root return `EROFS`. The launch config and its `params` block are documented in [Control Plane Reference](control-plane-reference.md) and [Configuration Reference](configuration-reference.md).

A `platform` mount pins the wapp to a host path the operator must know. When a wapp just needs *somewhere persistent to write* and should stay portable across hosts (no host-layout assumption), use a `volume` mount instead: the wapp names only a volume and the engine owns the backing location.

```json
{ "mounts": [ { "name": "volume", "path": "/data", "options": "name=cache" } ] }
```

The store is created on first use, namespaced to this wapp, and survives restarts and reboots the same way a preopen does. Use ordinary POSIX calls against `/data`.

Adding `shared` to the options puts the volume in a **cross-wapp** namespace — every wapp that mounts the same `name=<volname>,shared` reaches one store. That makes a shared volume an inter-wapp channel for a producer→processor→publisher pipeline: each stage processes files a downstream stage reads, and a read-only stage mounts it with `shared,ro`. The engine provides no locking — stages coordinate themselves (e.g. atomic rename, or a [named pipe](#preopens) for signalling). A plain `volume` (no `shared`) stays private to the wapp.

## Capability requirements

A wapp's effective capabilities are exactly what its launch config grants: the consoles, drivers, mounts, sockets, and the `/dev/wanted` control plane the supervisor wires up at start. The image itself declares nothing. A declarative capability-requirement vocabulary (its home — OCI image-config labels vs. implicit wasm imports — is an open design question) is deferred to a future revision.

## Building a wapp

Compile and package in one place with a multi-stage `Containerfile`. The builder stage is the `wanted-wasm-sdk` image, which carries the wasm toolchains (C/C++ via wasi-sdk, plus Zig, TinyGo, and Rust); the final stage is the wapp's read-only rootfs.

```dockerfile
# Containerfile
FROM wanted-wasm-sdk AS build
WORKDIR /src
COPY hello.c Makefile ./
RUN make NAME=hello                         # -> hello.wasm

FROM scratch
COPY --from=build /src/hello.wasm  app.wasm
# COPY assets/  assets/                    # optional packaged data files
```

The C reference for the compile flags is `wapps/hello/Makefile` — `--target=wasm32-wasi`, an initial/maximum linear memory of one 64 KiB page, a 4 KiB stack, `-Os`, and `--strip-all`. Read `/proc/wanted` at runtime for the engine's resource ceilings.

Build the image, then export its filesystem as the `.wapp` TAR — the exported rootfs *is* the wapp's TarFS root, with `app.wasm` at its top level:

```bash
podman build -t hello-wapp .
cid=$(podman create hello-wapp)
podman export "$cid" -o registry/hello:0.0.1-1.wapp
podman rm "$cid"
```

Install means placing that TAR in the registry directory under `<name>:<version>-<package>.wapp` — the filename **is** the image's identity (name and version). Launch it through the control plane — `create`, then `set_config`, then `start` from `wsh`; the full run is in the [Quick Start](quickstart.md), and the install/launch verbs are in the [Control Plane Reference](control-plane-reference.md).

The low-level fallback — no container build — is to tar the single file directly, exactly as the test harness does:

```bash
make -C wapps/hello                        # produces hello.wasm
mkdir pkg && cp wapps/hello/hello.wasm pkg/app.wasm
tar --format=ustar --owner=0 --group=0 --mtime='1970-01-01 00:00:00 UTC' \
    -C pkg -cf registry/hello:0.0.1-1.wapp app.wasm
```

## See also

- [Quick Start](quickstart.md) — build, package, and launch `hello` end to end.
- [VFS Reference](vfs-reference.md) — every `/dev/`, `/net/`, `/proc/`, and TarFS path.
- [Control Plane Reference](control-plane-reference.md) — the launch config schema and the start/stop verbs.
- [Configuration Reference](configuration-reference.md) — the engine config and the supervisor's own `params`.
