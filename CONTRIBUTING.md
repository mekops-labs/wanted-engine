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

All builds run inside the standardized build container — see `README.md`
for the authoritative build/test/run instructions and `make help` for the
full target list. The short version:

```bash
make build   # engine + supervisor images
make test    # unit + smoke suite via ctest
make smoke   # VFS/control-plane smoke via wsh
```

A change is ready for review once `make test` and `make smoke` pass.

## Code style

- **C99, no compiler extensions.** No GNU/Clang built-ins, `__attribute__`,
  VLAs, or non-standard library functions. The platform boundary is strict
  and resource limits (`MAX_WAPPS`, stack/heap sizes, etc.) are fixed —
  changes to either require an audit of all array-sized structures.
- **Formatting:** run `clang-format` (config in `.clang-format`) before
  committing.
- **Tests:** new VFS drivers or core behaviour need a corresponding
  `test/test-*.c` Unity test group.
- **Docs:** if a change affects the `/dev/wanted` contract, the VFS
  namespace, or the public API, update the matching file under `docs/` in
  the same change.

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
