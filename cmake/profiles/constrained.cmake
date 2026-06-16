# WANTED Engine resource limits — "constrained" profile (~512 KB RAM, e.g. ESP32/NuttX).
#
# Usage: cmake -C cmake/profiles/constrained.cmake -S . -B build
#        make build PROFILE=constrained
#
# The conservative baseline. It restates the wanted-config.h header defaults so
# all three profiles read in parallel; on this target the static slot table and
# per-instance WASM stack/heap are kept deliberately tight.
set(MAX_WAPPS       3    CACHE STRING "Max concurrent wapp instances")
set(WASM_STACK_SIZE 8192 CACHE STRING "Per-instance WAMR stack (bytes)")
set(WASM_HEAP_SIZE  8192 CACHE STRING "Per-instance WAMR heap (bytes)")
set(MAX_PATH_LEN    256  CACHE STRING "VFS path buffer length (bytes)")
