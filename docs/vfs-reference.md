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
- **Writes block by default.** A write short-writes into the space the ring has. On a full ring it sleeps and retries until the reader drains, under the same safety cap, then returns `EAGAIN`. `O_NONBLOCK` returns `EAGAIN` at once.
- **EOF** is returned only once a writer has attached and all writers have closed.
- **`poll()`** reports a reader readable when data is buffered, and readable with a hangup at EOF. A writer is writable while the ring has space.
- **`fcntl(F_SETFL, O_NONBLOCK)`** switches an open end between blocking and non-blocking.

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
| `/proc/memory` | r | yes | `heap_used` / `heap_total`, via `PlatformMemoryStats`; `store_free` / `store_total`; `wasm_pages_free` — WASM linear-memory pages the engine can still commit: every loaded wapp's headroom to its own ceiling, plus the full per-wapp ceiling for each free wapp slot. An image that declares its max equal to its initial memory reaches its ceiling as it instantiates and contributes no headroom, so the free slots are usually the whole figure. An uncapped build (`wasm_max_pages: 0`) counts no slot capacity — there is no page count to count. |
| `/proc/clock_quality` | r | no | Platform clock-quality metric. |
| `/proc/uptime` | r | no | `uptime_ms` — milliseconds since the engine started, the origin captured log lines are stamped from. |
| `/proc/net` | r (dir) | no | A directory: one flat file per `sockets[]` entry *this wapp* holds, named after the entry (`readdir` enumerates them). Reads the wapp's own per-wapp socket table, so a wapp can never see another wapp's sockets. |
| `/proc/net/<name>` | r | no | `connected` (`0`/`1`), `type` (the socket's scheme — known from config alone, always present), and `local` (the connected local endpoint, `<scheme>://<host>:<port>` or a bare path for `serial://`/`unix://` — present only once connected). |
| `/proc/wanted` | r | no | Engine identity and compile-time ceilings — `platform`, `version`, `supervisor_abi`, `max_wapps`, `max_wapp_name`, `max_path`, `wasm_stack`, `wasm_heap`, `wasm_worker_stack`, `wasm_max_pages`, `max_drivers`, `max_options`, `log_slots`, `max_layers`, `max_args`, `max_envs`, `reg_slots` (images the registry can hold; `0` where it is bounded only by the filesystem), `image_verify` and `image_verify_floor` (`enforcing` or `reporting` — the effective posture and the compiled-in floor a configuration cannot lower), `drivers` (the drivers available on this build), `digest` (present where the platform stamps a build-time image digest), and `serial` (present where the platform has a hardware serial to report). |

Each entry reads its value in one shot; a second read on the same fd returns EOF, regenerating on a fresh open.

`/proc/wanted` reports the engine itself as `key:\tvalue` lines, one per field — human-readable, split on the tab. Every field on it is **static for the life of the process**, so a reader may render it once and cache the result; a value that moves while the engine runs gets its own node (`/proc/uptime`, `/proc/memory`, `/proc/clock_quality`) rather than a line here.

```text
platform:	linux
version:	0.8.0+gf0d012c.20260713121818
supervisor_abi:	3
max_wapps:	3
max_wapp_name:	15 B
max_path:	256 B
wasm_stack:	8192 B
wasm_heap:	8192 B
wasm_worker_stack:	65536 B
wasm_max_pages:	1
max_drivers:	6
max_options:	128 B
max_layers:	4
max_args:	8
max_envs:	8
log_slots:	3
reg_slots:	0
drivers:	null log 9p config platform socket sha256 ed25519 inflate wanted
serial:	0123456789abcdef
```

`serial` is the unit's own hardware identity, stable across reflashes: the SPI
flash chip's factory id on ESP-IDF, the OTP device id on RP2350, and a
device-tree or DMI serial on Linux. It is never the Wi-Fi MAC — a caller may
derive a secret from this value, and the MAC is broadcast in every beacon
frame. The line is absent, rather than empty, on a platform with no source for
one: a container, a virtual machine with no DMI data, or a board whose
firmware reports nothing.

**Reading `serial` without writing a wapp**: boot the `wsh` debug supervisor
(`just supervisor-variant wsh && just build`, see [Platform
Guide](platform-guide.md#supervisor-variant)) and run `cat /proc/wanted` at
its prompt — see [`wapps/wsh/README.md`](../wapps/wsh/README.md) for the full
command set. Where that prompt reaches you differs by target:

- **AP-capable embedded targets (ESP-IDF, NuttX/RP2350), before a device is
  provisioned** — the console is UART (the `platform` console backing), so
  connect a serial terminal to it at the board's configured baud rate. This
  is the same console the production `:sheriff` supervisor uses for its
  provisioning prompt; reading `serial` this way means flashing `:wsh`
  instead, which is a factory-line or bench step, not something a device does
  once deployed.
- **Linux and OpenWRT** — the `platform` console backing is the engine
  process's own stdio, so an ordinary shell reaches it directly: run
  `wanted-cli` with the `:wsh` supervisor configured and the prompt appears
  on that same terminal, no serial cable needed.

This is how an operator learns the value the access-point passphrase derives
from, before a board ever leaves the bench: read `serial` once at
provisioning time, alongside the SSID a Wi-Fi-managing wapp prints when it
hosts an access point (`wifi-mgr: hosting "<ssid>"`, in its own wapp log —
see the `/proc/wapps/<name>` entries above), and record both on the unit's
label. A field operator never needs `serial` directly; the passphrase
derivation is a one-time, bench-side computation, not a runtime lookup.

`wasm_worker_stack` is the effective per-wapp worker thread native C stack (the
configured `WASM_WORKER_STACK_SIZE` after the platform's `PTHREAD_STACK_MIN`
floor); `max_drivers` / `max_options` size each launch-config drivers/mounts/sockets
section and the per-entry options blob. `max_layers` / `max_args` / `max_envs` bound
one wapp's filesystem layers, arguments and environment entries — a supervisor reads
them to refuse a deployment the engine would not accept. `drivers` lists the driver names a launch
config can request on this build — the platform-agnostic core plus the drivers
the running platform implements (e.g. `wifi ota` on ESP-IDF) and any linked in
from an out-of-tree tree (see the [Platform Guide](platform-guide.md)); naming
any other driver fails the launch with `-ENODEV`.

`digest` is the running image's build-time digest, 64 lowercase hex characters
(ESP-IDF stamps the ELF SHA-256 into the image descriptor). It identifies the
exact bytes that booted, thus a control plane confirming a firmware update
compares it rather than `version`, which two builds of one source tree can
share. The line is absent on a platform that stamps no digest.

`supervisor_abi` is the version of the contract between the engine and a supervisor wapp: the shape of the `/dev/wanted` control plane, its verbs, and the wapp states it reports. It is bumped only when a supervisor built against an earlier value would misread this engine — never for an ordinary release, and never for an addition an older supervisor can ignore. A supervisor reads it before acting on anything else and writes `rollback-supervisor` when it cannot support the value; an engine reporting no ABI line at all is assumed compatible.

`platform` is the build target (`linux`, `nuttx`, `esp-idf`, `dummy`); `version` is the git-derived SemVer baked in at compile time. The remaining fields are the fixed resource ceilings — any wapp can read them unprivileged to size itself to the host.

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
| `gpio` | `drivers[]` | `/dev/gpio/<name>/` | Digital I/O, one subtree per granted pin; see below. Backed by ESP-IDF and NuttX. On Linux the grant fails the launch with `-ENOSYS` until the libgpiod backing lands. |
| `uart` | `drivers[]` | `/dev/uart/<port>/` | A serial port: a `data` byte stream plus writable `baud` and `format`; see below. Backed by ESP-IDF and Linux. On NuttX the grant fails the launch with `-ENOSYS`. |
| `fb` | `drivers[]` | `/dev/fb/<screen>/` | A screen as a framebuffer: pixels written to `data`, made visible by `ctl` `flush`; see below. Screens are the headless in-memory buffers declared in `system.screens`. Built with `CONFIG_WANTED_VFS_FB`. |
| `input` | `drivers[]` | `/dev/input/<device>/` | Keyboard, encoder and text events as 8-byte records, one subtree per granted device; see below. Devices are the virtual ones declared in `system.inputs`, fed only through `inject`. Built with `CONFIG_WANTED_VFS_INPUT`. |
| `led` | `drivers[]` | `/dev/led/<name>/` | LEDs and backlights: brightness, fades and the `heartbeat` and `activity` triggers, one subtree per granted LED; see below. Pin LEDs are backed by ESP-IDF (LEDC PWM, GPIO) and by a state-only backing on Linux. Built with `CONFIG_WANTED_VFS_LED`. |
| `wifi` | `drivers[]` | `/dev/wifi/` | Wi-Fi station and access-point control; see below. Backed by ESP-IDF and NuttX; `-ENODEV` elsewhere. |
| `ota` | `drivers[]` | `/dev/ota` | A/B firmware update. `/dev/ota` is the control/status node — `write` one command per call (`begin` / `commit` / `abort` / `confirm` / `rollback`), `read` drains a status snapshot (`active_slot`, `status`, `pending_slot`, `last_failed_slot`, `boot_attempts`, and `pending_digest` — the staged image's own build-time digest, the value `/proc/wanted`'s `digest` reports once it boots, so confirming an update compares like with like; the line is absent when nothing is staged or the platform stamps none); `/dev/ota/slot` is the write-only streaming image sink for the inactive slot. End every `begin`: `commit` makes the staged image bootable, `abort` discards it. A session left open holds the slot, and every later `begin` answers `-EBUSY` until the board reboots. `rollback` reverts a booted image and reboots the board; it does not end a streaming write. Backed by ESP-IDF (`esp_ota_ops`) and Linux (slot directories under a boot root); `-ENOSYS` on NuttX. |
| `platform` | `mounts[]` | chosen `path` | A bind mount of a host directory as a native WASI preopen. `options` set the host source (`src=`) and access mode (`ro`/`rw`); a `ro` mount rejects every write with `-EROFS`. As a *console* backing instead, `platform` redirects the engine's native stdio (fds 0/1/2). |
| `volume` | `mounts[]` | chosen `path` | An engine-managed persistent store bound as a native WASI preopen. The wapp names only a volume (`name=`, default `default`); the engine owns the host location and creates it on first use. Private per wapp by default; `shared` makes it a cross-wapp store (one store every wapp naming it sees). `ro`/`rw` set access mode. Persists across restarts and reboots. |
| `config` | `mounts[]` | chosen `path` (e.g. `/etc/config`) | Read-only config-file injection, reachable outside `/dev`. |
| `9p` | `mounts[]` | chosen `path` | 9P2000 client for an external FS plugin. The `options` URL is `tcp://<host>:<port>`, `udp://<host>:<port>`, or `unix://<socket-path>` for a server on the same box. |
| `log` | `mounts[]` | chosen `path` | Read-only directory view of per-wapp captured logs. `<path>/<name>` reads wapp `<name>`'s ring-buffered output; the mount enumerates wapps with a live log slot. A `name=<wapp>` option scopes it to one wapp (default: all). The engine's own error channel appears as `.engine`, a name no image reference can carry; `name=.engine` scopes a grant to it alone. Grantable independently of `/dev/wanted`. Each line is prefixed `[+<ms>] ` — see [Log line stamps](#log-line-stamps). |
| `socket` | `sockets[]` | `/net/<name>` | TCP / UDP / TLS streams; see below. |
| `log` | console slot | — | Console capture: routes a wapp's stdout/stderr into its per-wapp log slot (read back via a `log` mount). |
| `pipe` | console slot | `/dev/pipe/<wapp>.<slot>` | Live console: backs a stdio slot with a named pipe a peer wapp can read at `/dev/pipe/<wapp>.<slot>` (or the `options` `name=`). `out`/`err` short-write, then wait on a full ring until the reader drains, and return `EAGAIN` at the safety cap, never dropping bytes; `in` reads a peer's writes. Distinct from `log` (buffered pull) — `pipe` is a live push to a peer. |

### Log line stamps

Every captured log line opens with `[+<ms>] `, the milliseconds elapsed since
the engine fixed its uptime origin at start:

```
[+12345] wanted: image install failed
[+12362] wanted: NoSpaceLeft
```

The stamp is boot-relative because a device need not have a trustworthy wall
clock — `/proc/clock_quality` reads `3` (uncalibrated) until something sets the
time, and an absolute stamp written before then would be wrong rather than
merely coarse. `/proc/uptime` reports the same counter as `uptime_ms`, so a
reader that pairs one read of it with its own clock resolves every stamp in a
ring to absolute time.

A line is closed by a newline, not by the `write()` that carried it: a producer
that flushes `"abc"` and then `"def\n"` yields one stamped line, and a line
still open when the ring is read carries the stamp it opened with. Output that
never emits a newline is stamped once, at its first byte.

The prefix is stored in the ring, so it counts against
`CONFIG_WANTED_LOG_CAP` (and `CONFIG_WANTED_LOG_PERSIST_CAP` for `.engine`)
like any other output.

### Preopen capability rights

Every preopen — the TarFS root, a `platform` bind mount, a `volume`, stdio — advertises WASI capability rights the wapp can read through `fd_fdstat_get`. A read-only grant (`ro`) advertises no write-class rights; libc caps a write request to fit before issuing the `path_open`, and a request that still exceeds a preopen's rights is refused with `ENOTCAPABLE`. This is defence in depth over the backing driver, which independently rejects any write that reaches it with `-EROFS`.

The TarFS root advertises the full right set: its read-only nature is enforced by the driver, so a write-open under `/` fails with `-EROFS`.

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
On a build without a backend the verdict read fails with `-ENOSYS`: on Linux
that means secure sockets deselected (`CONFIG_WANTED_VFS_SOCKET_TLS=n`, which
is what pulls OpenSSL in), and on ESP-IDF it is the port's current state
regardless of configuration.

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

### `gpio` — digital I/O

A `gpio` grant maps wapp-visible pin names onto the board's lines. Each granted
pin gets a subtree:

```
/dev/gpio/
  <name>/
    value      (rw on an output, r on an input)  "0" | "1"
    direction  (r)                               "in" | "out"
```

The grant names every pin, and nothing else is reachable:

```json
{ "name": "gpio", "options": "pins=boot0:4:out,nrst:5:out,btn:2:in" }
```

Each `pins=` entry is three colon-separated fields, `<name>:<address>:<direction>`,
optionally followed by `pull=up|down|none`, `drive=pp|od` and `init=0|1`. The address is the
backing's business — a GPIO number on ESP-IDF, a character-device path such as
`/dev/gpio0` on NuttX — and a wapp never sees it. It is one field, and `:` and
`,` are reserved: they separate fields and entries, so a backing addressing a
line as a chip plus an offset spells it `/dev/gpiochip0/17`, not `0:17`. A wapp built against
`/dev/gpio/boot0/value` therefore runs unchanged on every target: repinning a
board changes the launch config, not the image.

- `readdir /dev/gpio` lists exactly the granted pins. An ungranted name returns
  `-ENOENT` from every path beneath it. There is no default pin, and a missing,
  malformed, or empty `pins=` clause fails the launch.
- A `value` read returns `"0\n"` or `"1\n"`, then EOF on that descriptor. The
  value regenerates on a fresh open.
- A `value` write takes `"0"` or `"1"`, with or without a trailing newline. On a
  pin the grant made an input it returns `-EPERM`; any other payload is
  `-EINVAL`.
- `init=` is the level an output takes as the grant opens it, defaulting to
  `0`, and the backing writes it before the pad becomes an output. A line whose
  idle level is high — a reset line, a chip select — needs it: without it the
  pin drives low from the moment the wapp launches and holds the device it is
  wired to. On a pin the grant made an input it fails the launch.
- Direction, pull, and drive are fixed by the grant. `direction` is read-only,
  so a wapp cannot turn an input into an output and drive a line an external
  device is already driving. A backing that cannot honour a requested pull or
  drive mode fails the launch rather than serving `-ENOTSUP` in the field.
- Pin exclusivity across wapps is not enforced by the engine. Two wapps granted
  one line both configure it. Issue non-overlapping grants.

### `uart` — serial port

A `uart` grant hands one port to one wapp:

```
/dev/uart/
  <port>/
    data    (rw)  raw byte stream
    baud    (rw)  decimal rate, e.g. "921600"
    format  (rw)  <databits><parity><stopbits>, e.g. "8N1", "8E1", "7E2"
```

```json
{ "name": "uart", "options": "port=1,tx=1,rx=2,baud=57600,format=8E1" }
```

`port=` is required and names both the backing's port and the wapp-visible
directory. `baud=` and `format=` set the initial line configuration, defaulting
to `115200` and `8N1`. Every other key is platform addressing: `tx=`/`rx=` GPIO
numbers on ESP-IDF, `dev=/dev/ttyUSB0` on Linux. A key meaningless on the
running platform is rejected at launch, not ignored.

- A `data` read blocks until at least one byte arrives and returns **short** —
  it never waits to fill the caller's buffer. Open with `O_NONBLOCK` to get
  `-EAGAIN` on an empty receive buffer instead.
- **A blocking read has no wall-clock cap.** It returns on a byte or on
  `-EINTR`. An idle line is a UART's normal state and carries no information
  about a fault, so there is no elapsed time a timeout could be inferred from. A
  wapp that wants a deadline uses `poll()` with a timeout.
- `poll()` reports `data` readable once a byte has arrived. To learn that, it
  moves up to 32 received bytes into the driver, and the next read returns them
  first. `data` always reports writable. A line reconfiguration discards the
  moved bytes along with the receive buffer.
- `fcntl(F_SETFL, O_NONBLOCK)` switches an open `data` fd between blocking and
  non-blocking.
- A `data` write queues bytes and returns the count accepted, which may be
  short.
- `baud` and `format` are writable at runtime, because one link can carry two
  settings — a bootloader sync at 57600 8E1 and the framed channel that follows
  at 921600 8N1. A write drains the transmit buffer first, so a reconfiguration
  cannot truncate a byte already on the wire.
- **A write to `baud` or `format` discards the receive buffer.** Bytes received
  under the previous settings cannot be decoded under the new ones. A wapp that
  reconfigures mid-stream loses whatever was queued.
- A rate or format the backing cannot produce returns `-EINVAL`. The backing
  never selects the nearest achievable rate: that yields a link that looks
  configured and corrupts data.
- One wapp holds a port, exclusively; a second grant on a held port fails the
  launch. Routing several logical users onto one physical link is a broker
  wapp's job — it holds the grant and its peers reach it over `/dev/pipe`.

### `fb` — framebuffer

An `fb` grant hands one or more screens to one wapp as its writer:

```
/dev/fb/
  <screen>/
    info    (r)   "<width> <height> <format> <stride>\n"
    data    (rw)  pixels; the file offset is the byte offset
    ctl     (w)   "flush" | "flush <x> <y> <w> <h>" | "blank on" | "blank off"
```

```json
{ "name": "fb", "options": "screens=main" }
```

`screens=` lists the screens the wapp may touch, comma separated. A name the
engine does not know, a repeated name, and an empty entry fail the launch.
`observe` is reserved and never a screen name.

- `format` is `rgb565` or `rgb888`. `stride` is the byte length of one row.
- `data` takes positional I/O (`pread`/`pwrite`); plain `read` and `write` use
  and advance the descriptor's offset. An access is truncated at the end of the
  screen, and one starting past the end transfers 0 bytes.
- **A write changes the framebuffer, not the panel.** `flush` makes it visible
  and returns once the backing has handed the frame to the panel. A backing
  whose writes already reach the panel accepts `flush` and returns at once.
- A `ctl` line that is not one of the four forms, or a rectangle that is empty
  or leaves the screen, returns `-EINVAL`.
- One writer holds a screen; a second grant naming it fails the launch. A grant
  naming several screens takes all of them or fails.
- The engine converts, scales and draws nothing. Drawing is a library compiled
  into the wapp.

With `CONFIG_WANTED_VFS_FB_OBSERVE` an `observe` grant gets a read-only view:

```json
{ "name": "fb", "options": "screens=main,observe" }
```

```
/dev/fb/<screen>/
  info    (r)   as above
  data    (r)   the last flushed image
  damage  (r)   flushed rectangles, four little-endian u16 each: x y w h
```

- An observer has no `ctl`. After it attaches, its `data` never shows a pixel
  the writer has not flushed. The engine keeps a second copy of the screen while
  an observer exists and updates it on each `flush`, before reporting the
  rectangle.
- That copy starts as the screen's pixels when the first observer attaches, so
  an observer that starts late sees the screen. That first snapshot can include
  pixels the writer has written and not yet flushed.
- A `damage` read blocks until a flush is queued, returns whole records, and
  needs a buffer of at least 8 bytes (`-EINVAL` otherwise). `O_NONBLOCK` returns
  `-EAGAIN` when nothing is queued, and a stop ends a blocked read with
  `-EINTR`. `poll()` reports it readable while a record is queued.
- Each observer has its own queue of `CONFIG_WANTED_FB_DAMAGE_QUEUE` records.
  An observer that falls behind gets one full-screen record in place of its
  queue, so no flush goes unreported.
- Observers do not count against the writer, and may launch before it. At most
  `CONFIG_WANTED_FB_MAX_OBSERVERS` observe one screen; the next fails its launch.
- A build without observers fails an `observe` grant at launch. It is never
  read as a writer grant.

### `input` — keyboard, encoder and text events

An `input` grant hands one or more devices to one wapp as their owner:

```
/dev/input/
  <device>/
    events  (r)   event records, whole records per read
    info    (r)   "<types> keymap=<keymap>\n", e.g. "key text rel keymap=us"
```

```json
{ "name": "input", "options": "devices=kbd,keymap=us" }
```

`devices=` lists the devices, comma separated, and `keymap=` follows it. A name
the engine does not know, a repeated or empty name, a missing `keymap=`, and a
keymap other than the one each device declares fail the launch.

Each record is 8 bytes, little-endian:

| Offset | Field | Meaning |
|---|---|---|
| 0 | `sync` | `1` on the last record of a batch |
| 1 | `type` | `EV_SYN` `0x00`, `EV_KEY` `0x01`, `EV_REL` `0x02`, `EV_TEXT` `0xF0` |
| 2 | `code` (u16) | `SYN_DROPPED` `3`; a Linux key code; a Linux relative axis; `0` |
| 4 | `value` (i32) | key state `1` down, `0` up, `2` repeat; a signed delta; a Unicode scalar |

- `events` returns whole records and needs a buffer of at least 8 bytes
  (`-EINVAL` otherwise). It blocks until a record is queued. `O_NONBLOCK`
  returns `-EAGAIN` when the queue is empty, and a stop ends a blocked read with
  `-EINTR`. `poll()` reports it readable while a record is queued.
- A device queues 64 records. On overflow the engine clears the queue and queues
  one `EV_SYN` record with code `SYN_DROPPED`. A reader treats every held key as
  released on it.
- `EV_TEXT` carries the Unicode character the backing composed, with layers and
  Shift applied. A wapp reads `EV_TEXT` for what types and `EV_KEY` for keys
  that produce no character, and skips any `type` it does not know.
- One wapp owns a device. A second grant naming it fails the launch, and a grant
  naming several devices takes all of them or fails. Records queued before the
  owner launched are dropped.
- A device is declared in `system.inputs` and has no hardware behind it. The
  engine merges no devices and reads no keyboard itself.

With `CONFIG_WANTED_VFS_INPUT_INJECT` a trailing `inject` token grants the
write side of the devices instead:

```json
{ "name": "input", "options": "devices=kbd,keymap=us,inject" }
```

```
/dev/input/<device>/
  info    (r)   as above
  inject  (w)   event records, queued for the owner
```

- An `inject` grant has no `events` node and does not count as the owner, so it
  may launch before or after the owner and several may coexist.
- A write takes whole records and queues each one as if the device produced it.
  A length that is not a multiple of 8 returns `-EINVAL` and queues nothing.
- Injection is as sensitive as typing on the device. Enable it only in test and
  simulator builds.
- A build without injection fails an `inject` grant at launch. It is never read
  as an owner grant.

### `led` — LEDs and backlights

A `led` grant hands one or more LEDs to one wapp:

```
/dev/led/
  <name>/
    brightness      (rw)  the level the LED shows now, 0..max_brightness
    max_brightness  (r)   1 for an on/off LED, 255 for a PWM pin LED
    trigger         (rw)  none | heartbeat | activity
    idle_ms         (rw)  activity trigger: milliseconds of inactivity, 10000 by default
    idle_level      (rw)  activity trigger: the level it fades to, 0 by default
    ctl             (w)   fade <level> <ms>
```

```json
{ "name": "led", "options": "leds=kbd:46:pwm,display,activity=kbd:kbd,activity=display:kbd" }
```

`leds=` lists the entries, comma separated. An entry with colons,
`<name>:<address>:<mode>`, opens a pin LED: `mode` is `pwm` or `onoff`, and the
backing interprets `address` (on ESP-IDF a decimal GPIO number). A bare `<name>`
selects a device the board registered with `LedDeviceRegister`
(`src/include/vfs-led.h`). `activity=<led>:<input>` ties a granted LED to an
input device. Names are `[A-Za-z0-9_-]`, at most 15 characters. A repeated
name, a malformed entry, an unknown board device, an address the backing
refuses and an unknown input device fail the launch.

- `brightness` sets the level at once and reads the level the LED shows now,
  which moves during a fade. A value above `max_brightness`, or anything that
  is not decimal digits, returns `-EINVAL`.
- `ctl` takes one line, `fade <level> <ms>`. The LED ramps from its current
  level to `<level>` over `<ms>` milliseconds, in hardware where the backing
  has it. A duration of 0 sets the level at once. Any other line returns
  `-EINVAL`.
- An on/off LED has `max_brightness` 1, and a fade on it switches at the end.
- `trigger` starts at `none`, where `brightness` alone controls the LED.
  - `heartbeat` runs from the engine loop, so it stops when the loop stops. A
    beat is a fade up and down on a backing with hardware fades, and an on
    period of one loop pass otherwise. While it runs, a write to `brightness`
    or `ctl` returns `-EBUSY`. Writing another trigger leaves the LED dark.
  - `activity` holds the level last written to `brightness` or `ctl`. After
    `idle_ms` with no event on its input device it fades to `idle_level` over
    1 s, never brighter than the level held. The next event fades back over
    100 ms. A write to `brightness` or `ctl` counts as activity. An `idle_ms`
    of 0 turns the fade off. Without an `activity=` entry for the LED, writing
    `activity` returns `-EINVAL`.
- One wapp owns a LED. A second grant naming it fails the launch with
  `-EBUSY`, and a grant naming several LEDs takes all of them or fails.
- A pin LED exists while its grant does. A board device stays registered, and
  its trigger and idle settings return to their defaults when the grant ends.
- `observe` grants are not available: such a grant fails the launch.

The engine loop calls `LedTick()` about once a second, so idle and heartbeat
timing has that resolution. Input events reach the activity trigger at once.

### `wifi` — Wi-Fi station and access point

A `wifi` grant reaches the board's one radio:

```
/dev/wifi/
  status  (r)  link state, one line
  scan    (r)  results of the last scan, then EOF
  ctl     (w)  scan | connect <ssid> [pass] | disconnect | ap_start | ap_stop
```

There is no per-resource identity in the path — a device has one radio — so
`readdir /dev/wifi` always lists the same three entries.

- **`status`** reports one line, re-arming on a fresh `read` whenever the state
  changes (the same per-descriptor latch `gpio`'s `value` uses):
  - `disconnected` — no intent stored.
  - `connecting` — an association attempt is in flight, whether the first one
    or a driver-initiated retry; the two are not distinguished.
  - `connected <ssid> <ip>` — associated and addressed.
  - `ap <ssid>` — hosting an access point (ESP-IDF only; see below).
- **`scan`**, after `write "scan"` on `ctl`, streams the result as one
  `<ssid> <bssid> <rssi>` line per visible AP, then EOF. A `read` before any
  scan has run on that descriptor also returns EOF.
- **`ctl`** takes one command per write:
  - `scan` — runs a blocking scan; the result appears on `scan`.
  - `connect <ssid> [pass]` — stores the credentials as the live intent and
    associates. An empty `pass` configures an open network.
  - `disconnect` — clears the stored intent and drops the association.
  - `ap_start` — hosts an access point with driver-derived credentials (below).
    Replaces whatever intent — station or AP — was previously stored.
  - `ap_stop` — stops a running AP and returns to station mode. Idempotent.

**The driver owns reconnection, and never gives up.** A successful `connect`
persists; an unsolicited disconnect re-associates automatically, with a capped
backoff, for as long as that intent stands — no wapp involvement, no relaunch.
`disconnected` therefore means "no intent stored," never "gave up." There is
exactly one live intent at a time: `connect`, `ap_start` and `ap_stop` each
replace whatever was stored. A successful station association does not
implicitly stop a running AP; only `ap_stop` or a fresh `connect` does.

**AP credentials never cross the wapp boundary.** `ap_start` takes no
arguments — the driver derives both fields from the board's hardware serial
(`/proc/wanted`'s `serial:` line): the SSID is a compiled-in prefix plus the
low 24 bits of the serial in hex, carrying no secret; the passphrase is
HMAC-SHA256 of the serial, rendered as its first 16 hex digits. A board
reporting no serial cannot host an AP — `ap_start` answers `-ENODEV` rather
than falling back to a fixed passphrase.

**Only ESP-IDF hosts an access point.** NuttX's `bcm43xxx` driver has no AP
write path, so `ap_start` and `ap_stop` both answer `-ENODEV` there
unconditionally — never silently ad-hoc mode. A board with no `/dev/wifi`
grant at all gets the same `-ENODEV` shape from the missing driver.

Link state is module-level, not per-descriptor: it outlives the wapp that
raised it, which is what lets a fire-and-forget wapp bring up a link or an AP
and exit. Wi-Fi credentials themselves are not persisted by the driver across
a reboot — a caller that wants a connection to survive power-cycling writes
them somewhere durable and re-issues `connect` on the next boot.

### `socket` — the `/net/` network namespace

`/net/` routes to the socket driver. A `sockets[]` entry is created at `/net/<name>` (the name is the node label) and carries the transport described by its `address` — a URL, `<scheme>://<host>:<port>` for the network schemes, or a bare path for `serial://` / `unix://`. The entry's `role` decides the direction: `connect` (the default) reaches the address, `listen` binds it:

| Scheme | Transport |
|--------|-----------|
| `tcp://host:port` | Plain TCP |
| `udp://host:port` | Plain UDP |
| `tcps://host:port` | TLS TCP — Linux (OpenSSL); ESP-IDF and the NuttX sim (shared raw-mbedTLS layer; no CA bundle provisioned, so encrypted but unauthenticated) |
| `udps://host:port` | DTLS UDP (Linux only) |
| `serial:///dev/ttyACM0` | A local point-to-point byte-stream device — a UART or USB-CDC — in place of a network connection; a bare device path, no host or port |
| `unix:///run/some.sock` | AF_UNIX stream socket — a bare filesystem path, no host or port. `CONFIG_WANTED_VFS_SOCKET_UNIX`, hosted platforms only (Linux/OpenWRT); the option is a no-op on NuttX/ESP-IDF, which have no AF_UNIX family |

A wapp `open`s the `/net/<name>` node, then `read`/`write`s the stream and `close`s it; connection parameters come from the entry's `address`, not from the wapp. On NuttX, TLS is available where the board config enables `CONFIG_SYSTEM_WANTED_TLS` (the sim `wanted` config does); a build without it rejects the secure schemes at wapp launch.

A read blocks until data arrives, as the pipe and serial drivers do; `O_NONBLOCK` answers `-EAGAIN` on an empty receive buffer instead of waiting.

`poll()` reports a connection readable when data is buffered, including decrypted TLS data, and readable with a hangup when the peer has closed. A listener is readable when a connection waits to be accepted. An outbound socket that has not connected yet connects on its first `poll()`, as it would on its first read or write; a failed connect is that fd's poll error.

`fcntl(F_SETFL, O_NONBLOCK)` changes the blocking mode of the whole socket entry: a listener and every connection accepted from it share one mode.

#### Serving on a socket

An entry with `"role": "listen"` binds its `address` instead of connecting to it, and the wapp becomes the server. The role is a build option (`CONFIG_WANTED_VFS_SOCKET_LISTEN`); where it is absent, a config asking for it fails the launch.

```json
{ "name": "http", "address": "tcp://0.0.0.0:8080", "role": "listen", "backlog": 4, "max_conns": 2 }
```

On a stream transport, `/net/<name>` is the listener: opening it binds and listens, and `sock_accept` returns a **new fd** carrying one connection. That fd is a wapp fd like any other — `fd_read`, `fd_write`, `sock_recv`, `sock_send` and `close` all address the connection alone, so several connections are served at once without interfering, and closing one leaves the rest live. The listener fd itself carries no payload (a read returns `ENOTCONN`), and `max_conns` (up to the build's `VFS_SOCKET_MAX_CONNS`) caps how many connections are open at once; past it, accept returns `ENFILE`.

On a datagram transport there is no accept step: `/net/<name>` is the bound socket, a read takes the next datagram, and a write answers whoever that datagram came from. `backlog`/`max_conns` do not apply and are rejected.

A secure transport cannot listen — accept-side TLS needs a server certificate and key, and the launch config supplies neither, so `tcps`/`udps` with `role: listen` is rejected at launch rather than served unencrypted.

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
- [Error Reference](error-reference.md) — what each errno these operations return means in engine context.
