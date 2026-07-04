# ESP32-S3 flash / PSRAM coexistence reproducer

A standalone ESP-IDF app that exercises the flash-read-while-PSRAM-dirty
coherency hazard on the ESP32-S3 and reports whether reads stay coherent.

## What it does

Two threads run concurrently:

- **PSRAM churn** (pthread pinned to the APP core): allocates a 2 MB octal-PSRAM
  buffer and continuously writes then verifies a rolling pattern across it with a
  prime stride, defeating the data cache so real PSRAM traffic is sustained.
- **Flash read/verify** (PRO core): fills a raw 512 KiB `pattern` flash partition
  with a deterministic byte pattern once at startup, then loops reading it back
  via `esp_partition_read` and byte-verifies every chunk.

A flash read is bracketed by the SPI-flash driver with a global cache disable
unless `CONFIG_SPI_FLASH_AUTO_SUSPEND` keeps the cache enabled. During that
window a concurrent PSRAM access can observe corruption. The app counts flash
read errors, flash content mismatches, and PSRAM self-check mismatches, then
prints `VERDICT: CLEAN` or `VERDICT: CORRUPTION`.

The boot log reports which mitigations are compiled in, so a captured log is
self-identifying.

## Two configurations

| Overlay | `SPIRAM_XIP_FROM_PSRAM` | `SPI_FLASH_AUTO_SUSPEND` |
|---------|-------------------------|--------------------------|
| `sdkconfig.defaults.baseline`  | off | off |
| `sdkconfig.defaults.mitigated` | on  | on  |

Both keep octal PSRAM (`CONFIG_SPIRAM_MODE_OCT`) and a USB-Serial/JTAG console.

## Build

Firmware builds in the ESP-IDF container image
(`docker/Containerfile.esp-idf`); nothing is required on the host but podman:

```sh
PROJ="$PWD/platform/esp-idf/m0-coexistence"
podman run --rm --userns=keep-id -v "$PROJ:/proj:Z" -w /proj \
  -e SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.baseline" \
  localhost/wanted-esp-idf \
  idf.py -B build.baseline -DSDKCONFIG=build.baseline/sdkconfig set-target esp32s3 build
```

Swap `baseline` for `mitigated` (and the `-B`/`-DSDKCONFIG` paths) for the other
variant.

## Flash + capture (host)

Flashing and serial capture run on the host, where esptool talks to the board
directly. From the variant's build directory:

```sh
esptool -c esp32s3 -p /dev/ttyACM0 --before default-reset --after hard-reset \
  write-flash @flash_args
python3 capture.py /dev/ttyACM0 50    # reconnects across the reset re-enumeration
```

The board is an ESP32-S3R8 (8 MB octal PSRAM, 8 MB quad flash) over
USB-Serial/JTAG at `/dev/ttyACM0`.
