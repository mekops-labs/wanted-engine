---
title: "Configuration Reference"
date: 2026-06-08T17:30:00+01:00
weight: 60
toc: true
description: "The engine's JSON config: every field, its type, default, and effect — system flags and the supervisor's image and launch params."
---

The engine takes a single JSON config that decides one thing: which supervisor to boot and how. Everything else — installing and launching wapps — happens at runtime through the [control plane](control-plane-reference.md).

The schema itself is platform-independent, but the concrete examples, defaults, and driver options in this reference assume the **Linux build** — TLS sockets, `localhost` socket addresses, and host-filesystem preopens. Other targets differ: the NuttX port has no TLS, and host paths resolve against the simulator's filesystem. See the [Platform Guide](platform-guide.md).

## Loading order

```bash
wanted-cli                       # the configured default launch config
wanted-cli configs/example_config.json   # explicit config file
```

1. **CLI path argument** — if given, the JSON file is read and used verbatim.
2. **Compiled-in default** — with no argument, the engine uses the launch config selected at build time (`WANTED_DEFAULT_CONFIG`, default `configs/example_config.json`), compiled into the binary. See the [Platform Guide](platform-guide.md) for selecting it.
3. **Compiled-in supervisor params** — when the config omits `supervisor.params`, the engine applies the defaults baked into `src/default_supervisor_cfg.json.h`: a plain-TCP control socket at `localhost:8888`, the `wanted` control plane, and a `/var/lib/sheriff` preopen.

A minimal `{"system": {}}` is a valid config.

## Top-level schema

```json
{
  "system": {
    "privileged": false
  },
  "supervisor": {
    "imagePath": "./wasm/supervisor/sheriff/supervisor.tar",
    "params": {
      "console": { "in": {...}, "out": {...}, "err": {...} },
      "drivers": [ { "name": "...", "options": "..." } ],
      "mounts":  [ { "name": "...", "path": "...", "options": "..." } ],
      "sockets": [ { "name": "...", "address": "...", "role": "connect|listen" } ]
    }
  }
}
```

| Field | Type | Default | Effect |
|-------|------|---------|--------|
| `system.privileged` | boolean | `false` | Enables the privileged `/proc` entries (`wapps`, `memory`). When false they are hidden from reads and enumeration. |
| `supervisor.imagePath` | string | (build option) | Path to the supervisor TAR image. Overrides the compiled-in default. |
| `supervisor.params` | object | (compiled-in) | The supervisor's own launch config — same schema as a wapp `config` node. |

### `supervisor.imagePath` resolution

The supervisor image is resolved in priority order:

1. `supervisor.imagePath` in the config (runtime override, no rebuild).
2. The `CONFIG_WANTED_SUPERVISOR_*` Kconfig choice (compile-time default), or `CONFIG_WANTED_SUPERVISOR_IMAGE_PATH` when a package installs the image elsewhere.
3. `./wasm/supervisor/sheriff/supervisor.tar`.

### `supervisor.params` — launch config

The `params` object is the supervisor wapp's launch config, identical in shape to what a supervisor writes to a wapp `config` node:

The launch config addresses resources through three purpose-specific sections, each addressed the way the resource actually is:

| Field | Type | Notes |
|-------|------|-------|
| `console` | object | Slots `in` / `out` / `err`, each a driver spec backing the wapp's stdio. |
| `drivers` | array | Up to `MAX_DRIVERS_CNT` (default 6, profile-tunable) device singletons. Each mounts at `/dev/<name>` — the name determines the mount, so no `path`. |
| `mounts` | array | Up to `MAX_DRIVERS_CNT` file/backend drivers, each bound at an arbitrary absolute `path` outside `/dev` and `/net`. |
| `sockets` | array | Up to `MAX_DRIVERS_CNT` named sockets, each created at `/net/<name>`; the transport is the entry's `address` and the direction its `role`. |

Entry shapes per section:

| Section | Keys | Notes |
|---------|------|-------|
| `console.*` | `name`, `options` | Driver backing the stdio slot. |
| `drivers[]` | `name`, `options` | Device driver; mounted at `/dev/<name>`. A `path` is rejected. |
| `mounts[]` | `name`, `path`, `options` | `path` is required, absolute, and must not fall under `/dev` or `/net`. |
| `sockets[]` | `name`, `address`, `role`, `backlog`, `max_conns` | `name` is the `/net` node label; `address` is the transport URL. `role` is `connect` (default) or `listen`; `backlog` and `max_conns` bound a stream listener and are rejected elsewhere. A `path` is rejected. |

## Driver name registry

`name` selects one of the engine's built-in drivers. Some are platform-specific (`wifi` exists on NuttX and ESP-IDF, `ota` on ESP-IDF); naming a driver the running platform does not implement fails the launch with `-ENODEV`. `gpio` resolves everywhere, but it needs a backing for the lines behind it: a grant on a target with none fails the launch with `-ENOSYS`. Read `/proc/wanted` (`drivers` field) for the names available on a given build.

| `name` | Section | Purpose | `options` example |
|--------|---------|---------|-------------------|
| `null` | `drivers` | Bit bucket at `/dev/null`. | — |
| `sha256` | `drivers` | Streaming SHA-256 digest device at `/dev/sha256` — writes feed message bytes, the first read returns the digest as 64 hex characters. | — |
| `ed25519` | `drivers` | Ed25519 signature verification at `/dev/ed25519` — write public key + signature + message, read back `ok`/`fail`. `-ENOSYS` on a build without a crypto backend. | — |
| `inflate` | `drivers` | Streaming gzip decompression at `/dev/inflate` — a 4-byte LE size prefix, then the member; reads drain the decompressed output. | — |
| `gpio` | `drivers` | Digital I/O at `/dev/gpio/<name>/`, one subtree per granted pin, each with `value` and a read-only `direction`. `pins=` is required: a missing, malformed, or empty clause fails the launch. Backed by ESP-IDF and NuttX. | `pins=led:21:out,btn:2:in` |
| `wifi` | `drivers` | Wi-Fi station control at `/dev/wifi` as a text node: `write "scan"` / `"connect <ssid> <pass>"` / `"disconnect"`; reads stream scan results or a status line. NuttX and ESP-IDF only. | — |
| `ota` | `drivers` | A/B firmware update: `/dev/ota` control/status node (`begin`/`commit`/`confirm`/`rollback`), `/dev/ota/slot` streaming image sink. ESP-IDF only. | — |
| `log` | console slot | Ring-buffer console; output captured per-wapp and read back through a `log` mount. | — |
| `log` | `mounts` | Read-only directory view of per-wapp captured logs at `path`: `<path>/<name>` reads wapp `<name>`'s ring. Grantable independently of `/dev/wanted`. | `name=app1` |
| `pipe` | console slot | Live console: backs the slot with a named pipe in the shared store, so a peer wapp reads the stream at `/dev/pipe/<name>`. The pipe is auto-named `<wapp>.<slot>` (e.g. `app.out`) unless `options` pins `name=`. `out`/`err` are lossy (drop oldest on a full ring so an unread console never wedges the wapp); `in` reads a peer's writes. | `name=feed` |
| `platform` | console slot / `mounts` | As a console slot: the engine's native stdio (fds 0/1/2). In `mounts[]`: a bind mount of a host directory as a native WASI preopen at `path`; `options` set the host source and access mode. | `src=/etc/app,ro` |
| `volume` | `mounts` | An engine-managed persistent store mounted at `path`. The engine owns the host location, so the wapp names only a volume — no host path. Private per wapp by default; `shared` makes it a cross-wapp store. Portable across hosts. | `name=cache` |
| `socket` | `sockets` | TCP/UDP, plain or TLS, outbound or listening. The transport is the entry's `address`. | `tcp://localhost:8888` |
| `9p` | `mounts` | 9P2000 client for an external FS plugin. The `options` URL picks the transport: `tcp`/`udp` to reach a server over the network, `unix` to reach one on the same box over a filesystem socket. | `unix:///run/uci-9p.sock` |
| `config` | `mounts` | Read-only config-file injection (e.g. mounted at `/etc/config`). | `{"config_file":"/config.json"}` |
| `wanted` | `drivers` | The control-plane namespace at `/dev/wanted` (privileged). | — |

A socket `address` is a URL, where the scheme picks the transport: `<scheme>://<host>:<port>` with `tcp`/`udp` (plain) or `tcps`/`udps` (TLS/DTLS), or `serial://<device-path>` (a local UART / USB-CDC byte-stream device in place of a network connection). See the [VFS Reference](vfs-reference.md).

### Listening sockets

`"role": "listen"` makes the entry a server: `address` is then the **bind** endpoint the wapp serves on.

```json
"sockets": [
  { "name": "http",  "address": "tcp://0.0.0.0:8080", "role": "listen", "backlog": 4, "max_conns": 2 },
  { "name": "coap",  "address": "udp://0.0.0.0:5683", "role": "listen" }
]
```

| Field | Applies to | Effect |
|-------|-----------|--------|
| `role` | every entry | `connect` (default) opens an outbound connection; `listen` binds the address. Any other value fails the launch. |
| `backlog` | `listen` + `tcp` | Connection queue depth, default 4. |
| `max_conns` | `listen` + `tcp` | Cap on connections held open at once, up to the build's `VFS_SOCKET_MAX_CONNS` (default 4). Accepting past it returns `ENFILE`. |

- **`tcp` + `listen`** — the `/net/<name>` fd is the listener: it binds and listens when opened, `sock_accept` returns a connection fd of its own, and reading the listener itself fails with `ENOTCONN`. Each accepted fd reads, writes, and closes independently; closing one leaves the others serving.
- **`udp` + `listen`** — the `/net/<name>` fd is the bound socket itself: read a datagram from it, write the answer back. There is no accept step, and a write goes to the peer the last read came from.
- **`tcps`/`udps` + `listen`** — rejected. A TLS server needs a certificate and key, and the config supplies neither.
- **`serial` + `listen`** — rejected; a device has no bind step.

The role is a build option, `CONFIG_WANTED_VFS_SOCKET_LISTEN` (default off, on in the OpenWRT defconfig). A config naming `"role": "listen"` on a build without it fails the launch and says so — it never falls back to an outbound connection. See the [Platform Guide](platform-guide.md).

A `platform` mount is a **bind mount** — the Docker `-v /host/path:/wapp/path[:ro]` equivalent. Its `options` string carries two comma-separated knobs:

- `src=<hostpath>` — the absolute host directory backing the mount. Omitted, it defaults to `path`, so the host and wapp paths are identical (the original behaviour). With `src`, the wapp sees the directory under the clean internal `path` (e.g. `/cfg`) while the operator decides which host directory backs it — the same image is repointable per deployment.
- `ro` / `rw` — access mode. Omitted, it defaults to `rw`. A `ro` mount denies the wapp every write: the preopen advertises no write rights (a request beyond the grant fails with `ENOTCAPABLE`) and the backing store rejects any write that reaches it with `-EROFS`. The host directory must already exist (a read-only mount is never created).

Path resolution under the mount is confined to its host directory: an absolute symlink, a `..` escape, or a symlink inside the directory that points outside it cannot resolve through the mount — the wapp sees only what lives beneath `src`. This holds for read-only and read-write mounts alike (it closes a read escape `ro` cannot). On Linux it requires kernel ≥ 5.6; on an older kernel an open through the mount fails rather than resolving unconfined.

```jsonc
{ "name": "platform", "path": "/cfg",         "options": "src=/etc/app,ro" }  // map + read-only
{ "name": "platform", "path": "/host",        "options": "src=/home/user/wapp" }  // map, writable
{ "name": "platform", "path": "/var/lib/app" }  // src defaults to path, writable
```

A `volume` mount is an engine-managed persistent store — the Docker named-volume (`--mount type=volume`) equivalent. Unlike a bind mount, the wapp names only a **volume**, never a host path: the engine owns the backing location, creates it on first use, and binds it as a WASI preopen at `path`. The store therefore is generic for the wapp across hosts and works on targets with no operator-visible filesystem. Its `options` string carries:

- `name=<volname>` — the volume's name, a single path component (no `/`, `.`, or `..`). Omitted, it defaults to `default`. Distinct names give several independent stores.
- `ro` / `rw` — access mode. Omitted, it defaults to `rw`. A `ro` grant denies the wapp every write: the preopen advertises no write rights (a request beyond the grant fails with `ENOTCAPABLE`) and the backing store rejects any write that reaches it with `-EROFS`. The engine still provisions the backing store.
- `shared` — place the volume in the **cross-wapp shared namespace**. Omitted, the volume is **private**: namespaced under the wapp instance, so one wapp can never reach another's. A shared volume is global by name — every wapp that mounts the same `name=<volname>,shared` sees one store. Private and shared namespaces are disjoint: a private `name=X` and a shared `name=X` are different stores.

A shared volume could be used as deliberate inter-wapp channel, where each stage processes files in a store the next stage reads. The engine provides no locking; stages coordinate themselves (e.g. atomic rename, or a [named pipe](vfs-reference.md) for signalling). Which wapps share a volume is decided upstream by the supervisor handing the same `shared` volume to each — the engine enforces no policy, only the namespace.

The store persists across wapp restarts and engine reboots. It is **not** deleted when a wapp is `delete`d or uninstalled — cleanup is an explicit operator action. There is no per-volume size quota yet.

```jsonc
{ "name": "volume", "path": "/data"                              }  // the default private store, writable
{ "name": "volume", "path": "/cache", "options": "name=cache"    }  // a named private store
{ "name": "volume", "path": "/ref",   "options": "name=ref,ro"   }  // a named private store, read-only
{ "name": "volume", "path": "/stream", "options": "name=feed,shared" }  // a cross-wapp shared store
```

A relative/empty `src` or an unrecognised token is rejected at install.

## Annotated example

`configs/example_config.json` — a privileged engine booting the production sheriff supervisor with the control plane granted:

```json
{
    "system": {
        "privileged": true               // expose /proc/wapps and /proc/memory
    },
    "supervisor": {
        "imagePath": "./wasm/supervisor/sheriff/supervisor.tar",
        "params": {
            "console": {                  // supervisor stdio → engine console
                "in":  {"name": "platform"},
                "out": {"name": "platform"},
                "err": {"name": "platform"}
            },
            "drivers": [
                {"name": "wanted"}                          // control plane → /dev/wanted
            ]
        }
    }
}
```

`configs/example_config_wsh.json` is the same with `imagePath` pointing at the `wsh` debug supervisor. `configs/sheriff.json` is the production Sheriff config: it adds the `sha256`/`ed25519`/`inflate` offload devices, a `/var/lib/sheriff` platform mount for Sheriff's state, and two TLS sockets — `manager` (`tcps://localhost:8443`, the control-plane uplink) and `registry` (`tcps://localhost:5000`, OCI image pulls). `configs/sheriff-deputy.json` is the same wiring over plain TCP (`manager` at `tcp://localhost:8080`) for the local Deputy demo.

## See also

- [Control Plane Reference](control-plane-reference.md) — the launch-config schema in full (`console` / `drivers` / `mounts` / `sockets`).
- [Platform Guide](platform-guide.md) — the supervisor choice and other build configuration.
