---
title: "VFS Reference"
date: 2026-06-08T17:30:00+01:00
weight: 40
toc: true
description: "The fixed VFS a wapp always sees: the always-present /dev builtins, /proc, and the TarFS root. Config-mounted drivers are summarized separately."
---

A wapp's entire view of the world is the VFS, and it has two distinct parts. The **fixed namespace** is identical for every wapp regardless of configuration: the always-present `/dev/` builtins, the `/proc/` system namespace, and the TarFS root (`/`). On top of it, a wapp's launch config mounts **config-mounted drivers** at paths it chooses — these are not fixed locations.

This page is the exhaustive reference for the **fixed** part. The config-mounted drivers are summarized in [Config-mounted drivers](#config-mounted-drivers) and fully specified by the [Control Plane Reference](control-plane-reference.md) and [Configuration Reference](configuration-reference.md). See [Architecture](architecture.md) for how the router dispatches between namespaces.

## `/dev/` — device builtins

These five `/dev/` entries are **always present** in every wapp, independent of its launch config. (Other drivers can also appear under `/dev/` when the config mounts them — see [Config-mounted drivers](#config-mounted-drivers) — but those are not fixed paths and are not documented here.)

| Path | Driver | Access | Semantics |
|------|--------|--------|-----------|
| `/dev/null` | null | rw | Reads return 0 bytes (EOF); writes are accepted and discarded. |
| `/dev/pipe/<name>` | pipe | rw | Process-wide named-pipe IPC. See below. |
| `/dev/stdin` | stdio | r | Alias of WASI fd 0 — reads from the same console backing. |
| `/dev/stdout` | stdio | w | Alias of WASI fd 1 — writes to the same console backing. |
| `/dev/stderr` | stdio | w | Alias of WASI fd 2 — writes to the same console backing. |

The stdio entries alias the wapp's standard descriptors: opening `/dev/stdout` and writing reaches exactly the same place WASI fd 1 does — the `platform` console, the `log` ring, or `/dev/null` — whichever the launch config wired the slot to (see [Control Plane Reference](control-plane-reference.md)).

### `/dev/pipe/<name>` — inter-wapp IPC

A named pipe over a single process-wide store: a pipe opened by one wapp is visible to another, so it is a wapp-to-wapp channel. Open `/dev/pipe/<name>` for reading or writing; the name is created on first use.

- **Ring buffer** of 4096 bytes; up to 8 concurrent named pipes.
- **Reads block by default.** With no data and a writer attached (or none yet seen), a read sleeps and retries; `O_NONBLOCK` opts out. A bounded safety cap limits the wait.
- **EOF** is returned only once a writer has attached and all writers have closed.

```c
int fd = open("/dev/pipe/work", O_WRONLY);   /* writer */
write(fd, payload, len);
close(fd);                                    /* signals EOF to the reader */
```

## `/proc/` — process namespace

A read-only, flat namespace exposing system state. Privileged entries are visible only when the engine config sets `system.privileged: true`; otherwise they are hidden from both reads and directory enumeration.

| Path | Access | Privileged | Content |
|------|--------|:----------:|---------|
| `/proc/wapps` | r | yes | Per-wapp state — name and status for each running wapp. |
| `/proc/memory` | r | yes | `heap_used` / `heap_total`, via `PlatformMemoryStats`. |
| `/proc/clock_quality` | r | no | Platform clock-quality metric. |
| `/proc/wanted` | r | no | Engine identity and compile-time ceilings — `platform`, `version`, `max_wapps`, `max_wapp_name`, `max_path`, `wasm_stack`, `wasm_heap`, `wasm_max_pages`, `log_slots`. |

Each entry reads its value in one shot; a second read on the same fd returns EOF, regenerating on a fresh open.

`/proc/wanted` reports the engine itself as `key:\tvalue` lines, one per field — human-readable, split on the tab:

```text
platform:	linux
version:	0.6.0+g06f5cca.20260608205353
max_wapps:	3
max_wapp_name:	15 B
max_path:	256 B
wasm_stack:	8192 B
wasm_heap:	8192 B
wasm_max_pages:	1
log_slots:	3
```

`platform` is the build target (`linux`, `nuttx`, `dummy`); `version` is the git-derived SemVer baked in at compile time. The remaining fields are the fixed resource ceilings — any wapp can read them unprivileged to size itself to the host.

## `/` — TarFS application space

The wapp's root filesystem is **TarFS**: a read-only merge of the image's OCI layers (up to 4), newest-shadows-oldest. Any file packaged into the image appears here at its archived path.

- **Layer merge** — a path resolved in the newest layer that defines it; an O(log N) sorted index avoids linear scans.
- **Whiteouts** — a `.wh.<name>` entry in a newer layer logically deletes `<name>` from older layers.
- **Format** — POSIX ustar, including PAX and GNU long-name entries.
- **Read-only** — writes are rejected with `EROFS`. A wapp's writable storage is a **preopen** (a host directory bound into the namespace), not TarFS.

```bash
$ cat /assets/config.txt   # a file baked into the wapp image
...
```

## Config-mounted drivers

Beyond the fixed namespace above, a wapp sees whatever its launch config grants through three sections — device `drivers[]` (mounted at `/dev/<name>`), file/backend `mounts[]` (bound at an arbitrary `path`), and `sockets[]` (created at `/net/<name>`). The paths below follow from each section's addressing rule. The schema that grants them is the [Control Plane Reference](control-plane-reference.md) launch config.

| Driver | Section | Path | Purpose |
|--------|---------|------|---------|
| `wanted` | `drivers[]` | `/dev/wanted` | The control-plane namespace; privileged supervisors only. Fully specified in the [Control Plane Reference](control-plane-reference.md). |
| `null` | `drivers[]` | `/dev/null` | Bit bucket. |
| `platform` | `mounts[]` | chosen `path` | A bind mount of a host directory as a native WASI preopen. `options` set the host source (`src=`) and access mode (`ro`/`rw`); a `ro` mount rejects every write with `-EROFS`. As a *console* backing instead, `platform` redirects the engine's native stdio (fds 0/1/2). |
| `volume` | `mounts[]` | chosen `path` | An engine-managed persistent store bound as a native WASI preopen. The wapp names only a volume (`name=`, default `default`); the engine owns the host location and creates it on first use. Private per wapp by default; `shared` makes it a cross-wapp store (one store every wapp naming it sees). `ro`/`rw` set access mode. Persists across restarts and reboots. |
| `config` | `mounts[]` | chosen `path` (e.g. `/etc/config`) | Read-only config-file injection, reachable outside `/dev`. |
| `9p` | `mounts[]` | chosen `path` | 9P2000 client for an external FS plugin. |
| `socket` | `sockets[]` | `/net/<name>` | TCP / UDP / TLS streams; see below. |
| `log` | console slot | — | Console capture; output readable at `/dev/wanted/wapps/<name>/log`. |

### `socket` — the `/net/` network namespace

`/net/` routes to the socket driver. A `sockets[]` entry is created at `/net/<name>` (the name is the node label) and bound to the connection described by its `address` — a URL `<scheme>://<host>:<port>`:

| Scheme | Transport |
|--------|-----------|
| `tcp://host:port` | Plain TCP |
| `udp://host:port` | Plain UDP |
| `tcps://host:port` | TLS TCP (Linux only) |
| `udps://host:port` | DTLS UDP (Linux only) |

A wapp `open`s the `/net/<name>` node, then `read`/`write`s the stream and `close`s it; connection parameters come from the entry's `address`, not from the wapp. TLS is OpenSSL-backed on Linux; the NuttX sim has no TLS.

#### Testing a TLS socket

Stand up an OpenSSL test server, then point a wapp's socket `address` at it with `tcps://localhost:8889`:

```bash
# create a sample cert and key
openssl req -x509 -newkey rsa:2048 -keyout key.pem -out cert.pem -days 365 -nodes
# start a TLS server the wapp can connect to
openssl s_server -key key.pem -cert cert.pem -accept 8889
```

## See also

- [Control Plane Reference](control-plane-reference.md) — the `/dev/wanted` namespace in full, and the launch config that mounts drivers.
- [Wapp Authoring](wapp-authoring.md) — packaging files into the TarFS root and declaring preopens.
