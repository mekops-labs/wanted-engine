# OpenWRT packaging

Builds an OpenWRT `.ipk` for the WANTED engine, one per target architecture. The
engine runs as a `procd` service on the router, provisioned from a config file
on the writable overlay.

## What the package installs

| Path | Purpose |
|------|---------|
| `/usr/bin/wanted-cli` | the cross-built engine |
| `/usr/share/wanted/supervisor.tar` | packaged built-in supervisor (read-only default) |
| `/etc/wanted/config.json` | engine + supervisor config (conffile; user-editable, on overlay) |
| `/etc/init.d/wanted` | `procd` service |

The service runs the engine from `/srv/wanted` (persistent overlay), so its
registry (`./registry`) and data (`./data`) live there.

## Supervisor resolution: built-in, upgradable

The config's `supervisor.imagePath` points at `/srv/wanted/supervisor.tar` — a
writable, overlay-staged image. The engine prefers it if present and otherwise
falls back to the packaged built-in at `/usr/share/wanted/supervisor.tar`, so a
fresh install always boots. Staging a new image at the overlay path is how the
supervisor is updated without replacing the package.

## Building

Prerequisite: the OpenWRT SDK for the target (musl cross toolchain). See
`cmake/toolchain-openwrt.cmake` for the env vars it expects. All commands run
inside the standard build container (which provides cmake/ninja); the SDK
toolchain is self-hosted.

Cross-build the engine, pointing the compiled-in supervisor path at the packaged
location and disabling OpenSSL (this bring-up package ships the self-contained
`wsh` supervisor and needs no TLS):

```sh
export STAGING_DIR=$SDK/staging_dir
export OPENWRT_TOOLCHAIN=$STAGING_DIR/toolchain-aarch64_cortex-a53_gcc-13.3.0_musl
export OPENWRT_CROSS=aarch64-openwrt-linux-musl
export OPENWRT_SYSROOT=$STAGING_DIR/target-aarch64_cortex-a53_musl
export OPENWRT_ARCH=aarch64
cmake -B build-ipk-aarch64 -S . -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-openwrt.cmake \
      -DBUILD_TESTING=OFF -C cmake/profiles/small.cmake -DSECURE_SOCKETS=OFF \
      -DWANTED_SUPERVISOR_IMAGE_PATH=/usr/share/wanted/supervisor.tar
cmake --build build-ipk-aarch64 -j"$(nproc)"
```

Assemble the ipk:

```sh
sh packaging/openwrt/make-ipk.sh aarch64_cortex-a53 \
   build-ipk-aarch64/cmd/wanted-cli wasm/supervisor/wsh/supervisor.tar \
   "$(git describe --tags | sed s/^v//)" dist
```

Repeat with the mipsel SDK (`OPENWRT_ARCH=mips`, cross prefix
`mipsel-openwrt-linux-musl`, opkg arch `mipsel_24kc`) for the second target.

## Installing on the router

```sh
opkg install wanted-engine_<version>_<arch>.ipk
```

`opkg` places the files, registers `/etc/wanted/config.json` as a conffile
(edits survive upgrades), and enables the `procd` service. Edit the config and
`/etc/init.d/wanted restart` to reprovision.

## Notes

- This package ships the interactive `wsh` supervisor as the built-in default —
  a self-contained bring-up target. A production control-plane supervisor that
  needs TLS and signature verification requires an OpenSSL-enabled cross-build
  (`SECURE_SOCKETS` on, OpenSSL from the SDK target feed).
- The `.ipk` is a gzipped tar (not a Debian `ar` archive) — the OpenWRT format.
