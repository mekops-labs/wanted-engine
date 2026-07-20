# kconfiglib

Build-time configuration front end: reads the engine's `Kconfig` to produce
`.config` and the generated `wanted-autoconf.h`.

| | |
|----------|---
| Upstream | <https://github.com/ulfalizer/Kconfiglib>
| Version  | 14.1.0
| Source   | `kconfiglib-14.1.0.tar.gz` from PyPI
| sha256   | `bed2cc2216f538eca4255a83a4588d8823563cdd50114f86cf1a2674e602c93c`
| License  | ISC — see `LICENSE.txt` 

Only the entry points the build uses are kept. The Tk GUI and the `all*config`
batch scripts are not carried.

## Refreshing

```bash
curl -sL <sdist-url> -o kcl.tar.gz
sha256sum kcl.tar.gz          # record it in the table above
tar xzf kcl.tar.gz
cp kconfiglib-<ver>/{kconfiglib,genconfig,menuconfig,savedefconfig,olddefconfig,defconfig}.py \
   kconfiglib-<ver>/LICENSE.txt tools/kconfiglib/
```
