# WANTED Engine resource limits — "big" profile (effectively unconstrained, e.g. Linux/cloud).
#
# Usage: cmake -C cmake/profiles/big.cmake -S . -B build
#        make build PROFILE=big
#
# No meaningful memory ceiling, so the limits are sized for headroom rather than
# footprint: many concurrent instances, generous per-instance WASM stack/heap,
# and PATH_MAX-class path buffers.
set(MAX_WAPPS       64      CACHE STRING "Max concurrent wapp instances")
set(WASM_STACK_SIZE 131072  CACHE STRING "Per-instance WAMR stack (bytes)")
set(WASM_HEAP_SIZE  1048576 CACHE STRING "Per-instance WAMR heap (bytes)")
set(MAX_PATH_LEN    4096    CACHE STRING "VFS path buffer length (bytes)")
