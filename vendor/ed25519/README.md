# ed25519 (verify-only subset)

Vendored from https://github.com/orlp/ed25519, commit
`b1f19fab4aebe607805620d25a5e42566ce46a0e` (2022-10-03).

Only the files `ed25519_verify()` actually needs are kept: `verify.c`,
`ed25519.h`, `sha512.{c,h}`, `ge.{c,h}` + `precomp_data.h`, `sc.{c,h}`,
`fe.{c,h}`, `fixedint.h`. The upstream repo also has `sign.c`, `keypair.c`,
`seed.c`, `key_exchange.c`, `add_scalar.c` (signing, keypair generation, key
exchange) - not needed here, since `PlatformEd25519Verify` only ever verifies
a signature the engine's own key never produced (Deputy signs; the device
only checks). Dropped for a smaller footprint, not because they don't work.

See `LICENSE` (upstream zlib-style license, permissive, compatible with this
repo's Apache-2.0).

Used by `platform/nuttx/api/crypto.c` - NuttX has no Ed25519 support in its
vendored mbedTLS build (confirmed against the ESP32-S3 port's own finding,
see plans/wanted-sheriff-deputy-uart-transport.md in the mekops-kb). Linux
keeps its existing OpenSSL-backed `PlatformEd25519Verify`
(`platform/linux/api/crypto.c`) unchanged - this vendor library is NuttX-only.
