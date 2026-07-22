# OpenWRT packaging

Builds an OpenWRT `.ipk` for the WANTED engine, one per target architecture. The
engine runs as a `procd` service on the router, provisioned from a config file
on the writable overlay.

## What the package installs

| Path | Purpose |
|------|---------|
| `/usr/bin/wanted-cli` | the cross-built engine |
| `/usr/share/wanted/supervisor.tar` | packaged built-in supervisor — the configured variant (read-only default) |
| `/etc/wanted/config.json` | engine + supervisor structure (conffile; the configured JSON) |
| `/etc/config/wanted` | UCI deployment settings — endpoints and identity (conffile) |
| `/etc/init.d/wanted` | `procd` service |

The service runs the engine from `/srv/wanted` (persistent overlay), so its
registry (`./registry`) and data (`./data`) live there.

## Deployment settings: UCI

`/etc/wanted/config.json` carries the structure — drivers, console, mounts. What
differs per deployment comes from UCI and is merged into `/var/run/wanted/config.json`
at every start, so a stale render cannot outlive a config change or a reboot.

```sh
uci set wanted.main.manager='tcps://marshal.example:8443'
uci set wanted.main.registry='tcps://registry.example:5000'   # optional
uci add_list wanted.main.marshal_key='1:<64 hex chars>'       # one per Marshal key
uci set wanted.main.device_id='node-01'                       # optional; hostname otherwise
uci commit wanted && /etc/init.d/wanted restart
```

`manager` and at least one `marshal_key` are empty by default and the service
**refuses to start** without them, logging which are unset — a node that cannot
reach or verify its control plane is not worth running. Each `marshal_key` list
entry is `<id>:<64 hex>`; the id is the rotation key id the control plane signs
with. `device_id` and the keys reach Sheriff through the launch config's
`envs[]`, and are written to `identity/` on first boot and never overwritten:
fix a wrong value before first boot, or wipe `/srv/wanted/…/identity/`.

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
make openwrt-package SDK=aarch64   # generic 64-bit ARM (armsr/armv8)
make openwrt-package SDK=mipsel    # generic 32-bit MIPS (malta/le)

# or point it at the SDK for whichever target the deployment runs:
make openwrt-package SDK=<sdk-url-or-dir>
# inside the devcontainer/CI: just openwrt-package <sdk-url>
```

The recipe downloads + caches the SDK (under `.openwrt-sdk/`), stages OpenSSL
into it, auto-detects the arch, cross-builds the engine **with TLS**, and writes
the `.ipk` to `dist/`. The SDK argument can also be a path to an already-extracted
SDK directory. Runs inside the standard build container.

**Configuration.** The build uses the build dir's `.config` — what `make
menuconfig` last wrote — and nothing else. No defconfig is applied implicitly;
load the router envelope explicitly when you want it:

```sh
make openwrt-package DEFCONFIG=openwrt SDK=aarch64  # seeds a build dir that has no .config yet
make menuconfig                                     # adjust, including the supervisor variant
make defconfig openwrt                              # reload the envelope, discarding local edits
```

The supervisor variant selected there decides which image the `.ipk` carries, so
a package configured for `sheriff` needs `make sheriff` to have built
`wasm/supervisor/sheriff/supervisor.tar` — the packaging step fails loudly if it
is missing rather than substituting another. Pass an image path as the second
argument to `openwrt-package.sh` to override the choice. The one value the
package pins itself is `CONFIG_WANTED_SUPERVISOR_IMAGE_PATH`: the `.ipk` installs
the image at a fixed path, so the binary is compiled to read it from there.

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

`opkg` places the files, registers both config files as conffiles (edits survive
upgrades), and enables the `procd` service — which then stays down until UCI
carries an endpoint and a key. Set those, `/etc/init.d/wanted restart`, and check
`logread -e wanted` if it does not come up.

## Notes

- `wsh`, the self-contained debug shell, boots standalone; it ignores the UCI
  endpoints and identity, which only the control-plane supervisor consumes.
  Either variant can be the packaged built-in or staged at the overlay path.
- The `.ipk` is a gzipped tar (not a Debian `ar` archive) — the OpenWRT format.
