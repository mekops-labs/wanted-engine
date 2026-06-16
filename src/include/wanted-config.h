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

/* Per-instance WAMR execution stack, in bytes. Allocated once per running wapp
 * instance, so the worst-case cost is MAX_WAPPS * WASM_STACK_SIZE. */
#ifndef WASM_STACK_SIZE
#define WASM_STACK_SIZE 8192
#endif

/* Per-instance WAMR application heap, in bytes. Allocated once per running wapp
 * instance, so the worst-case cost is MAX_WAPPS * WASM_HEAP_SIZE. */
#ifndef WASM_HEAP_SIZE
#define WASM_HEAP_SIZE 8192
#endif

/* VFS path buffer length, in bytes. Sizes every fixed path buffer in a launch
 * config (wapp_driver_t.path) and the supervisor image path; cost is linear in
 * the number of such buffers. Shared across the engine and platform layers. */
#ifndef MAX_PATH_LEN
#define MAX_PATH_LEN 256
#endif

/* Per-wapp log ring slots. Derived from MAX_WAPPS — one log ring per wapp slot;
 * the log store has no capacity of its own. Kept explicit here so the coupling
 * is visible rather than buried in the log store. */
#define LOG_SLOTS MAX_WAPPS
