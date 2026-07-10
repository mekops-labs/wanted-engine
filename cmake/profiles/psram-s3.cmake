# WANTED Engine resource limits — "psram-s3" profile (like ESP32-S3 + octal PSRAM).
#
# ~320 KB internal DRAM, 8 MB octal PSRAM. WASM runtime in PSRAM.
# Internal .bss: per-wapp slot struct + log ring.
#
# Measured footprint (on ESP32S3 with 8MB PSRAM):
#
#   | state (WiFi down, post-allocator-fix, MAX_WAPPS=20)      | internal free | PSRAM free |
#   |-----------------------------------------------------------|---------------|------------|
#   | boot, supervisor only                                      |     260 331 B |   6.83 MB  |
#   | + 19 concurrent user wapps (supervisor+19 = MAX_WAPPS)     |     143 039 B |   2.76 MB  |
#   | 20th wapp `start`                                          | rejected -ENOSPC (clean) | — |
#
# WASM_HEAP_SIZE=0 — WAMR's app-heap-append path fails on ESP-IDF/WAMR - to be investigated.

set(MAX_WAPPS              20     CACHE STRING "Max concurrent wapp instances")
set(WASM_STACK_SIZE        65536  CACHE STRING "Per-instance WAMR stack (bytes)")
set(WASM_HEAP_SIZE         0      CACHE STRING "Per-instance WAMR host-managed heap (bytes); 0 = off, see above")
set(WASM_WORKER_STACK_SIZE 65536  CACHE STRING "Per-wapp worker thread native C stack (bytes)")
set(WASM_MAX_MEMORY_PAGES  16     CACHE STRING "Max WASM linear pages/wapp (64 KiB each; 0 = unbounded)")
set(MAX_PATH_LEN           256    CACHE STRING "VFS path buffer length (bytes)")
set(MAX_DRIVERS_CNT        6      CACHE STRING "Slots per drivers/mounts/sockets launch-config section")
set(MAX_OPTIONS_SIZE       128    CACHE STRING "Per-driver options blob (bytes)")
