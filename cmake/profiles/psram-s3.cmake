# WANTED Engine resource limits — "psram-s3" profile (ESP32-S3 + octal PSRAM).
#
# Usage: consumed by the ESP-IDF component build
#        (platform/esp-idf/components/wanted_engine/CMakeLists.txt); the idf.py
#        build bypasses the top-level CMakeLists profile loop.
#
# A two-axis target the memory-tier profiles do not fit: only ~320 KB internal
# DRAM but 8 MB octal PSRAM. With worker stacks in PSRAM (esp_pthread
# stack_alloc_caps = MALLOC_CAP_SPIRAM) and WASM linear memory + WAMR stack/heap
# PSRAM-backed, the per-wapp *runtime* draws from PSRAM and does not pressure
# internal RAM. What stays in internal .bss is the per-wapp static slot struct
# and its log ring, so the instance count is bounded by that, not by stack RAM.
#
# Hence: a moderate MAX_WAPPS well above the "constrained" 3, generous
# PSRAM-backed WASM stack/heap/worker-stack, and lean MAX_PATH_LEN / driver
# slots to keep the internal-RAM slot struct small. MAX_WAPPS is provisional —
# tune against `make sizes` and the internal-DRAM budget once WiFi/TLS land.
set(MAX_WAPPS              8      CACHE STRING "Max concurrent wapp instances")
set(WASM_STACK_SIZE        65536  CACHE STRING "Per-instance WAMR stack (bytes)")
set(WASM_HEAP_SIZE         131072 CACHE STRING "Per-instance WAMR heap (bytes)")
set(WASM_WORKER_STACK_SIZE 65536  CACHE STRING "Per-wapp worker thread native C stack (bytes)")
set(WASM_MAX_MEMORY_PAGES  16     CACHE STRING "Max WASM linear pages/wapp (64 KiB each; 0 = unbounded)")
set(MAX_PATH_LEN           256    CACHE STRING "VFS path buffer length (bytes)")
set(MAX_DRIVERS_CNT        6      CACHE STRING "Slots per drivers/mounts/sockets launch-config section")
set(MAX_OPTIONS_SIZE       128    CACHE STRING "Per-driver options blob (bytes)")
