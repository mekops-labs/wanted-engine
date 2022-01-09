#include <unistd.h>
#include <pthread.h>
#include <wasm3.h>

#include "my_api.h"

pthread_mutex_t mtx;

struct {
    char buf[100];
    bool set;
    pthread_cond_t cond;
} msg;

m3ApiRawFunction(send)
{
    m3ApiGetArgMem  (char *, buf)
    m3ApiGetArg     (size_t, len)

    m3ApiCheckMem(buf, len);

    pthread_mutex_lock(&mtx);
    while (msg.set) {
        pthread_cond_wait(&msg.cond, &mtx);
    }
    memcpy(msg.buf, buf, len);

    msg.set = true;
    pthread_cond_broadcast(&msg.cond);

    pthread_mutex_unlock(&mtx);

    m3ApiSuccess();
}

m3ApiRawFunction(recv)
{
    m3ApiGetArgMem  (char *, buf)
    m3ApiGetArg     (size_t, len)

    m3ApiCheckMem(buf, len);

    pthread_mutex_lock(&mtx);
    while (!msg.set) {
        pthread_cond_wait(&msg.cond, &mtx);
    }
    memcpy(buf, msg.buf, len);

    msg.set = false;
    pthread_cond_broadcast(&msg.cond);

    pthread_mutex_unlock(&mtx);

    m3ApiSuccess();
}

m3ApiRawFunction(my_sleep)
{
    m3ApiGetArg     (uint32_t, t)

    printf("sleep\n");

    sleep(t);

    m3ApiSuccess();
}

m3ApiRawFunction(get_rand)
{
    m3ApiReturnType(uint32_t)

    uint32_t r = (uint32_t)rand();

    m3ApiReturn(r);
}

static
M3Result  SuppressLookupFailure (M3Result i_result)
{
    if (i_result == m3Err_functionLookupFailed)
        return m3Err_none;
    else
        return i_result;
}

M3Result  LinkMyApi  (IM3Module module)
{
    M3Result result = m3Err_none;

    const char* env = "wanted";

    (SuppressLookupFailure (m3_LinkRawFunction(module, env, "send", "v(*i)", &send)));
    (SuppressLookupFailure (m3_LinkRawFunction(module, env, "recv", "v(*i)", &recv)));
    (SuppressLookupFailure (m3_LinkRawFunction(module, env, "sleep", "v(i)", &my_sleep)));
    (SuppressLookupFailure (m3_LinkRawFunction(module, env, "get_rand", "i()", &get_rand)));

_catch:
    return result;
}

void MyApiInit(void) {
    srand(time(NULL));

    pthread_mutex_init(&mtx, NULL);
    pthread_cond_init(&msg.cond, NULL);
}
