# Contributing to WANTED Engine

Thanks for your interest in WANTED. This guide covers the basics for
getting a change merged.

## Getting the code

```bash
git clone --recursive https://gitlab.com/mekops/wanted/wanted-engine.git
cd wanted-engine
```

If you cloned without `--recursive`, run `git submodule update --init --recursive`.

## Building and testing

All builds run inside the standardized build container via [`just`](https://just.systems)
recipes — see `README.md` for the authoritative build/test/run instructions and
`just --list` for the full recipe list. On a bare host, `make <recipe>` runs the
same recipe in the container. The short version:

```bash
just build         # engine + supervisor images (Kconfig defaults; `just menuconfig` to change)
just test          # unit + smoke suite via ctest
just smoke-engine  # production supervisor instantiates cleanly
```

A change is ready for review once `just test` and `just smoke-engine` pass.

## Code style

- **C99, no compiler extensions.** No GNU/Clang built-ins, `__attribute__`,
  VLAs, or non-standard library functions. The platform boundary is strict
  and resource limits (`MAX_WAPPS`, stack/heap sizes, etc.) are fixed —
  changes to either require an audit of all array-sized structures.
- **Formatting:** run `clang-format` (config in `.clang-format`) before
  committing.
- **Tests:** new VFS drivers or core behaviour need a corresponding
  `test/test-*.c` Unity test group.
- **Docs:** keep `docs/` in sync **in the same change**. When a change alters a
  VFS path or mountpoint, a `/dev/wanted` verb or node, an errno returned at the
  VFS/control boundary, a config key, or the public API, update the matching
  page in the same commit — `vfs-reference.md`, `control-plane-reference.md`,
  `error-reference.md`, `configuration-reference.md`, or the doc comments on the
  public header — so a reader never sees a stale contract. The pages carry Hugo
  front matter and flow to the docs site via `make docs-sync`.

## Naming conventions

Function names encode their visibility and role:

- **`PascalCase`** — public functions exported in a header
  (`WantedStart`, `TarFsInit`, `LogStoreAppend`).
- **`Subsystem_Method`** — public functions namespaced to a subsystem or
  object (`DevFs_Register`, `NetFs_SockSend`, `ProcFs_Open`, `TarFs_Read`).
  The prefix is the owning type/subsystem; for object-style APIs the first
  argument is the instance it operates on.
- **`lowerCase`** — file-local `static` helpers (`ensureConnected`,
  `copyField`). Prefer camelCase for new helpers; some older code uses
  `snake_case` (`alloc_fd`, `pending_find`) — don't churn it gratuitously,
  but new code should be camelCase.
- **`_Method`** — file-local `static` callbacks implementing the
  `vfs_driver_t` operation table (`_Open`, `_Read`, `_Stat`, `_ReadDir`).
  One set per driver `.c`; assigned to the driver's function pointers.

Note: the `_Method` form uses a leading underscore followed by an uppercase
letter, which C reserves for the implementation (C11 §7.1.3). It is an
established, deliberate convention here — `cert-dcl37-c` is disabled in
`.clang-tidy` to accommodate it — but keep these names strictly file-local
`static`; never expose a `_`-prefixed identifier in a header.

## License headers

WANTED Engine is licensed under Apache-2.0 (see `LICENSE` and `NOTICE`).
Every new `.c`/`.h` file outside `vendor/` and `third_party/` must start
with:

```c
/* SPDX-License-Identifier: Apache-2.0 */
```

## Commit conventions

- Format: `{area}: {title}` subject line, optional bullet-point body.
  Areas: `feat`, `fix`, `refactor`, `test`, `docs`, `build`, `platform`.
- One logical change per commit — don't mix unrelated areas.
- No `Co-Authored-By` trailers.

## Submitting changes

Open a merge request against `main`. By submitting a contribution, you
agree it is licensed under Apache-2.0, per Section 5 of `LICENSE`.
