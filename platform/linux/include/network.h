#pragma once
#include <stdint.h>

#if defined(SECURE_SOCKETS) && SECURE_SOCKETS == 1
    void *TLSInitCtx(void);
    void TLSFreeCtx(void *ctx);
    void *TLSOpenConnection(void *ctx, int socket);
    int  TLSWrite(void *ssl, const void *buf, int n);
    int  TLSRead(void *ssl, void *buf, int n);
    int  TLSAccept(void *ssl);
    int  TLSShutdown(void *ssl);
    void TLSFree(void *ssl);
#else
#define TLSInitCtx()                    (NULL)
#define TLSFreeCtx(ctx)
#define TLSOpenConnection(ctx, socket)  (NULL)
#define TLSWrite(ssl, buf, n)           (-1)
#define TLSRead(ssl, buf, n)            (-1)
#define TLSAccept(ssl)                  (-1)
#define TLSShutdown(ssl)                (-1)
#define TLSFree(ssl)
#endif