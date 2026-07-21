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

One command builds a production `.ipk` for any router — pass the target's SDK
URL from [downloads.openwrt.org](https://downloads.openwrt.org/) (find your
router's target/subtarget there):

```sh
make openwrt-package-aarch64      # generic 64-bit ARM (armsr/armv8)
make openwrt-package-mipsel       # generic 32-bit MIPS (malta/le)

# or point it at the SDK for whichever target the deployment runs:
make openwrt-package SDK=<sdk-url-or-dir>
# inside the devcontainer/CI: just openwrt-package <sdk-url>
```

The recipe downloads + caches the SDK (under `.openwrt-sdk/`), stages OpenSSL
into it, auto-detects the arch, cross-builds the engine **with TLS**, and writes
the `.ipk` to `dist/`. The SDK argument can also be a path to an already-extracted
SDK directory. Runs inside the standard build container.

**Production / OpenSSL.** The engine links OpenSSL for real TLS sockets and
Ed25519 signature verification. OpenSSL is not shipped in the SDK, so the recipe
stages it once per SDK (`scripts/feeds` + `make package/openssl/compile`); the
result is cached in the SDK. Runtime `Depends` are derived from the binary's
linked libraries (`libopenssl`, plus `libatomic` on 32-bit MIPS).

Under the hood the recipe calls `openwrt-package.sh`, which drives
`cmake/toolchain-openwrt.cmake` and `make-ipk.sh` — use those directly for a
custom build (e.g. `-DSECURE_SOCKETS=OFF` for a no-TLS bring-up build).

## Installing on the router

```sh
opkg install wanted-engine_<version>_<arch>.ipk
```

`opkg` places the files, registers `/etc/wanted/config.json` as a conffile
(edits survive upgrades), and enables the `procd` service. Edit the config and
`/etc/init.d/wanted restart` to reprovision.

## Notes

- The built-in default supervisor is the self-contained `wsh` shell, which boots
  standalone. The engine is TLS-capable; to run the control-plane supervisor,
  stage its image at the overlay path and set the manager/registry `tcps://`
  endpoints in the config (it expects a reachable control-plane peer).
- The `.ipk` is a gzipped tar (not a Debian `ar` archive) — the OpenWRT format.
