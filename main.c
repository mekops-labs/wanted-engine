#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <wasm3.h>

#include "wasm/test_prog.wasm.h"

#define FATAL(msg, ...) { printf("Fatal: " msg "\n", ##__VA_ARGS__); return 1; }

m3ApiRawFunction(sum)
{
    m3ApiReturnType  (int32_t)
    m3ApiGetArg      (int32_t, a)
    m3ApiGetArg      (int32_t, b)

    printf("sum hit!\n");

    m3ApiReturn(a + b)
}

m3ApiRawFunction(ext_memcpy)
{
    m3ApiReturnType  (int32_t)
    m3ApiGetArgMem   (void *, tgt)
    m3ApiGetArgMem   (void *, src)
    m3ApiGetArg      (size_t, len)

    m3ApiCheckMem(src, len);


    printf("memcpy hit!\n");
    memcpy(tgt, src, len);

    m3ApiReturn(0)
}

int main() {
    M3Result status;

    IM3Environment env = m3_NewEnvironment();
    IM3Runtime rt = m3_NewRuntime(env, 1024, NULL);
    IM3Module mod;

    uint8_t *wasm = (uint8_t *) test_prog_wasm;
    size_t wasm_len = test_prog_wasm_len;

    status = m3_ParseModule(env, &mod, wasm, wasm_len);
    if (status) FATAL("m3_ParseModule: %s\n", status);

    status = m3_LoadModule(rt, mod);
    if (status) FATAL("m3_LoadModule: %s\n", status);

    m3_LinkRawFunction(mod, "*", "sum", "i(ii)", &sum);
    m3_LinkRawFunction(mod, "*", "ext_memcpy", "i(**i)", &ext_memcpy);

/*
    IM3Function f, f2;
    status = m3_FindFunction (&f, rt, "test");
    if (status) FATAL("m3_FindFunction: %s", status);

    status = m3_FindFunction (&f2, rt, "test_memcpy");
    if (status) FATAL("m3_FindFunction: %s", status);

    printf("Running test\n");

    int32_t a = 50, b = 10, r;
    status = m3_CallV (f, a, b);
    if (status) FATAL("m3_CallV: %s", status);

    m3_GetResultsV(f, &r);
    printf("test(%d,%d)=%d\n", a, b, r);

    int64_t r2;
    status = m3_CallV (f2);
    if (status) FATAL("m3_CallV: %s", status);

    m3_GetResultsV(f2, &r2);
    printf("test_memcpy()=%lx\n", r2);
*/

    IM3Function f;
    status = m3_FindFunction (&f, rt, "_start");
    if (status) FATAL("m3_FindFunction: %s", status);

    status = m3_CallV (f);
    if (status) FATAL("m3_CallV: %s", status);

    return 0;
}
