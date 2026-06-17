# WANTED Engine resource limits — "small" profile (128 MB–1 GB RAM, e.g. a router).
#
# Usage: cmake -C cmake/profiles/small.cmake -S . -B build
#        make build PROFILE=small
#
# Orders of magnitude more RAM than the constrained baseline, so the slot table
# and per-instance WASM allocations opt up well past the header defaults while
# staying short of the unconstrained "big" profile.
#
# Measured footprint (`make sizes`), LP64 / ILP32:
#   per-wapp structs  30.0 KB /  29.1 KB  (exact engine slot structures)
#   per-wapp runtime 336.0 KB / 336.0 KB  (WASM stack + heap + ~16 KB WAMR)
#   max linear         1.00 MB             (WASM_MAX_MEMORY_PAGES=16 x 64 KiB)
#   engine overhead   22.2 KB /  22.2 KB  (wantedConfig_t)
#   worst case        21.74 MB / 21.73 MB  (overhead + MAX_WAPPS x per-wapp)
# WAMR overhead is approximate; excludes the per-image writable module copy.
set(MAX_WAPPS             16     CACHE STRING "Max concurrent wapp instances")
set(WASM_STACK_SIZE       65536  CACHE STRING "Per-instance WAMR stack (bytes)")
set(WASM_HEAP_SIZE        262144 CACHE STRING "Per-instance WAMR heap (bytes)")
set(WASM_MAX_MEMORY_PAGES 16     CACHE STRING "Max WASM linear pages/wapp (64 KiB each; 0 = unbounded)")
set(MAX_PATH_LEN          512    CACHE STRING "VFS path buffer length (bytes)")
set(MAX_DRIVERS_CNT       8      CACHE STRING "Slots per drivers/mounts/sockets launch-config section")
set(MAX_OPTIONS_SIZE      256    CACHE STRING "Per-driver options blob (bytes)")
