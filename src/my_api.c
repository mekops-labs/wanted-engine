#include <unistd.h>
#include <pthread.h>
#include <wasm3.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#include "my_api.h"

pthread_mutex_t mtx;

struct {
    char buf[100];
    bool set;
    pthread_cond_t cond;
} msg;

m3ApiRawFunction(my_send)
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

m3ApiRawFunction(my_recv)
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

m3ApiRawFunction(my_socket)
{
    m3ApiReturnType(int32_t)
    m3ApiGetArg     (int, domain)
    m3ApiGetArg     (int, type)
    m3ApiGetArg     (int, proto)

    if (domain == 1) domain = 2;
    if (type == 6) type = 1;

    int sockfd = socket(domain, type, proto);

    m3ApiReturn((int32_t)sockfd);
}

m3ApiRawFunction(my_connect)
{
    m3ApiReturnType(int32_t)
    m3ApiGetArg     (int32_t, sockfd)
    m3ApiGetArgMem  (struct sockaddr *, addr)
    m3ApiGetArg     (socklen_t, addrlen)

    struct sockaddr_in *a = (struct sockaddr_in *)addr;
    if (a->sin_family == 1) a->sin_family = 2;

    printf("!!! %d %x %x\n", a->sin_family, a->sin_port, a->sin_addr);
    int ret = connect(sockfd, addr, addrlen);

    m3ApiReturn(ret);
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

    (SuppressLookupFailure (m3_LinkRawFunction(module, env, "send", "v(*i)", &my_send)));
    (SuppressLookupFailure (m3_LinkRawFunction(module, env, "recv", "v(*i)", &my_recv)));
    (SuppressLookupFailure (m3_LinkRawFunction(module, env, "sleep", "v(i)", &my_sleep)));
    (SuppressLookupFailure (m3_LinkRawFunction(module, env, "get_rand", "i()", &get_rand)));
    (SuppressLookupFailure (m3_LinkRawFunction(module, env, "socket", "i(iii)", &my_socket)));
    (SuppressLookupFailure (m3_LinkRawFunction(module, env, "connect", "i(i*i)", &my_connect)));

_catch:
    return result;
}

void MyApiInit(void) {
    srand(time(NULL));

    pthread_mutex_init(&mtx, NULL);
    pthread_cond_init(&msg.cond, NULL);
}
