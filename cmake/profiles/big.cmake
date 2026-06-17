# WANTED Engine resource limits — "big" profile (effectively unconstrained, e.g. Linux/cloud).
#
# Usage: cmake -C cmake/profiles/big.cmake -S . -B build
#        make build PROFILE=big
#
# No meaningful memory ceiling, so the limits are sized for headroom rather than
# footprint: many concurrent instances, generous per-instance WASM stack/heap,
# and PATH_MAX-class path buffers.
#
# Measured footprint (`make sizes`), LP64 / ILP32:
#   per-wapp structs 265.1 KB / 264.2 KB  (exact; MAX_PATH_LEN + driver slots inflate wapp_t)
#   per-wapp runtime   1.14 MB /   1.14 MB (WASM stack + heap + ~16 KB WAMR)
#   max linear        unbounded            (WASM_MAX_MEMORY_PAGES=0: no cap)
#   engine overhead  260.8 KB / 260.8 KB  (wantedConfig_t; MAX_PATH_LEN + options dominate)
#   worst case        unbounded            (linear grows to each module's own max)
# WAMR overhead is approximate; excludes the per-image writable module copy.
set(MAX_WAPPS             64      CACHE STRING "Max concurrent wapp instances")
set(WASM_STACK_SIZE       131072  CACHE STRING "Per-instance WAMR stack (bytes)")
set(WASM_HEAP_SIZE        1048576 CACHE STRING "Per-instance WAMR heap (bytes)")
set(WASM_MAX_MEMORY_PAGES 0       CACHE STRING "Max WASM linear pages/wapp (64 KiB each; 0 = unbounded)")
set(MAX_PATH_LEN          4096    CACHE STRING "VFS path buffer length (bytes)")
set(MAX_DRIVERS_CNT       16      CACHE STRING "Slots per drivers/mounts/sockets launch-config section")
set(MAX_OPTIONS_SIZE      1024    CACHE STRING "Per-driver options blob (bytes)")
