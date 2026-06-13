/* SPDX-License-Identifier: Apache-2.0 */

#include <stddef.h>

#include <wasm_export.h>

#include "wanted_wasm_api.h"

static int32_t wanted_func(wasm_exec_env_t exec_env,
                           int32_t a, int32_t b, int32_t c) {
    (void)exec_env; (void)a; (void)b; (void)c;
    return 0;
}

static NativeSymbol wanted_natives[] = {
    { "func", wanted_func, "(iii)i", NULL },
};

void RegisterWantedNatives(void) {
    wasm_runtime_register_natives(
        "wanted", wanted_natives,
        sizeof(wanted_natives) / sizeof(wanted_natives[0]));
}
