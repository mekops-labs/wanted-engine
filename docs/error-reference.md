---
title: "Error Reference"
date: 2026-07-16T00:00:00+01:00
weight: 70
toc: true
description: "Every errno the engine returns at the VFS and control-plane boundary — what it means in engine context and the typical cause."
---

The engine reports failures as negated POSIX `errno` values returned from VFS
operations and control-plane writes. WASI surfaces these to a wapp as the
corresponding `__WASI_E*` code, so a wapp sees ordinary `errno` on a failed
`open`/`read`/`write`/`ioctl`. This page gives the **engine-context** meaning —
the same code can mean different things than it would on a native filesystem.

## Control plane (`/dev/wanted`)

| errno | Engine meaning | Typical cause |
|-------|----------------|---------------|
| `ENOENT` | The named node does not exist. Namespaces are **existence-gated**: any node of a wapp the engine has not `create`d returns `ENOENT` — there is no default-for-a-guessed-path. | Opening `/dev/wanted/wapps/<name>/…` before `create <name>`; a typo in a control path. |
| `EINVAL` | A malformed control write: a bad verb, a config that fails validation, or an argument where none is allowed. | A `start` with inline args; invalid launch-config JSON; a mount under a reserved namespace (`/dev`, `/net`, `/proc`). |
| `EBUSY` | The target is not in a state that accepts the verb. | `create` of a name that already exists; a lifecycle verb against a slot mid-transition. |
| `EROFS` | A write to a read-only control/state node. | Writing a `/proc/wapps/*` observability leaf, or a state node that is read-only by contract. |
| `EPERM` | The wapp lacks the capability for this action. | A non-supervisor wapp writing the root `ctl`; reaching the control plane without the `wanted` mount. |

## Filesystem & VFS (`/`, mounts, preopens)

| errno | Engine meaning | Typical cause |
|-------|----------------|---------------|
| `ENOENT` | Path does not resolve within the wapp's namespace. | A file absent from the TarFS layers; a path outside every mount; parent-traversal denied at root. |
| `EROFS` | The mount or driver is read-only. | Writing TarFS (`/`), a `platform`/`volume` mount opened with `ro`, or any driver whose write op is `NULL`. |
| `EISDIR` / `ENOTDIR` | The path is a directory used as a file, or vice-versa. | `read`/`write` on a directory fd; opening a file path with `O_DIRECTORY`. |
| `EEXIST` | The target already exists and exclusive creation was requested. | `O_CREAT|O_EXCL` on an existing file; `mkdir` of an existing directory. |
| `ENAMETOOLONG` | A path or a config buffer exceeds its compile-time bound. | A path longer than `MAX_PATH_LEN`; a volume/host path that overflows its buffer. |
| `EACCES` | The operation is not permitted for how the fd was opened. | Writing a file opened read-only. |
| `EXDEV` | A rename that crosses drivers/mounts. | `rename` between two different VFS namespaces (renames are within one driver only). |
| `ENOTSUP` | The driver does not implement this operation. | `OpenAt`/`readdir` on a driver that is a flat leaf; an op a virtual driver does not provide. |
| `ENOSYS` | The platform provides no backing for this call. | A filesystem op stubbed out on a constrained target (e.g. rename on a registry-only store). |
| `EFBIG` / `ENOSPC` | The write exceeds a size or capacity bound. | A store or ring buffer at capacity; a write past a fixed region. |
| `EIO` | A backing I/O operation failed. | A host/flash read or write error under a driver. |

## Sockets (`/net`)

| errno | Engine meaning | Typical cause |
|-------|----------------|---------------|
| `ENOTSOCK` | A socket op on a non-socket fd. | Calling `sock_*` on a file or device node. |
| `ECONNABORTED` | The connection could not be established or was torn down. | `open` on a socket node whose connect failed. |
| `EMSGSIZE` | The datagram or framed message exceeds the transport limit. | A UDP/pipe write larger than the buffer. |
| `EAGAIN` | A non-blocking operation would block. | Reading an empty pipe or socket with no data ready. |
| `EINTR` | A blocking call was interrupted. | A wapp `stop` interrupting a blocked `read`/`sleep` so the interpreter can unwind. |

## Resource limits

| errno | Engine meaning | Typical cause |
|-------|----------------|---------------|
| `EMFILE` | The wapp's fd table is full. | A wapp leaking or over-opening file descriptors (`MAX_OPENED_FILES`). |
| `ENFILE` | An engine-wide table is full. | No free wapp slot (`MAX_WAPPS`) when launching. |
| `ENOMEM` | An allocation failed. | Linear-memory growth refused at the per-wapp page cap (`WASM_MAX_MEMORY_PAGES`); host heap exhausted. |
| `EBADF` | The fd is not a valid open descriptor. | Using a closed or never-opened fd; an out-of-range fd number. |

## Generic

| errno | Engine meaning |
|-------|----------------|
| `EINVAL` | Catch-all for a malformed argument to a VFS or API call (a NULL buffer, a bad flag, an out-of-range value). |
| `ENODEV` | A requested driver is not available on this build/platform. |
| `EBADMSG` | A framed/encoded message failed to parse. |

## See also

- [VFS Reference](vfs-reference.md) — the mountpoints and drivers these codes come from.
- [Control Plane Reference](control-plane-reference.md) — the `/dev/wanted` verbs and their gating.
- [Troubleshooting](troubleshooting.md) — mapping a symptom back to one of these codes.
