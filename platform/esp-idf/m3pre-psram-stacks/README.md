# ESP32-S3 PSRAM task-stack safety experiment

A standalone ESP-IDF app that checks whether worker task stacks placed in PSRAM
stay coherent while flash operations run concurrently on the ESP32-S3 — the
precondition for putting wapp worker stacks in PSRAM instead of scarce internal
DRAM.

## What it does

Eight worker tasks are created with `xTaskCreateStaticPinnedToCore`, each given a
16 KB stack allocated from octal PSRAM (`heap_caps_malloc(MALLOC_CAP_SPIRAM)`,
verified with `esp_ptr_external_ram`); their TCBs stay in internal RAM. Each
worker repeatedly fills and byte-verifies a large on-stack canary buffer and
recurses to touch deep stack, yielding to force context switches. Concurrently a
flash task erases, writes, and reads+verifies a rolling window of a raw 1 MB
partition — the operations that bracket a cache disable. Workers and flash task
are split across both cores.

A PSRAM stack that loses coherence across a flash cache-disable window shows up
as a canary mismatch (or a crash); flash corruption shows up as a read/verify
mismatch. The app counts both, plus per-config throughput, and prints
`VERDICT: CLEAN` or `VERDICT: CORRUPTION`. The boot log reports which mitigations
are compiled in, so a captured log is self-identifying.

The engine's production path uses `pthread` with
`esp_pthread_cfg_t.stack_alloc_caps = MALLOC_CAP_SPIRAM`; the static-task route
here only isolates the silicon behaviour.

## Two configurations

| Overlay | `SPIRAM_XIP_FROM_PSRAM` | `SPI_FLASH_AUTO_SUSPEND` |
|---------|-------------------------|--------------------------|
| `sdkconfig.defaults.baseline`  | off | off |
| `sdkconfig.defaults.mitigated` | on  | on  |

Both keep octal PSRAM, `SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY`, and a USB-Serial/JTAG
console.

## Build

Firmware builds in the ESP-IDF container image (`docker/Containerfile.esp-idf`):

```sh
PROJ="$PWD/platform/esp-idf/m3pre-psram-stacks"
podman run --rm --userns=keep-id -v "$PROJ:/proj:Z" -w /proj \
  -e SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.mitigated" \
  localhost/wanted-esp-idf \
  idf.py -B build.mitigated -DSDKCONFIG=build.mitigated/sdkconfig set-target esp32s3 build
```

Swap `mitigated` for `baseline` (and the `-B`/`-DSDKCONFIG` paths) for the other
variant.

## Flash + capture (host)

```sh
esptool -c esp32s3 -p /dev/ttyACM0 --before default-reset --after hard-reset \
  write-flash @build.mitigated/flash_args
```

The board is an ESP32-S3R8 (8 MB octal PSRAM, 8 MB quad flash) over
USB-Serial/JTAG at `/dev/ttyACM0`.
