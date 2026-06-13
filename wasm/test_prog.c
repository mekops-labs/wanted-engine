/* SPDX-License-Identifier: Apache-2.0 */

#include "wanted_libc.h"

#define WASM_EXPORT __attribute__((used)) __attribute__((visibility("default")))

int WASM_EXPORT entry(int id) {
    e_print("%s\n", "Hello world");
    wanted_func(1, 2, 3);

    return 0;
}
