/* SPDX-License-Identifier: Apache-2.0 */

/* Secure sockets over raw mbedTLS: network.h's TLS layer, wrapping the
 * plain socket fd platform/posix/socket.c has already created and connected
 * (or accepted) — the same seam Linux fills with OpenSSL
 * (platform/linux/api/ssocket.c). Shared by the mbedTLS platforms (ESP-IDF,
 * NuttX): mbedtls_net_context is a plain `int fd` field the caller may set
 * directly, so the engine keeps socket ownership. Raw mbedTLS rather than a
 * platform wrapper because ESP-IDF's esp-tls cannot wrap an fd it did not
 * open (esp_tls_conn_new_sync's ESP_TLS_INIT state unconditionally opens and
 * connects its own socket, confirmed against the vendored source). ESP32-S3
 * hardware-accelerated AES/SHA/ECC/bignum are that SoC's mbedTLS Kconfig
 * defaults — no extra wiring needed to get them.
 *
 * No CA bundle is provisioned (verification is MBEDTLS_SSL_VERIFY_NONE):
 * this proves the handshake and record layer, matching what the shared
 * platform/posix/socket.c call sites need; certificate-chain verification
 * against a trust store is a follow-up before any production posture.
 *
 * Neither this file's TLSOpenConnection nor Linux's ever branches on
 * direction: both unconditionally run a client-mode handshake, including
 * from PlatformNetAccept's call site, so there is no live TLS-server accept
 * path anywhere in this engine yet; TLSAccept is a parity no-op. */

#include <errno.h>
#include <stddef.h>
#include <string.h>

#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"

#include <wanted_malloc.h>

/* One entropy/DRBG pair + one mbedtls_ssl_config, created once by
 * TLSInitCtx and reused across every connection it opens. */
typedef struct {
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_ssl_config conf;
} tls_ctx_t;

/* One TLS session, returned by TLSOpenConnection. */
typedef struct {
    mbedtls_ssl_context ssl;
    mbedtls_net_context net; /* net.fd is the caller's already-connected fd */
} tls_conn_t;

void *TLSInitCtx(void) {
    static const unsigned char pers[] = "wanted-tls";

    tls_ctx_t *ctx = (tls_ctx_t *)WantedMalloc(sizeof(*ctx));
    if (ctx == NULL)
        return NULL;

    mbedtls_entropy_init(&ctx->entropy);
    mbedtls_ctr_drbg_init(&ctx->drbg);
    mbedtls_ssl_config_init(&ctx->conf);

    if (mbedtls_ctr_drbg_seed(&ctx->drbg, mbedtls_entropy_func, &ctx->entropy,
                              pers, sizeof(pers) - 1) != 0)
        goto fail;

    if (mbedtls_ssl_config_defaults(&ctx->conf, MBEDTLS_SSL_IS_CLIENT,
                                    MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT) != 0)
        goto fail;

    mbedtls_ssl_conf_authmode(&ctx->conf, MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_rng(&ctx->conf, mbedtls_ctr_drbg_random, &ctx->drbg);

    return ctx;

fail:
    mbedtls_ssl_config_free(&ctx->conf);
    mbedtls_ctr_drbg_free(&ctx->drbg);
    mbedtls_entropy_free(&ctx->entropy);
    WantedFree(ctx);
    return NULL;
}

void TLSFreeCtx(void *ctxIn) {
    tls_ctx_t *ctx = (tls_ctx_t *)ctxIn;
    if (ctx == NULL)
        return;
    mbedtls_ssl_config_free(&ctx->conf);
    mbedtls_ctr_drbg_free(&ctx->drbg);
    mbedtls_entropy_free(&ctx->entropy);
    WantedFree(ctx);
}

void *TLSOpenConnection(void *ctxIn, int socket) {
    tls_ctx_t *ctx = (tls_ctx_t *)ctxIn;
    if (ctx == NULL)
        return NULL;

    tls_conn_t *conn = (tls_conn_t *)WantedMalloc(sizeof(*conn));
    if (conn == NULL)
        return NULL;

    mbedtls_ssl_init(&conn->ssl);
    mbedtls_net_init(&conn->net);
    conn->net.fd = socket;

    if (mbedtls_ssl_setup(&conn->ssl, &ctx->conf) != 0)
        goto fail;

    mbedtls_ssl_set_bio(&conn->ssl, &conn->net, mbedtls_net_send,
                        mbedtls_net_recv, mbedtls_net_recv_timeout);

    int ret;
    while ((ret = mbedtls_ssl_handshake(&conn->ssl)) != 0) {
        if (ret != MBEDTLS_ERR_SSL_WANT_READ &&
            ret != MBEDTLS_ERR_SSL_WANT_WRITE)
            goto fail;
    }

    return conn;

fail:
    mbedtls_ssl_free(&conn->ssl);
    WantedFree(conn);
    return NULL;
}

int TLSWrite(void *connIn, const void *buf, int n) {
    tls_conn_t *conn = (tls_conn_t *)connIn;
    if (conn == NULL)
        return -EINVAL;
    return mbedtls_ssl_write(&conn->ssl, (const unsigned char *)buf, (size_t)n);
}

int TLSRead(void *connIn, void *buf, int n) {
    tls_conn_t *conn = (tls_conn_t *)connIn;
    if (conn == NULL)
        return -EINVAL;
    return mbedtls_ssl_read(&conn->ssl, (unsigned char *)buf, (size_t)n);
}

int TLSAccept(void *connIn) {
    (void)connIn;
    return 1;
}

int TLSShutdown(void *connIn) {
    tls_conn_t *conn = (tls_conn_t *)connIn;
    if (conn == NULL)
        return -EINVAL;
    return mbedtls_ssl_close_notify(&conn->ssl);
}

/* Releases the mbedTLS session state only. The socket fd (conn->net.fd) is
 * owned by platform/posix/socket.c's netCtx, which closes it separately —
 * calling mbedtls_net_free here would close a socket this layer never
 * opened, so it is not called; only mbedtls_ssl_free tears down. */
void TLSFree(void *connIn) {
    tls_conn_t *conn = (tls_conn_t *)connIn;
    if (conn == NULL)
        return;
    mbedtls_ssl_free(&conn->ssl);
    WantedFree(conn);
}
