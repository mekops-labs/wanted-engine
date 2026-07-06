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
# slots to keep the internal-RAM slot struct small.
#
# Real-hardware capacity measurement (2026-07-06, XIAO ESP32-S3, this profile,
# `heap_caps_get_free_size` split by MALLOC_CAP_INTERNAL/MALLOC_CAP_SPIRAM —
# PlatformMemoryStats deliberately aggregates both pools, so it can't answer
# this; see app_main.c's logHeapCaps): confirms MAX_WAPPS=8 as-is rather than
# raising it.
#
#   | state                                          | internal free | PSRAM free |
#   |-------------------------------------------------|---------------|------------|
#   | boot, supervisor only, no WiFi                   |     ~214-222 KB |    ~7.06 MB |
#   | + WiFi (`wifi-connect`) associated + DHCP leased  |       ~168 KB |    ~6.97 MB |
#   | + 7 concurrent user wapps (supervisor+7 = MAX_WAPPS) |      ~33 KB |    ~5.57 MB |
#   | 8th wapp `start`                                  | rejected -ENOSPC (clean, no crash) | — |
#
# Findings:
# 1. **MAX_WAPPS bounds the supervisor too.** `platform/esp-idf/wapps.c`'s
#    `thread_data_t threads[MAX_WAPPS]` is shared by every instance including
#    the compiled-in supervisor (`PlatformWappStart` takes an `isSupervisor`
#    flag but no separate slot) — so this profile's real ceiling is **7
#    concurrent user wapps**, not 8. Confirmed live: the 8th `start` failed
#    with a clean `-ENOSPC` at the slot table, not a crash or memory
#    exhaustion — the architectural limit bites before the memory limit does.
# 2. **PSRAM is nowhere near the binding constraint** (>5.5 MB free at full
#    load out of 8 MB) — internal DRAM is the real ceiling, matching this
#    profile's own reasoning above.
# 3. **Internal-DRAM margin at full load (WiFi + 7 wapps) is ~33 KB (~10 % of
#    the ~320 KB total) — positive but thin**, and measurably tighter than
#    without WiFi (~69 KB headroom at the same 7-wapp count with WiFi down).
#    Each additional wapp costs ~19-22 KB of internal DRAM (structs +
#    thread/WAMR bookkeeping — close to but somewhat above the ~17.7 KB
#    static-struct estimate `make sizes` gives, since that excludes
#    thread_data_t and WAMR's own per-instance heap allocations). **Not
#    raising MAX_WAPPS past 8** on this evidence: the margin is too thin to
#    spend on more concurrent instances, especially since a live TLS
#    handshake's mbedTLS buffers (tens of KB, not yet measured concurrently
#    with a fully-loaded 8-slot WiFi configuration — deliberately not tested,
#    to avoid risking an internal-DRAM exhaustion crash on hardware) would
#    eat further into it. Re-measure if WASM_WORKER_STACK_SIZE, MAX_DRIVERS_CNT,
#    or MAX_OPTIONS_SIZE change, or before raising MAX_WAPPS.
#
# WASM_HEAP_SIZE is 0 (host-managed heap off), not a nonzero value: WAMR's
# app-heap-append path (the module has no exported malloc, so the runtime
# grows linear memory by WASM_HEAP_SIZE bytes for its own allocator) fails
# `wasm_runtime_instantiate` ("init app heap failed") on this ESP-IDF/WAMR
# combination — confirmed on the XIAO ESP32-S3 at M7 with wsh.wasm; root
# cause not isolated further (no [GC_ERROR] reaches the console, so the
# failing internal check in ems_kfc.c's gc_init_with_struct_and_pool is not
# yet identified). 0 sidesteps the path entirely and costs nothing here: this
# engine's own host functions (wasi-vfs.c, wanted_wasm_api.c) never call
# wasm_runtime_module_malloc, and every wapp in wapps/ is wasi-sdk-compiled
# with its own libc allocator operating on its own linear memory.
set(MAX_WAPPS              8      CACHE STRING "Max concurrent wapp instances")
set(WASM_STACK_SIZE        65536  CACHE STRING "Per-instance WAMR stack (bytes)")
set(WASM_HEAP_SIZE         0      CACHE STRING "Per-instance WAMR host-managed heap (bytes); 0 = off, see above")
set(WASM_WORKER_STACK_SIZE 65536  CACHE STRING "Per-wapp worker thread native C stack (bytes)")
set(WASM_MAX_MEMORY_PAGES  16     CACHE STRING "Max WASM linear pages/wapp (64 KiB each; 0 = unbounded)")
set(MAX_PATH_LEN           256    CACHE STRING "VFS path buffer length (bytes)")
set(MAX_DRIVERS_CNT        6      CACHE STRING "Slots per drivers/mounts/sockets launch-config section")
set(MAX_OPTIONS_SIZE       128    CACHE STRING "Per-driver options blob (bytes)")
