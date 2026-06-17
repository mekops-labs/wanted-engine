/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

/* Engine resource limits — the single home for the engine-wide compile-time
 * tunables that dimension its static memory envelope.
 *
 * Scope: these are *engine* knobs, not driver knobs. A limit private to one VFS
 * driver (e.g. the 9P open-file table or the socket address buffer) stays a
 * local #define at the top of that driver, like every other driver's limits.
 * Only limits that size the engine core or are shared across the codebase live
 * here.
 *
 * On a static-allocation target (constrained/NuttX) these constants size
 * statically allocated tables and per-instance WAMR allocations; there is no
 * heap budget to defer them to runtime. The right value differs by roughly two
 * orders of magnitude across targets (~512 KB RAM vs. a router vs.
 * Linux/cloud), so each is overridable at build time.
 *
 * Override precedence: a build-system define wins over the header default.
 *   - CMake: -DMAX_WAPPS=... (forwarded as a compile definition by the
 *     top-level CMakeLists; per-target values ship as profiles under
 *     cmake/profiles/{constrained,small,big}.cmake).
 *   - NuttX: the same -D, forwarded from the app build (see the engine's NuttX
 *     app Makefile / `make nuttx-build PROFILE=...`).
 * Each default is wrapped in #ifndef so either path overrides it without
 * editing this file. The defaults are the conservative "constrained" envelope;
 * larger targets opt up via a profile.
 *
 * Changing any limit resizes statically allocated structures — audit every
 * array dimensioned by it before adjusting a default here.
 */

/* Maximum concurrent wapp instances. Dimensions the per-platform static slot
 * tables (thread_data_t threads[MAX_WAPPS], pending-create slots) and, through
 * LOG_SLOTS, the per-wapp log ring store. This is the dominant term in the
 * engine's static footprint: each slot reserves a wapp_t plus its log ring, and
 * the per-instance WASM stack/heap below are multiplied by the running count.
 */
#ifndef MAX_WAPPS
#define MAX_WAPPS 3
#endif

/* --- WASM per-instance memory ---------------------------------------------
 * A running wapp has three engine-controlled memory regions, passed to WAMR at
 * instantiation (wasm_runtime_instantiate_ex):
 *
 *   1. Operand stack  (WASM_STACK_SIZE)      — host memory, OUTSIDE linear mem
 *   2. App heap       (WASM_HEAP_SIZE)       — host-managed, OUTSIDE linear mem
 *   3. Linear memory  (WASM_MAX_MEMORY_PAGES)— the wapp's addressable memory
 *
 * The stack and app heap sit outside linear memory because they are runtime
 * constructs, not part of the address space the wasm module can load/store:
 * the operand stack is the interpreter's own evaluation stack, and the app heap
 * is a host-side allocator the runtime hands out via
 * wasm_runtime_module_malloc. The module can only ever read/write its linear
 * memory — that is the only region whose size it (and the WASI/VFS bridge) can
 * observe.
 */

/* Operand (execution) stack, in bytes, per running instance — the classic
 * interpreter's stack of operands and call frames. Lives in host memory,
 * outside linear memory, so the wapp cannot address it. Distinct from the
 * wapp's C "aux stack", which lives inside linear memory and is fixed by the
 * wapp's own linker (wasm-ld -z stack-size), not by this knob. Worst-case cost
 * is MAX_WAPPS * WASM_STACK_SIZE. */
#ifndef WASM_STACK_SIZE
#define WASM_STACK_SIZE 8192
#endif

/* App heap, in bytes, per running instance — a runtime-managed heap backing the
 * host-side wasm_runtime_module_malloc API, allocated outside linear memory.
 * WAMR disables it automatically when the module exports its own malloc/free,
 * so a wasi wapp (which mallocs from its libc heap at the top of linear memory)
 * typically does not use it. Worst-case cost is MAX_WAPPS * WASM_HEAP_SIZE. */
#ifndef WASM_HEAP_SIZE
#define WASM_HEAP_SIZE 8192
#endif

/* Ceiling on a wapp's linear memory, in 64 KiB pages — the memory the wapp
 * actually addresses (its data, C aux stack, and libc heap). Enforced in two
 * places: WAMR bounds memory.grow to this at runtime, and the engine refuses to
 * load any image whose *declared initial* memory exceeds it (WAMR would
 * otherwise clamp the cap up to the module's initial, letting a large initial
 * sidestep the runtime bound). 0 = no cap. The constrained default is a single
 * page. (Caveat: a module that never calls memory.grow is collapsed by WAMR's
 * shrunk-memory pass to a single fixed page, so its initial reads as one page
 * and is not caught by the load check.) */
#ifndef WASM_MAX_MEMORY_PAGES
#define WASM_MAX_MEMORY_PAGES 1
#endif

/* VFS path buffer length, in bytes. Sizes every fixed path buffer in a launch
 * config (wapp_driver_t.path) and the supervisor image path; cost is linear in
 * the number of such buffers. Shared across the engine and platform layers. */
#ifndef MAX_PATH_LEN
#define MAX_PATH_LEN 256
#endif

/* Per-section launch-config resource slots — the array length of each of a
 * launch config's drivers[], mounts[] and sockets[] sections. A wapp_config_t
 * embeds three such arrays of wapp_driver_t, and the engine keeps MAX_WAPPS
 * pending configs plus the supervisor config, so this is multiplied many times
 * into the static footprint. The constrained default covers the handful a
 * single wapp realistically declares; larger targets opt up via a profile. */
#ifndef MAX_DRIVERS_CNT
#define MAX_DRIVERS_CNT 6
#endif

/* Driver options blob, in bytes — the per-entry wapp_driver_t.options string
 * (e.g. a mount's "src=..." or a socket's transport address). Replicated across
 * every driver/mount/socket slot, so with MAX_DRIVERS_CNT it is a dominant term
 * in the static footprint. The constrained default holds a typical options
 * string with headroom; larger targets opt up via a profile. */
#ifndef MAX_OPTIONS_SIZE
#define MAX_OPTIONS_SIZE 128
#endif

/* Per-wapp log ring slots. Derived from MAX_WAPPS — one log ring per wapp slot;
 * the log store has no capacity of its own. Kept explicit here so the coupling
 * is visible rather than buried in the log store. */
#define LOG_SLOTS MAX_WAPPS
