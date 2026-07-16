# NuttX Platform

Selected at configure time with `-DWANTED_PLATFORM=nuttx`, which also sets
`WAMR_BUILD_PLATFORM=nuttx`.


## Directory layout

```
platform/nuttx/
  CMakeLists.txt           Static library platform_nuttx (sim)
  include/
    config-nuttx.h         Registry root paths and constants
    wapp-stop.h            Cooperative wapp-stop contract
  api/
    clock-sleep.c          nanosleep-backed PlatformSleep
    random.c               /dev/urandom
    wapps.c                Thread lifecycle; wasm_runtime_terminate stop
    memory.c               PlatformMemoryStats via mallinfo()
    registry.c             Wapp index (sim: opendir/readdir over hostfs)
    crypto.c               PlatformEd25519Verify via vendored orlp/ed25519
    extram.c               External-RAM seam
    fs-volume.c            Volume store
  app/
    wanted_sim_main.c      Sim init shim (hostfs /data mount)
    wanted_esp32_main.c    Boot-ROMFS init shims (HW)
    wanted_rp2350_main.c
  vfs/
    vfs-nuttx.c            VfsPlatformFsInit driver
    vfs-gpio.c             /dev/gpio driver
    vfs-wifi.c             /dev/wifi driver
```

Shared POSIX sources (`platform/posix/`: `clock.c`, `mutex.c`, `fs.c`,
`socket.c`, `registry-store.c`, `wapps-image.c`, `sha256.c`) are compiled
into the build alongside this directory. TLS secure sockets come from the
shared raw-mbedTLS layer (`platform/posix/ssocket-mbedtls.c`), compiled when
`CONFIG_SYSTEM_WANTED_TLS` enables mbedTLS (`apps/crypto/mbedtls`) and
`SECURE_SOCKETS=1`.


## Build note (sim)

WAMR's NuttX platform (`vendor/wamr/core/shared/platform/nuttx/`) includes
`<nuttx/config.h>` and `<nuttx/cache.h>`. These headers exist only inside a
configured NuttX tree. A standalone host-gcc build of WAMR with
`WAMR_BUILD_PLATFORM=nuttx` therefore does not compile without the NuttX sim
include environment. The engine is built as a NuttX built-in application so
NuttX's build provides those include paths.
