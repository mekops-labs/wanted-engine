# wsh — WANTED debug shell

`wsh` is a small interactive shell that runs as a WANTED **supervisor** variant.
It is a WASI wapp (built with the bundled wasi-sdk clang) whose console is wired
to the engine's stdio, so it can be driven from a terminal or a pipe. It exists
to inspect a running engine by hand: walk the VFS namespaces, read/write files,
and drive the `/dev/wanted` control plane.

Build it as a supervisor image with `make -C wasm/supervisor VARIANT=wsh`, or run
it interactively with `make wsh-shell` (Linux) / `make nuttx-shell` (sim).

## Commands

Each line is split on whitespace into a command and its arguments.

### Filesystem

| Command | Args | Description |
|---|---|---|
| `ls` | `[dir]` | List a directory (defaults to the current one): name, type, size, dev, ino. |
| `cd` | `<dir>` | Change the working directory. |
| `pwd` | — | Print the working directory. |
| `cat` | `<file>` | Write a file's contents to stdout. |
| `write` | `<file> <content…>` | Write the remaining arguments (joined by spaces) to a file. |
| `cp` | `<src> <dst>` | Copy a file. |
| `rm` | `<file>` | Remove a file. |

These operate through the wapp's VFS, so they reach whatever is mounted in its
namespace — TarFS (`/`), `/dev`, `/proc`, `/net`, and any configured preopens.

### Control plane (`/dev/wanted`)

Available only when the supervisor is granted the `wanted` driver (it is, by
default). These are how the supervisor manages other wapps.

| Command | Args | Description |
|---|---|---|
| `start` | `<name>` | Create-and-launch a wapp by name (`start <name>` → `/dev/wanted/ctl`). |
| `stop` | `<name>` | Stop a running wapp (`stop` → `/dev/wanted/wapps/<name>/ctl`). |
| `status` | `[name]` | With a name, print that wapp's `state`/`version`/`id`; without one, list every wapp under `/dev/wanted/wapps` with its state. |

### Session / system control

| Command | Args | Description |
|---|---|---|
| `help` | — | List the available commands. |
| `exit` | — | Return from the shell. The engine treats this as a clean supervisor exit and **respawns** wsh, so the session restarts rather than ending the engine. EOF (Ctrl-D) behaves the same. |
| `poweroff` | — | Stop all child wapps, then ask the engine to shut down (`poweroff` → `/dev/wanted/ctl`). The engine exits without respawning the supervisor — on the host the process exits; on NuttX the board powers off. |
| `reboot` | — | Stop all child wapps, then ask the engine to restart (`reboot` → `/dev/wanted/ctl`). On the host the engine re-execs its own image; on NuttX the board resets. The current wsh instance is not respawned in place. |

`poweroff` and `reboot` are ordinary writes to the existing control node, not new
host calls — the `/dev/wanted` grant is the capability that gates them, so a wapp
without that grant cannot reach them.

## Changelog

### 0.5.0

- Added `poweroff` and `reboot` commands: drain child wapps, then request engine
  shutdown / restart via `/dev/wanted/ctl`.
- `exit` (and EOF) now returns to a respawned shell instead of ending the engine;
  use `poweroff` to stop the engine.

### 0.4.1

- `start` / `stop` / `status` control-plane builtins driving `/dev/wanted`.
- Filesystem builtins: `ls`, `cd`, `pwd`, `cat`, `write`, `cp`, `rm`, `help`,
  `exit`.
