---
title: "VFS Reference"
date: 2026-06-08T17:30:00+01:00
weight: 40
toc: true
description: "Every mountpoint and driver a wapp can see: the /dev, /net, /proc, and TarFS root namespaces."
---

A wapp's entire view of the world is the VFS. This page is the exhaustive reference for every path: which namespace serves it, how it is opened, and its read/write semantics. See [Architecture](architecture.md) for how the router dispatches between namespaces.

## `/dev/` — device namespace

`/dev/` prefix-routes to registered sub-drivers. Five are **always present** in every wapp; the rest are mounted only when the launch config grants them.

| Path | Driver | Access | Always present | Semantics |
|------|--------|--------|:--------------:|-----------|
| `/dev/null` | null | rw | yes | Reads return 0 bytes (EOF); writes are accepted and discarded. |
| `/dev/pipe/<name>` | pipe | rw | yes | Process-wide named-pipe IPC. See below. |
| `/dev/stdin` | stdio | r | yes | Stub; reads return EOF. |
| `/dev/stdout` | stdio | w | yes | Stub; writes are discarded. |
| `/dev/stderr` | stdio | w | yes | Stub; writes are discarded. |
| `/dev/platform` | platform | rw | config | Host filesystem access — the `platform` driver mounted at a path. (As a *console* backing instead, `platform` redirects the engine's native stdio.) |
| `/dev/wanted` | wanted | rw | config | The control-plane namespace; privileged supervisors only. See [Control Plane Reference](control-plane-reference.md). |

The stdio stubs exist so a wapp's standard descriptors always resolve; a wapp that wants real stdout is given a `platform` or `log` console in its launch config (see [Control Plane Reference](control-plane-reference.md)), not by writing to `/dev/stdout`.

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

## `/net/` — network namespace

`/net/` routes to the socket driver. The driver is mounted at one or more paths by the launch config (`/net/s`, `/net/ss`, `/net/manager` — the path is chosen by configuration, not fixed), each bound to a connection described by its `options` string:

| Option | Transport |
|--------|-----------|
| `t host port` | Plain TCP |
| `u host port` | Plain UDP |
| `T host port` | TLS TCP (Linux only) |
| `U host port` | TLS UDP (Linux only) |

A wapp `open`s the mounted path, then `read`/`write`s the stream and `close`s it; connection parameters come from the mount's `options`, not from the wapp. TLS is OpenSSL-backed on Linux; the NuttX sim has no TLS.

### Testing a TLS socket

Stand up an OpenSSL test server, then point a wapp's `socket` driver at it with options `T localhost 8889`:

```bash
# create a sample cert and key
openssl req -x509 -newkey rsa:2048 -keyout key.pem -out cert.pem -days 365 -nodes
# start a TLS server the wapp can connect to
openssl s_server -key key.pem -cert cert.pem -accept 8889
```

## `/proc/` — process namespace

A read-only, flat namespace exposing system state. Privileged entries are visible only when the engine config sets `system.privileged: true`; otherwise they are hidden from both reads and directory enumeration.

| Path | Access | Privileged | Content |
|------|--------|:----------:|---------|
| `/proc/wapps` | r | yes | Per-wapp state — name and status for each running wapp. |
| `/proc/memory` | r | yes | `heap_used` / `heap_total`, via `PlatformMemoryStats`. |
| `/proc/clock_quality` | r | no | Platform clock-quality metric. |

Each entry reads its value in one shot; a second read on the same fd returns EOF, regenerating on a fresh open.

## `/` — TarFS application space

The wapp's root filesystem is **TarFS**: a read-only merge of the image's OCI layers (up to 4), newest-shadows-oldest. Any file packaged into the image appears here at its archived path.

- **Layer merge** — a path resolved in the newest layer that defines it; an O(log N) sorted index avoids linear scans.
- **Whiteouts** — a `.wh.<name>` entry in a newer layer logically deletes `<name>` from older layers.
- **Format** — POSIX ustar, including PAX and GNU long-name entries.
- **Read-only** — writes are rejected with `EROFS`. A wapp's writable storage is a **preopen** (a host directory bound into the namespace), not TarFS.

```bash
$ cat /etc/role        # a file baked into the wapp image
reader
```

## See also

- [Control Plane Reference](control-plane-reference.md) — the `/dev/wanted` namespace in full.
- [Wapp Authoring](wapp-authoring.md) — packaging files into the TarFS root and declaring preopens.
