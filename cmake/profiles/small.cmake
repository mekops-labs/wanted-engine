# WANTED Engine resource limits — "small" profile (128 MB–1 GB RAM, e.g. a router).
#
# Usage: cmake -C cmake/profiles/small.cmake -S . -B build
#        make build PROFILE=small
#
# Orders of magnitude more RAM than the constrained baseline, so the slot table
# and per-instance WASM allocations opt up well past the header defaults while
# staying short of the unconstrained "big" profile.
set(MAX_WAPPS       16     CACHE STRING "Max concurrent wapp instances")
set(WASM_STACK_SIZE 65536  CACHE STRING "Per-instance WAMR stack (bytes)")
set(WASM_HEAP_SIZE  262144 CACHE STRING "Per-instance WAMR heap (bytes)")
set(MAX_PATH_LEN    512    CACHE STRING "VFS path buffer length (bytes)")
