# WANTED Engine resource limits — "constrained" profile (~512 KB RAM, e.g. ESP32/NuttX).
#
# Usage: cmake -C cmake/profiles/constrained.cmake -S . -B build
#        make build PROFILE=constrained
#
# The conservative baseline. It restates the wanted-config.h header defaults so
# all three profiles read in parallel; on this target the static slot table and
# per-instance WASM stack/heap are kept deliberately tight.
#
# Real-hardware capacity check (2026-07-06): this profile models a target with
# NO PSRAM offload — WASM stack/heap/worker-stack and linear memory all come
# out of the same tight internal RAM as the engine's own structs, unlike
# "psram-s3" (see that profile for the ESP32-S3+PSRAM real-hardware numbers,
# where those regions are PSRAM-backed instead). That is a different sizing
# problem than this file's MAX_WAPPS/WASM_*_SIZE knobs solve, so the S3 board's
# measured PSRAM/internal-DRAM split does not transfer numerically here. One
# finding IS structural and applies to every platform: MAX_WAPPS bounds the
# *total* concurrent-instance slot table (platform/*/wapps.c's
# thread_data_t threads[MAX_WAPPS]), and the compiled-in supervisor occupies
# one of those slots — so MAX_WAPPS=3 here means 2 concurrent user wapps
# alongside the supervisor, not 3. No board matching this profile's ~512 KB
# RAM envelope was available this session to re-verify its own WASM_*_SIZE
# numbers against real free-heap headroom the way psram-s3 was.
#
# Measured footprint (`make sizes`, re-measured 2026-07-06 post M8-M10), LP64 / ILP32:
#   per-wapp structs  17.7 KB /  16.7 KB  (exact engine slot structures)
#   per-wapp runtime  96.0 KB /  96.0 KB  (WASM stack + heap + worker stack + ~16 KB WAMR)
#   max linear        64.0 KB             (WASM_MAX_MEMORY_PAGES=1 x 64 KiB)
#   engine overhead    9.5 KB /   9.5 KB  (wantedConfig_t)
#   worst case       542.5 KB / 539.5 KB  (overhead + MAX_WAPPS x per-wapp)
# WAMR overhead is approximate; excludes the per-image writable module copy.
set(MAX_WAPPS              3    CACHE STRING "Max concurrent wapp instances")
set(WASM_STACK_SIZE        8192 CACHE STRING "Per-instance WAMR stack (bytes)")
set(WASM_HEAP_SIZE         8192 CACHE STRING "Per-instance WAMR heap (bytes)")
set(WASM_WORKER_STACK_SIZE 65536 CACHE STRING "Per-wapp worker thread native C stack (bytes)")
set(WASM_MAX_MEMORY_PAGES  1    CACHE STRING "Max WASM linear pages/wapp (64 KiB each; 0 = unbounded)")
set(MAX_PATH_LEN           256  CACHE STRING "VFS path buffer length (bytes)")
set(MAX_DRIVERS_CNT        6    CACHE STRING "Slots per drivers/mounts/sockets launch-config section")
set(MAX_OPTIONS_SIZE       128  CACHE STRING "Per-driver options blob (bytes)")
