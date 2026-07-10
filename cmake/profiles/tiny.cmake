# WANTED Engine resource limits — "tiny" profile (no-PSRAM, e.g. ESP32-WROOM).
#
# Usage: cmake -C cmake/profiles/tiny.cmake -S . -B build
#        make build PROFILE=tiny
#
# The floor profile, below "constrained". It targets a board without PSRAM.
# Smallest launch-config slot table. PSRAM boards should use "constrained" instead.
#
# Measured footprint (`make sizes`), LP64 / ILP32:
#   per-wapp structs  12.5 KB /  11.5 KB  (exact engine slot structures)
#   per-wapp runtime  40.0 KB /  40.0 KB  (WASM stack + heap + worker stack + ~16 KB WAMR)
#   max linear        64.0 KB             (WASM_MAX_MEMORY_PAGES=1 x 64 KiB)
#   engine overhead    4.2 KB /   4.2 KB  (wantedConfig_t)
#   worst case       237.2 KB / 235.3 KB  (overhead + MAX_WAPPS x per-wapp)
# WAMR overhead is approximate; excludes the per-image writable module copy.
set(MAX_WAPPS              2    CACHE STRING "Max concurrent wapp instances")
set(WASM_STACK_SIZE        4096 CACHE STRING "Per-instance WAMR stack (bytes)")
set(WASM_HEAP_SIZE         4096 CACHE STRING "Per-instance WAMR heap (bytes)")
set(WASM_WORKER_STACK_SIZE 16384 CACHE STRING "Per-wapp worker thread native C stack (bytes)")
set(WASM_MAX_MEMORY_PAGES  1    CACHE STRING "Max WASM linear pages/wapp (64 KiB each; 0 = unbounded)")
set(MAX_PATH_LEN           128  CACHE STRING "VFS path buffer length (bytes)")
set(MAX_DRIVERS_CNT        4    CACHE STRING "Slots per drivers/mounts/sockets launch-config section")
set(MAX_OPTIONS_SIZE       64   CACHE STRING "Per-driver options blob (bytes)")
