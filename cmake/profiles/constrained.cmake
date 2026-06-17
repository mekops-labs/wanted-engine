# WANTED Engine resource limits — "constrained" profile (~512 KB RAM, e.g. ESP32/NuttX).
#
# Usage: cmake -C cmake/profiles/constrained.cmake -S . -B build
#        make build PROFILE=constrained
#
# The conservative baseline. It restates the wanted-config.h header defaults so
# all three profiles read in parallel; on this target the static slot table and
# per-instance WASM stack/heap are kept deliberately tight.
#
# Measured footprint (`make sizes`), LP64 / ILP32:
#   per-wapp structs  51.1 KB /  50.2 KB  (exact engine slot structures)
#   per-wapp runtime  32.0 KB /  32.0 KB  (WASM stack + heap + ~16 KB WAMR)
#   max linear        64.0 KB             (WASM_MAX_MEMORY_PAGES=1 x 64 KiB)
#   engine overhead   43.1 KB /  43.0 KB  (wantedConfig_t)
#   worst case       484.3 KB / 481.6 KB  (overhead + MAX_WAPPS x per-wapp)
# WAMR overhead is approximate; excludes the per-image writable module copy.
set(MAX_WAPPS             3    CACHE STRING "Max concurrent wapp instances")
set(WASM_STACK_SIZE       8192 CACHE STRING "Per-instance WAMR stack (bytes)")
set(WASM_HEAP_SIZE        8192 CACHE STRING "Per-instance WAMR heap (bytes)")
set(WASM_MAX_MEMORY_PAGES 1    CACHE STRING "Max WASM linear pages/wapp (64 KiB each; 0 = unbounded)")
set(MAX_PATH_LEN          256  CACHE STRING "VFS path buffer length (bytes)")
