#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>

#include <openssl/ssl.h>
#include <openssl/err.h>

void *TLSInitCtx(void)
{
    const SSL_METHOD *method;
    SSL_CTX *ctx;

    SSL_library_init();

    OpenSSL_add_all_algorithms();  /* Load cryptos, et.al. */
    SSL_load_error_strings();   /* Bring in and register error messages */
    method = TLS_client_method();  /* Create new client-method instance */
    ctx = SSL_CTX_new(method);   /* Create new context */
    if ( ctx == NULL )
    {
        ERR_print_errors_fp(stderr);
        return NULL;
    }
    return (void *)ctx;
}

void TLSFreeCtx(void *ctx)
{
    if (NULL == ctx) {
        return;
    }

    SSL_CTX_free((SSL_CTX *)ctx);       /* release context */
}

void *TLSOpenConnection(void *ctx, int socket)
{
    SSL *ssl;

    if (NULL == ctx) {
        return NULL;
    }

    ssl = SSL_new((SSL_CTX *)ctx);      /* create new SSL connection state */
    if (ssl == NULL)
        return NULL;

    SSL_set_fd(ssl, socket);    /* attach the socket descriptor */
    if (SSL_connect(ssl) < 1) {  /* perform the connection */
        ERR_print_errors_fp(stderr);
        return NULL;
    }

    return ssl;
}

int TLSWrite(void *ssl, const void *buf, int n)
{
    if (NULL == ssl) {
        return -EINVAL;
    }

    return SSL_write((SSL *)ssl, buf, n);
}

int TLSRead(void *ssl, void *buf, int n)
{
    if (NULL == ssl) {
        return -EINVAL;
    }

    return SSL_read((SSL *)ssl, buf, n);
}

int TLSAccept(void *ssl)
{
    if (NULL == ssl) {
        return -EINVAL;
    }

    return SSL_accept((SSL *)ssl);
}

int TLSShutdown(void *ssl)
{
    if (NULL == ssl) {
        return -EINVAL;
    }

    return SSL_shutdown((SSL *)ssl);
}

void TLSFree(void *ssl)
{
    SSL_free((SSL *)ssl);        /* release connection state */
}

/* left for reference */
// void ShowCerts(SSL* ssl)
// {
//     X509 *cert;
//     char *line;
//     cert = SSL_get_peer_certificate(ssl); /* get the server's certificate */
//     if ( cert != NULL )
//     {
//         printf("Server certificates:\n");
//         line = X509_NAME_oneline(X509_get_subject_name(cert), 0, 0);
//         printf("Subject: %s\n", line);
//         free(line);       /* free the malloc'ed string */
//         line = X509_NAME_oneline(X509_get_issuer_name(cert), 0, 0);
//         printf("Issuer: %s\n", line);
//         free(line);       /* free the malloc'ed string */
//         X509_free(cert);     /* free the malloc'ed certificate copy */
//     }
//     else
//         printf("Info: No client certificates configured.\n");
//}
