#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <unistd.h>
#include <pthread.h>

#include <wasm3.h>
#include <m3_api_libc.h>

#include "wasm/test_prog.wasm.h"

#define FATAL(msg, ...) { printf("Fatal: " msg "\n", ##__VA_ARGS__); return (void *)1; }

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

m3ApiRawFunction(my_sleep)
{
    m3ApiGetArg     (uint32_t, t)

    printf("sleep\n");

    sleep(t);

    m3ApiSuccess();
}

typedef struct {
    char * name;
    uint8_t *wasm;
    size_t wasm_len;
} data_t;

void *WA_thread( void *ptr )
{
    data_t *ctx = ptr;
    M3Result status;
    IM3Module mod;
    IM3Environment env = m3_NewEnvironment();
    IM3Runtime rt = m3_NewRuntime(env, 1024, NULL);
    IM3Function f;

    printf("entering thread: %s\n", ctx->name);

    status = m3_ParseModule(env, &mod, ctx->wasm, ctx->wasm_len);
    if (status) FATAL("m3_ParseModule[%s]: %s", ctx->name, status);

    status = m3_LoadModule(rt, mod);
    if (status) FATAL("m3_LoadModule[%s]: %s", ctx->name, status);

    m3_LinkRawFunction(mod, "*", "sum", "i(ii)", &sum);
    m3_LinkRawFunction(mod, "*", "ext_memcpy", "i(**i)", &ext_memcpy);
    m3_LinkRawFunction(mod, "*", "sleep", "v(i)", &my_sleep);

    m3_LinkLibC(mod);

    status = m3_FindFunction (&f, rt, "main");
    if (status) FATAL("m3_FindFunction[%s]: %s", ctx->name, status);

    status = m3_CallV (f, (int32_t)ctx->name[1]);
    if (status) FATAL("m3_CallV[%s]: %s", ctx->name, status);

    return (void *)status;
}

int main() {
    pthread_t thread1, thread2;

    data_t t1_data = {.name = "t1", .wasm = (uint8_t *) test_prog_wasm, .wasm_len = test_prog_wasm_len};
    data_t t2_data = {.name = "t2", .wasm = (uint8_t *) test_prog_wasm, .wasm_len = test_prog_wasm_len};

    int  iret1, iret2;

    /* Create independent threads each of which will execute function */

    iret1 = pthread_create( &thread1, NULL, WA_thread, (void*) &t1_data);
    iret2 = pthread_create( &thread2, NULL, WA_thread, (void*) &t2_data);

    /* Wait till threads are complete before main continues. Unless we  */
    /* wait we run the risk of executing an exit which will terminate   */
    /* the process and all threads before the threads have completed.   */

    pthread_join( thread1, NULL);
    pthread_join( thread2, NULL);

    printf("Thread 1 returns: %d\n",iret1);
    printf("Thread 2 returns: %d\n",iret2);

    return 0;
}
