#include <wasm3.h>

#include "wanted_wasm_api.h"

m3ApiRawFunction(my_func) {
    m3ApiReturnType(int32_t) m3ApiGetArg(int, a) m3ApiGetArg(int, b)
        m3ApiGetArg(int, c)

            m3ApiReturn((int32_t)0);
}

static M3Result SuppressLookupFailure(M3Result i_result) {
    if (i_result == m3Err_functionLookupFailed)
        return m3Err_none;
    else
        return i_result;
}

M3Result LinkWantedApi(IM3Module module) {
    M3Result result = m3Err_none;

    const char *env = "wanted";

    (SuppressLookupFailure(
        m3_LinkRawFunction(module, env, "func", "i(iii)", &my_func)));

    return result;
}
