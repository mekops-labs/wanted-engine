# Development docker image for Wanted Engine

For building and CI use.

## Usage for local dev

There is a helper script in the root directory of this project (`run-docker.sh`).
Just run it without parameters. Your `$PWD` needs to be set in the root directory of the project.

## Building the image for multiple platforms

One of the easiest way to build the image for multiple platfroms is to use `buildx` with `binfmt-qemu-static` and `qemu-user-static` installed.

```sh
docker buildx build --platform linux/amd64,linux/arm64 -t registry.gitlab.com/wanted-project/wanted-engine/build:latest --push .
```

Platforms are limited to `amd64` and `arm64` — the bundled wasi-sdk ships host
binaries for those two Linux arches only.

## Changelog

### 0.6.0

- add `ccache` for compiler caching (engine cmake builds; auto-detected by CMake)

### 0.5.0

- add nuttx tooling

### 0.4.0

- rebase on `debian:trixie-slim` (was `ubuntu:22.04`); smaller base, current toolchain (clang/lld 19, gcc 14)
- unversioned LLVM packages track the distro default; install with `--no-install-recommends`
- select the wasi-sdk bundle per build arch (`TARGETARCH`); drop the `arm/v7` image (no wasi-sdk host build)
- switch the wapp toolchain to the full bundled wasi-sdk v24 at `/opt/wasi-sdk` (its own clang + lld), replacing the system-clang-14 + supplemental `libclang_rt`/`wasi-sysroot` approach — the bundled lld defines `__heap_end`, which wasi-libc's allocator requires, so wapps that use `malloc` now link

### 0.3.0

- update WASI SDK v16 → v24 (last version compatible with the libclang_rt supplement approach; v26+ require switching to the full bundled SDK)
- add `cmake-curses-gui` (`ccmake` interactive CMake configurator)
- add `gdb` and `lldb-14` for native and LLVM debugging
- add `valgrind` for heap profiling and memory leak detection
- add `strace` for syscall tracing
- add `clang-format-14` and `clang-tidy-14` for formatting and static analysis
- add `wabt` (WebAssembly Binary Toolkit: `wasm-objdump`, `wasm2wat`, `wat2wasm`)
- add `jq` for JSON manipulation
- group `apt-get` package list with inline comments by category

### 0.2.0

- add `gcovr` for generating code coverage report in CI

### 0.1.1

- multiplatform image (amd64, aarch64, armv7)

### 0.1.0

- first release
- WASI Sdk v16
- clang 14
