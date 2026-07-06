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
# Real-hardware capacity measurement, most recently 2026-07-06 (XIAO
# ESP32-S3, this profile, `heap_caps_get_free_size` split by
# MALLOC_CAP_INTERNAL/MALLOC_CAP_SPIRAM — PlatformMemoryStats deliberately
# aggregates both pools, so it can't answer this on its own).
#
# Superseded by the PSRAM-allocator plan
# (plans/wanted-engine-esp-idf-psram-allocator.md): `WantedMalloc` now routes
# through a real `PlatformExtram*` (platform/esp-idf/extram.c,
# heap_caps_malloc(MALLOC_CAP_SPIRAM)) instead of the dummy internal-RAM stub,
# so per-wapp bookkeeping (wapp_t, TarFS index, log-store singleton, launch
# config) moved to PSRAM; only `vfs_ctx_t`/`wasi_ctx_t` (hot syscall-dispatch
# path) stay internal. This raised MAX_WAPPS from 8 to 20 — which in turn
# required raising `WAPP_IMAGE_MAX_SLOTS` (platform/esp-idf/include/
# config-esp-idf.h) from 8 to 20 and the "wapps" flash partition
# (platform/esp-idf/project/partitions.csv) from 3M to 3200K: that constant
# also sizes registry_flash.c's concurrently-mmap'd-image table, a resource
# this plan's M0-M2 didn't originally account for (see M3's finding below).
#
#   | state (WiFi down, post-allocator-fix, MAX_WAPPS=20)      | internal free | PSRAM free |
#   |-----------------------------------------------------------|---------------|------------|
#   | boot, supervisor only                                      |     260 331 B |   6.83 MB  |
#   | + 19 concurrent user wapps (supervisor+19 = MAX_WAPPS)     |     143 039 B |   2.76 MB  |
#   | 20th wapp `start`                                          | rejected -ENOSPC (clean) | — |
#
# Findings (M0-M3, plans/wanted-engine-esp-idf-psram-allocator.md):
# 1. **MAX_WAPPS bounds the supervisor too** — unchanged finding,
#    `platform/esp-idf/wapps.c`'s `thread_data_t threads[MAX_WAPPS]` is shared
#    with the compiled-in supervisor, so N here is N-1 concurrent user wapps.
# 2. **Per-wapp internal-DRAM cost dropped from ~21.9 KB to ~6.2 KB** (a ~72 %
#    reduction), confirmed flat across the full 0-19 wapp range (117 352 B
#    total delta / 19 = 6 176 B average). The remaining cost is exactly
#    `vfs_ctx_t` + `wasi_ctx_t`, the two structs M1's audit kept internal for
#    hot-path latency; everything else (`wapp_t`, TarFS index, log-store
#    singleton, launch config) is PSRAM-resident and verified safe under
#    concurrent flash writes (M2/M3's M10-harness regression, 40/40 clean, no
#    corruption) with no host-suite regression (`make test`, 59/60 — the one
#    failure is pre-existing/environmental, confirmed via `git stash`).
# 3. **PSRAM remains nowhere near the binding constraint**: ~224 KB/wapp
#    (linear memory + stacks, unchanged by this plan) against ~6.83 MB free
#    at boot — 19 wapps uses ~4.3 MB, PSRAM free stays at 2.76 MB, well short
#    of exhaustion.
# 4. **The actual ceiling hit before internal/PSRAM capacity was
#    `WAPP_IMAGE_MAX_SLOTS`** (registry_flash.c's `g_mmapTable`, sized off
#    this constant, bounds concurrently-*mmap'd* wapp images) — a fixed
#    flash-partition/mmap-handle-table resource, orthogonal to memory
#    capacity. The first `MAX_WAPPS=20` attempt hit `-ENOMEM` at the 9th
#    concurrent wapp with 211 KB internal and 5.1 MB PSRAM both free, because
#    `WAPP_IMAGE_MAX_SLOTS` (config-esp-idf.h) was still 8. Fixed by raising
#    it to 20 alongside `MAX_WAPPS` (its header comment already documents this
#    coupling) and resizing the "wapps" partition slot size from 384 KiB to
#    160 KiB (3200 KiB / 20) to fit within the 8 MB flash — comfortably above
#    every observed test-wapp image (largest ~57.8 KB).
# 5. **MAX_WAPPS=20 verified live on hardware, WiFi down**: 19 concurrent user
#    wapps all reached `running`; the 20th cleanly rejected `-ENOSPC` (not
#    `-ENOMEM` — confirms the fix); full teardown recovered heap to within
#    noise of the pre-load baseline.
# 6. **WiFi-up is not yet re-verified live** on this build. `MAX_WAPPS=20` was
#    chosen using the WiFi-down hardware number above plus this profile's
#    previously-measured WiFi internal-RAM cost (~54 KB — ESP-IDF/lwIP-owned
#    DMA/IOB buffer overhead, independent of the engine's own allocator, so it
#    should carry over unchanged, but this is an estimate, not a
#    re-measurement): estimated WiFi-up floor ≈ 260 331 − 54 000 ≈ 206 000 B;
#    at 19 wapps that leaves ≈ 206 000 − 19×6 200 ≈ 88 000 B (~86 KB,
#    comfortably above the pre-plan ~33 KB WiFi-up margin at a fraction of
#    today's wapp count). Re-check this live (and a live TLS handshake's
#    mbedTLS buffers, still unmeasured concurrently with a fully-loaded WiFi
#    configuration — a caveat carried over unresolved from the pre-plan
#    measurement) before pushing MAX_WAPPS past 20.
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
set(MAX_WAPPS              20     CACHE STRING "Max concurrent wapp instances")
set(WASM_STACK_SIZE        65536  CACHE STRING "Per-instance WAMR stack (bytes)")
set(WASM_HEAP_SIZE         0      CACHE STRING "Per-instance WAMR host-managed heap (bytes); 0 = off, see above")
set(WASM_WORKER_STACK_SIZE 65536  CACHE STRING "Per-wapp worker thread native C stack (bytes)")
set(WASM_MAX_MEMORY_PAGES  16     CACHE STRING "Max WASM linear pages/wapp (64 KiB each; 0 = unbounded)")
set(MAX_PATH_LEN           256    CACHE STRING "VFS path buffer length (bytes)")
set(MAX_DRIVERS_CNT        6      CACHE STRING "Slots per drivers/mounts/sockets launch-config section")
set(MAX_OPTIONS_SIZE       128    CACHE STRING "Per-driver options blob (bytes)")
