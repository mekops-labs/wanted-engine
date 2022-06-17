# Development docker image for Wanted Engine

For building and CI use.

## Usage for local dev

There is a helper script in the root directory of this project (`run-docker.sh`).
Just run it without parameters. Your `$PWD` needs to be set in the root directory of the project.

## Building the image for multiple platforms

One of the easiest way to build the image for multiple platfroms is to use `buildx` with `binfmt-qemu-static` and `qemu-user-static` installed.

```sh
docker buildx build --platform linux/amd64,linux/arm64,linux/arm/v7 -t registry.gitlab.com/wanted-project/wanted-engine/build:latest --push .
```

## Changelog

### 0.2.0

- add `gcovr` for generating code coverage report in CI

### 0.1.1

- multiplatform image (amd64, aarch64, armv7)

### 0.1.0

- first release
- WASI Sdk v16
- clang 14
