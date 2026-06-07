# NuttX Platform

Selected at configure time with `-DWANTED_PLATFORM=nuttx`, which also sets
`WAMR_BUILD_PLATFORM=nuttx`.


## Directory layout

```
platform/nuttx/
  CMakeLists.txt           Static library platform_nuttx (sim)
  include/
    config-nuttx.h         Registry root paths and constants
  api/
    clock.c                clock_gettime / nanosleep
    random.c               /dev/urandom
    mutex.c                pthread mutex wrapper
    wapps.c                Thread lifecycle; wasm_runtime_terminate stop
    memory.c               PlatformMemoryStats via mallinfo()
    registry.c             Wapp index (sim: opendir/readdir over hostfs)
    fs.c                   Preopen state dir, rename, mkdir
    socket.c               Plain BSD sockets
    ssocket.c              TLS via mbedTLS
    registry_flash.c       HW only: flash partition mapping
  vfs/
    vfs-nuttx.c            VfsPlatformFsInit driver
```


## Build note (sim)

WAMR's NuttX platform (`vendor/wamr/core/shared/platform/nuttx/`) includes
`<nuttx/config.h>` and `<nuttx/cache.h>`. These headers exist only inside a
configured NuttX tree. A standalone host-gcc build of WAMR with
`WAMR_BUILD_PLATFORM=nuttx` therefore does not compile without the NuttX sim
include environment. The engine is built as a NuttX built-in application so
NuttX's build provides those include paths.
