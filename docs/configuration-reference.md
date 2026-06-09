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
      "drivers": [ { "name": "...", "path": "...", "options": "..." } ],
      "preopens": ["..."]
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

| Field | Type | Notes |
|-------|------|-------|
| `console` | object | Slots `in` / `out` / `err`, each a driver spec backing the wapp's stdio. |
| `drivers` | array | Up to 10 driver specs mounted into the namespace. |
| `preopens` | array | Up to 4 absolute paths bound as read-write WASI preopens (created if absent). |

A **driver spec** (used by each `console` slot and each `drivers[]` entry):

| Key | Type | Notes |
|-----|------|-------|
| `name` | string | A driver from the engine table (below). |
| `path` | string | Mount point inside the namespace; must resolve under `/dev/*` or `/net/*`, or name a console slot. |
| `options` | string | Driver-specific configuration. |

## Driver name registry

`name` selects one of the engine's built-in drivers:

| `name` | Mounts at | Purpose | `options` example |
|--------|-----------|---------|-------------------|
| `null` | `/dev/*` | Bit bucket. | — |
| `log` | console slot | Ring-buffer console; output readable at `/dev/wanted/wapps/<name>/log`. | — |
| `platform` | console slot / path | As a console slot: the engine's native stdio (fds 0/1/2). Mounted at a path: the host filesystem. | — |
| `socket` | `/net/*` | TCP/UDP, plain or TLS. | `t localhost 8888`, `T localhost 8889` |
| `9p` | `/dev/*` | 9P2000 client for an external FS plugin. | `tcp!localhost!5640` |
| `config` | `/dev/*` | Read-only config-file injection. | `{"config_file":"/config.json"}` |
| `virt` | `/dev/*` or `/net/*` | Namespace multiplexer over named sub-drivers. | — |
| `wanted` | `/dev/wanted` | The control-plane namespace (privileged). | — |

Socket option syntax is `t|u|T|U host port` (lowercase plain, uppercase TLS; `t`/`T` TCP, `u`/`U` UDP). See the [VFS Reference](vfs-reference.md).

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
                {"name": "wanted", "path": "/dev/wanted"}   // control plane
            ]
        }
    }
}
```

`configs/example_config_wsh.json` is the same with `imagePath` pointing at the `wsh` debug supervisor; `configs/sheriff.json` adds a `socket` driver and a `/var/lib/sheriff` preopen for Sheriff's state.

## See also

- [Control Plane Reference](control-plane-reference.md) — the launch-config schema in full (`console` / `drivers` / `preopens`).
- [Platform Guide](platform-guide.md) — `WANTED_SUPERVISOR_IMAGE_PATH` and other build options.
