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

A read-only namespace exposing system state. Privileged entries are visible only when the engine config sets `system.privileged: true`; otherwise they are hidden from both reads and directory enumeration.

| Path | Access | Privileged | Content |
|------|--------|:----------:|---------|
| `/proc/wapps` | r (dir) | yes | A directory: one subdirectory per running wapp (`readdir` enumerates them). Each `/proc/wapps/<name>/` exposes the read-only status leaves below. |
| `/proc/wapps/<name>/state` | r | yes | Lifecycle token (`running`, `exited`, `failure`, …) for a running/terminal instance. |
| `/proc/wapps/<name>/image` | r | yes | The registry image the instance runs. |
| `/proc/wapps/<name>/version` | r | yes | The image's version tag (opaque string). |
| `/proc/wapps/<name>/id` | r | yes | Engine-assigned wapp id (decimal). |
| `/proc/wapps/<name>/exit_code` | r | yes | WASI exit code (authoritative when `state == exited`; else the sentinel `-1`). |
| `/proc/wapps/<name>/memory` | r | yes | Per-wapp WASM linear-memory accounting: `linear_cur` / `linear_max` (bytes) and `pages_cur` / `pages_max`. |
| `/proc/memory` | r | yes | `heap_used` / `heap_total`, via `PlatformMemoryStats`. |
| `/proc/clock_quality` | r | no | Platform clock-quality metric. |
| `/proc/wanted` | r | no | Engine identity and compile-time ceilings — `platform`, `version`, `max_wapps`, `max_wapp_name`, `max_path`, `wasm_stack`, `wasm_heap`, `wasm_worker_stack`, `wasm_max_pages`, `max_drivers`, `max_options`, `log_slots`, and `drivers` (the drivers available on this build). |

Each entry reads its value in one shot; a second read on the same fd returns EOF, regenerating on a fresh open.

`/proc/wanted` reports the engine itself as `key:\tvalue` lines, one per field — human-readable, split on the tab:

```text
platform:	linux
version:	0.8.0+gf0d012c.20260713121818
max_wapps:	3
max_wapp_name:	15 B
max_path:	256 B
wasm_stack:	8192 B
wasm_heap:	8192 B
wasm_worker_stack:	65536 B
wasm_max_pages:	1
max_drivers:	6
max_options:	128 B
log_slots:	3
drivers:	null log 9p config platform socket sha256 ed25519 inflate wanted
```

`wasm_worker_stack` is the effective per-wapp worker thread native C stack (the
configured `WASM_WORKER_STACK_SIZE` after the platform's `PTHREAD_STACK_MIN`
floor); `max_drivers` / `max_options` size each launch-config drivers/mounts/sockets
section and the per-entry options blob. `drivers` lists the driver names a launch
config can request on this build — the platform-agnostic core plus the drivers
the running platform implements (e.g. `gpio wifi` on NuttX); naming any other
driver fails the launch with `-ENODEV`.

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
| `sha256` | `drivers[]` | `/dev/sha256` | Streaming SHA-256 digest device; see below. |
| `ed25519` | `drivers[]` | `/dev/ed25519` | Ed25519 signature-verification device; see below. |
| `inflate` | `drivers[]` | `/dev/inflate` | Streaming gzip decompression device; see below. |
| `gpio` | `drivers[]` | `/dev/gpio` | A GPIO pin as a text level node — `write "1"/"0"` drives it high/low, `read` returns `"0\n"/"1\n"`. The engine does the GPIO ioctl; the wapp uses only WASI. Backed by the host GPIO character device on NuttX (default `/dev/gpio0`, overridable via `options`). NuttX only — naming it on a platform without GPIO (Linux) fails the launch with `-ENODEV`. |
| `wifi` | `drivers[]` | `/dev/wifi` | Wi-Fi station control as a text node. `write "scan"` starts a scan (following reads stream one `<ssid> <bssid> <rssi>` line per AP, then EOF); `write "connect <ssid> <pass>"` associates (WPA2-PSK) and runs DHCP; `write "disconnect"` drops the association; a plain `read` returns one status line — `connected <ssid> <ip>` or `disconnected`. The engine drives the radio (WAPI on NuttX, `esp_wifi` on ESP-IDF); the wapp stays pure WASI. NuttX and ESP-IDF only — `-ENODEV` elsewhere. |
| `ota` | `drivers[]` | `/dev/ota` | A/B firmware update. `/dev/ota` is the control/status node — `write` one command per call (`begin` / `commit` / `confirm` / `rollback`), `read` drains a status snapshot (active slot, confirmed state, pending swap); `/dev/ota/slot` is the write-only streaming image sink for the inactive slot. ESP-IDF only (`esp_ota_ops`) — `-ENODEV` elsewhere. |
| `platform` | `mounts[]` | chosen `path` | A bind mount of a host directory as a native WASI preopen. `options` set the host source (`src=`) and access mode (`ro`/`rw`); a `ro` mount rejects every write with `-EROFS`. As a *console* backing instead, `platform` redirects the engine's native stdio (fds 0/1/2). |
| `volume` | `mounts[]` | chosen `path` | An engine-managed persistent store bound as a native WASI preopen. The wapp names only a volume (`name=`, default `default`); the engine owns the host location and creates it on first use. Private per wapp by default; `shared` makes it a cross-wapp store (one store every wapp naming it sees). `ro`/`rw` set access mode. Persists across restarts and reboots. |
| `config` | `mounts[]` | chosen `path` (e.g. `/etc/config`) | Read-only config-file injection, reachable outside `/dev`. |
| `9p` | `mounts[]` | chosen `path` | 9P2000 client for an external FS plugin. |
| `log` | `mounts[]` | chosen `path` | Read-only directory view of per-wapp captured logs. `<path>/<name>` reads wapp `<name>`'s ring-buffered output; the mount enumerates wapps with a live log slot. A `name=<wapp>` option scopes it to one wapp (default: all). Grantable independently of `/dev/wanted`. |
| `socket` | `sockets[]` | `/net/<name>` | TCP / UDP / TLS streams; see below. |
| `log` | console slot | — | Console capture: routes a wapp's stdout/stderr into its per-wapp log slot (read back via a `log` mount). |
| `pipe` | console slot | `/dev/pipe/<wapp>.<slot>` | Live console: backs a stdio slot with a named pipe a peer wapp can read at `/dev/pipe/<wapp>.<slot>` (or the `options` `name=`). `out`/`err` are lossy writers (drop oldest on a full ring); `in` reads a peer's writes. Distinct from `log` (buffered pull) — `pipe` is a live push to a peer. |

### `sha256` — streaming digest device

Each open of `/dev/sha256` starts a fresh digest stream: `write` feeds message
bytes (any chunking), and the first `read` finalizes the digest and returns it
as 64 lowercase hex characters (partial reads resume where they left off; a
drained stream reads 0). Once read, the stream is sealed — further writes fail
with `-EINVAL`; `close` releases it, and a new `open` starts the next digest.
Two streams may be open concurrently per wapp; a third open fails with
`-EBUSY`. This lets a wapp verify content digests without carrying SHA-256
code, its constant table, or block buffers in its own linear memory.

```c
int fd = open("/dev/sha256", O_RDWR);
write(fd, data, len);              /* repeat while streaming */
char hex[64];
read(fd, hex, sizeof(hex));        /* "ba7816bf..." */
close(fd);
```

### `ed25519` — signature-verification device

Each open of `/dev/ed25519` performs one verification. The write stream is
framed: the first 32 bytes are the raw Ed25519 public key, the next 64 bytes
the signature, and everything after is the message (streamed in any chunking,
up to 64 KiB). `read` returns the verdict as a text token — `ok` when the
signature verifies, `fail` when it does not — and seals the stream. Reading
before the 96-byte key+signature header is complete fails with `-EINVAL`; one
verification is in flight at a time per wapp (second open: `-EBUSY`).

The engine holds no keys: the wapp supplies the public key it trusts, so key
custody stays with the caller and the engine only runs the curve arithmetic —
through `PlatformEd25519Verify`, which a platform backs with its crypto
library or hardware: OpenSSL on Linux, the vendored `orlp/ed25519` on NuttX.
On a build without a backend (Linux with `SECURE_SOCKETS=0`, or the current
ESP-IDF port) the verdict read fails with `-ENOSYS`.

### `inflate` — streaming gzip decompression device

Each open of `/dev/inflate` decompresses one gzip member. The write stream is
length-prefixed: the first 4 bytes declare the compressed member size (LE u32,
gzip header and trailer included), then the member bytes follow in any
chunking. Reads drain the decompressed output as it becomes available; a
**short write** means the internal output buffer is full — read before writing
more. Reading mid-member with nothing decoded yet returns `-EAGAIN`; when the
member is fully decoded and drained, reads return 0. The gzip trailer's CRC32
and length are validated — malformed or truncated input fails the stream with
`-EIO` until close. Writing past the declared size fails with `-EFBIG`; one
member decode is in flight at a time per wapp (second open: `-EBUSY`).

```c
int fd = open("/dev/inflate", O_RDWR);
uint8_t pfx[4] = { len & 0xff, (len >> 8) & 0xff,
                   (len >> 16) & 0xff, (len >> 24) & 0xff };
write(fd, pfx, 4);
while (fed < len) {
    int w = write(fd, gz + fed, len - fed);   /* short write: drain first */
    if (w > 0) fed += w;
    while ((r = read(fd, out, sizeof(out))) > 0)
        consume(out, r);                       /* -EAGAIN: keep feeding */
}
while ((r = read(fd, out, sizeof(out))) > 0)
    consume(out, r);                           /* r == 0: member complete */
close(fd);
```

The size prefix is part of the contract because the decoder cannot resume
after exhausting its *input* mid-symbol (output pauses are fine): declaring
where the member ends lets the engine decode eagerly with a safe input margin
and finish deterministically. Callers always know the compressed size (a
fetched blob's length, a file's size). The 32 KiB DEFLATE history window lives
in engine memory for the lifetime of the open — the wapp carries neither the
window nor the inflate code.

### `socket` — the `/net/` network namespace

`/net/` routes to the socket driver. A `sockets[]` entry is created at `/net/<name>` (the name is the node label) and bound to the connection described by its `address` — a URL, `<scheme>://<host>:<port>` for the network schemes or `serial://<device-path>` for a local device:

| Scheme | Transport |
|--------|-----------|
| `tcp://host:port` | Plain TCP |
| `udp://host:port` | Plain UDP |
| `tcps://host:port` | TLS TCP — Linux (OpenSSL); ESP-IDF and the NuttX sim (shared raw-mbedTLS layer; no CA bundle provisioned, so encrypted but unauthenticated) |
| `udps://host:port` | DTLS UDP (Linux only) |
| `serial:///dev/ttyACM0` | A local point-to-point byte-stream device — a UART or USB-CDC — in place of a network connection; a bare device path, no host or port |

A wapp `open`s the `/net/<name>` node, then `read`/`write`s the stream and `close`s it; connection parameters come from the entry's `address`, not from the wapp. On NuttX, TLS is available where the board config enables `CONFIG_SYSTEM_WANTED_TLS` (the sim `wanted` config does); a build without it rejects the secure schemes at wapp launch.

A `serial://` socket puts the device in raw mode and flushes its RX buffer on open, so a request/response exchange starts from a clean stream. It assumes the device delivers a reliable, ordered byte stream (true for a UART or USB-CDC); a lossy, unordered link needs its own framing/retry layer on top. It is how the Sheriff↔Deputy control-plane link runs on a board with no network stack — see the [Platform Guide](platform-guide.md).

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
