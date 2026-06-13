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
wanted-cli                       # compiled-in default config
wanted-cli configs/example_config.json   # explicit config file
```

1. **CLI path argument** — if given, the JSON file is read and used verbatim.
2. **Built-in minimal default** — with no argument, the engine uses `{"system": {}}`: no privileged `/proc`, and the supervisor falls back to compiled-in defaults.
3. **Compiled-in supervisor params** — when the config omits `supervisor.params`, the engine applies the defaults baked into `src/default_supervisor_cfg.json.h`: a plain-TCP control socket at `localhost:8888`, the `wanted` control plane, and a `/var/lib/sheriff` preopen.

`{"system": {}}` is a valid config.

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
      "sockets": [ { "name": "...", "address": "..." } ]
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
2. `WANTED_SUPERVISOR_IMAGE_PATH` CMake option (compile-time default).
3. `./wasm/supervisor/sheriff/supervisor.tar`.

### `supervisor.params` — launch config

The `params` object is the supervisor wapp's launch config, identical in shape to what a supervisor writes to a wapp `config` node:

The launch config addresses resources through three purpose-specific sections, each addressed the way the resource actually is:

| Field | Type | Notes |
|-------|------|-------|
| `console` | object | Slots `in` / `out` / `err`, each a driver spec backing the wapp's stdio. |
| `drivers` | array | Up to 10 device singletons. Each mounts at `/dev/<name>` — the name determines the mount, so no `path`. |
| `mounts` | array | Up to 10 file/backend drivers, each bound at an arbitrary absolute `path` outside `/dev` and `/net`. |
| `sockets` | array | Up to 10 named connections, each created at `/net/<name>`; the transport is the entry's `address`. |

Entry shapes per section:

| Section | Keys | Notes |
|---------|------|-------|
| `console.*` | `name`, `options` | Driver backing the stdio slot. |
| `drivers[]` | `name`, `options` | Device driver; mounted at `/dev/<name>`. A `path` is rejected. |
| `mounts[]` | `name`, `path`, `options` | `path` is required, absolute, and must not fall under `/dev` or `/net`. |
| `sockets[]` | `name`, `address` | `name` is the `/net` node label; `address` is the connection URL. A `path` is rejected. |

## Driver name registry

`name` selects one of the engine's built-in drivers:

| `name` | Section | Purpose | `options` example |
|--------|---------|---------|-------------------|
| `null` | `drivers` | Bit bucket at `/dev/null`. | — |
| `log` | console slot | Ring-buffer console; output readable at `/dev/wanted/wapps/<name>/log`. | — |
| `platform` | console slot / `mounts` | As a console slot: the engine's native stdio (fds 0/1/2). In `mounts[]`: a host directory bound as a native WASI preopen at `path`. | — |
| `socket` | `sockets` | TCP/UDP, plain or TLS. The transport is the entry's `address`. | `tcp://localhost:8888` |
| `9p` | `mounts` | 9P2000 client for an external FS plugin. | `tcp://localhost:5640` |
| `config` | `mounts` | Read-only config-file injection (e.g. mounted at `/etc/config`). | `{"config_file":"/config.json"}` |
| `wanted` | `drivers` | The control-plane namespace at `/dev/wanted` (privileged). | — |

A socket `address` is a URL `<scheme>://<host>:<port>`, where the scheme picks the transport: `tcp`/`udp` (plain) or `tcps`/`udps` (TLS/DTLS). See the [VFS Reference](vfs-reference.md).

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

`configs/example_config_wsh.json` is the same with `imagePath` pointing at the `wsh` debug supervisor; `configs/sheriff.json` adds a `manager` socket and a `/var/lib/sheriff` platform mount for Sheriff's state.

## See also

- [Control Plane Reference](control-plane-reference.md) — the launch-config schema in full (`console` / `drivers` / `mounts` / `sockets`).
- [Platform Guide](platform-guide.md) — `WANTED_SUPERVISOR_IMAGE_PATH` and other build options.
