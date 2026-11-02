# Interpreter-mode benchmark

Scripts that compare WAMR's classic and fast interpreters on the Linux engine
and the NuttX simulator. This is a measurement aid, not part of the product.

## Build

1. Create two Linux build directories with an optimized build type, and select
   the fast interpreter in the second one:

   ```bash
   # in the build container (make shell)
   BUILD_DIR=build-poc-fast just setconfig 'WANTED_WAMR_INTERP_FAST=y'
   # on the host
   CMAKE_EXTRA_ARGS=-DCMAKE_BUILD_TYPE=Release make build BUILD_DIR=build-poc-classic
   CMAKE_EXTRA_ARGS=-DCMAKE_BUILD_TYPE=Release make build BUILD_DIR=build-poc-fast
   ```

2. In both directories, enable the framebuffer driver, its observers and
   listening sockets, and allow ten memory pages per wapp:
   `WANTED_VFS_FB`, `WANTED_VFS_FB_OBSERVE`, `WANTED_VFS_SOCKET_LISTEN`,
   `WANTED_WASM_MAX_MEMORY_PAGES=10`, `WANTED_WASM_HEAP_SIZE=65536`.
3. Build the wapps: `make wapps`. The benchmark wapps are `wapps/wbench` and
   `wapps/wlkload`.

## NuttX simulator

1. Run `nuttx-build.sh` inside the build container with `MODE=classic` and then
   `MODE=fast`. It builds with `nuttx_poc_defconfig` or
   `nuttx_poc_fast_defconfig`, switches the simulator to full optimization
   without coverage, rebuilds every object, and copies the kernel to
   `/scratch`.
2. Run `nx-run.sh <kernel> <output>` for each result.

## Output

- `wbench` prints one line per workload:
  `wbench: <name> median_us=<n> min_us=<n> max_us=<n>`. Each value is the time
  of one call, from 7 timed blocks. A block repeats the workload until it lasts
  at least 200 ms.
- The framebuffer profiles report two lines each: `<name>.render` is the
  interpreted pixel work, and `<name>.io` is the writes and the `flush`.
- `wlkload` reads `SCREEN` (`term` or `tender`), `MODE` (`full`, `row`, `tile`
  or `idle`), `RATE_HZ`, `SECONDS` and `WARMUP_S` from its environment.
- Each `wlkload` frame carries the flush time in row 0: 64 bits of
  `CLOCK_REALTIME` microseconds, in four `rgb565` pixels or sixteen `gray4`
  pixels.

## Run

- `run.sh <wanted-cli> <output>` runs the `wbench` supervisor once and records
  memory.
- `lk.sh` runs one Lookout scenario. `matrix.sh` runs all of them.
- `agg.py` and `aggl.py` print the medians.

The scripts expect a work directory that holds `app.wasm`, `wbench.tar`,
`config.json`, and a `lk/` directory with a registry containing the Lookout
package, `wlkload.tar` and `mkconfig.py`.
