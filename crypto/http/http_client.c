/*
 * Copyright 2001-2019 The OpenSSL Project Authors. All Rights Reserved.
 * Copyright Siemens AG 2018-2019
 *
 * Licensed under the Apache License 2.0 (the "License").  You may not use
 * this file except in compliance with the License.  You can obtain a copy
 * in the file LICENSE in the source distribution or at
 * https://www.openssl.org/source/license.html
 */

#include "e_os.h"
#include <stdio.h>
#include <stdlib.h>
#include "crypto/ctype.h"
#include <string.h>
#include <openssl/asn1.h>
#include <openssl/evp.h>
#include <openssl/err.h>
#include <openssl/httperr.h>
#include <openssl/buffer.h>
#include <openssl/http.h>
#include "internal/sockets.h"
#include "internal/cryptlib.h"

#include "http_local.h"

#define HTTP_PREFIX "HTTP/"
#define HTTP_VERSION_PATT "1." /* or, e.g., "1.1" */
#define HTTP_VERSION_MAX_LEN 3

/* Stateful HTTP request code, supporting blocking and non-blocking I/O */

/* Opaque HTTP request status structure */

struct ossl_http_req_ctx_st {
    int state;                  /* Current I/O state */
    unsigned char *iobuf;       /* Line buffer */
    int iobuflen;               /* Line buffer length */
    BIO *io;                    /* BIO to perform I/O with */
    BIO *mem;                   /* Memory BIO response is built into */
    unsigned long asn1_len;     /* ASN1 length of response */
    unsigned long max_resp_len; /* Maximum length of response */
    time_t max_time;            /* Maximum end time of the transfer, or 0 */
};

#define HTTP_MAX_RESP_LENGTH    (100 * 1024)
#define HTTP_MAX_LINE_LEN       4096;

/* HTTP states */

#define OHS_NOREAD          0x1000 /* If set no reading should be performed */
#define OHS_ERROR           (0 | OHS_NOREAD) /* Error condition */
#define OHS_FIRSTLINE       1 /* First line being read */
#define OHS_HEADERS         2 /* MIME headers being read */
#define OHS_ASN1_HEADER     3 /* HTTP initial header (tag+length) being read */
#define OHS_ASN1_CONTENT    4 /* HTTP content octets being read */
#define OHS_ASN1_WRITE_INIT (5 | OHS_NOREAD) /* 1st call: ready to start I/O */
#define OHS_ASN1_WRITE      (6 | OHS_NOREAD) /* Request being sent */
#define OHS_ASN1_FLUSH      (7 | OHS_NOREAD) /* Request being flushed */
#define OHS_DONE            (8 | OHS_NOREAD) /* Completed */
#define OHS_HTTP_HEADER     (9 | OHS_NOREAD) /* Headers set, w/o final \r\n */

OSSL_HTTP_REQ_CTX *OSSL_HTTP_REQ_CTX_new(BIO *io, long timeout, int maxline)
{
    OSSL_HTTP_REQ_CTX *rctx;

    if (io == NULL) {
        HTTPerr(0, ERR_R_PASSED_NULL_PARAMETER);
        return 0;
    }

    if ((rctx = OPENSSL_zalloc(sizeof(*rctx))) == NULL)
        return NULL;
    rctx->state = OHS_ERROR;
    rctx->max_resp_len = HTTP_MAX_RESP_LENGTH;
    rctx->mem = BIO_new(BIO_s_mem());
    rctx->io = io;
    rctx->iobuflen = maxline > 0 ? maxline : HTTP_MAX_LINE_LEN;
    rctx->iobuf = OPENSSL_malloc(rctx->iobuflen);
    if (rctx->iobuf == NULL || rctx->mem == NULL) {
        OSSL_HTTP_REQ_CTX_free(rctx);
        return NULL;
    }
    rctx->max_time = timeout > 0 ? time(NULL) + timeout : 0;
    return rctx;
}

/* this indirectly calls ERR_clear_error() */
void OSSL_HTTP_REQ_CTX_free(OSSL_HTTP_REQ_CTX *rctx)
{
    if (!rctx)
        return;
    BIO_free(rctx->mem);
    OPENSSL_free(rctx->iobuf);
    OPENSSL_free(rctx);
}

BIO *OSSL_HTTP_REQ_CTX_get0_mem_bio(OSSL_HTTP_REQ_CTX *rctx)
{
    if (rctx == NULL) {
        HTTPerr(0, ERR_R_PASSED_NULL_PARAMETER);
        return NULL;
    }
    return rctx->mem;
}

void OSSL_HTTP_REQ_CTX_set_max_response_length(OSSL_HTTP_REQ_CTX *rctx,
                                               unsigned long len)
{
    /* TODO: unexport this function or add arg check and result value */
    rctx->max_resp_len = len != 0 ? len : HTTP_MAX_RESP_LENGTH;
}

/*
 * Create HTTP header using given op and path (or "/" in case path is NULL).
 * Server name and port must be given if and only if a proxy is used.
 */
int OSSL_HTTP_REQ_CTX_header(OSSL_HTTP_REQ_CTX *rctx, const char *op,
                             const char *path,
                             const char *server, const char *port)
{
    if (rctx == NULL || op == NULL) {
        HTTPerr(0, ERR_R_PASSED_NULL_PARAMETER);
        return 0;
    }

    if (BIO_printf(rctx->mem, "%s ", op) <= 0)
        return 0;

    if (server != NULL) {
        /*
         * Section 5.1.2 of RFC 1945 states that the absoluteURI form is only
         * allowed when using a proxy
         */
        if (BIO_printf(rctx->mem, "http://%s", server) <= 0)
            return 0;
        if (port != NULL && BIO_printf(rctx->mem, ":%s", port) <= 0)
            return 0;
    }

    /* Make sure path includes a forward slash */
    if (path == NULL)
        path = "/";
    if (path[0] != '/' && BIO_printf(rctx->mem, "/") <= 0)
        return 0;

    if (BIO_printf(rctx->mem, "%s "HTTP_PREFIX"1.0\r\n", path) <= 0)
        return 0;
    rctx->state = OHS_HTTP_HEADER;
    return 1;
}

int OSSL_HTTP_REQ_CTX_add1_header(OSSL_HTTP_REQ_CTX *rctx,
                                  const char *name, const char *value)
{
    if (rctx == NULL || name == NULL) {
        HTTPerr(0, ERR_R_PASSED_NULL_PARAMETER);
        return 0;
    }

    if (BIO_puts(rctx->mem, name) <= 0)
        return 0;
    if (value != NULL) {
        if (BIO_write(rctx->mem, ": ", 2) != 2)
            return 0;
        if (BIO_puts(rctx->mem, value) <= 0)
            return 0;
    }
    if (BIO_write(rctx->mem, "\r\n", 2) != 2)
        return 0;
    rctx->state = OHS_HTTP_HEADER;
    return 1;
}

int OSSL_HTTP_REQ_CTX_i2d(OSSL_HTTP_REQ_CTX *rctx, const char *content_type,
                          const ASN1_ITEM *it, ASN1_VALUE *req)
{
    static const char req_hdr[] =
        "Content-Type: %s\r\n"
        "Content-Length: %d\r\n\r\n";
    int reqlen = ASN1_item_i2d(req, NULL, it);

    if (rctx == NULL || content_type == NULL || it == NULL || req == NULL) {
        HTTPerr(0, ERR_R_PASSED_NULL_PARAMETER);
        return 0;
    }

    if (reqlen <= 0
            || BIO_printf(rctx->mem, req_hdr, content_type, reqlen) <= 0)
        return 0;
    if (ASN1_item_i2d_bio(it, rctx->mem, req) <= 0)
        return 0;
    rctx->state = OHS_ASN1_WRITE_INIT;
    return 1;
}

/*
 * Create OSSL_HTTP_REQ_CTX structure using the values provided.
 * If 'req' == NULL then 'it' and 'content_type' are ignored.
 * Server name and port must be given if and only if a proxy is used.
 */
OSSL_HTTP_REQ_CTX *HTTP_sendreq_new(BIO *bio, const char *path,
                                    const char *server, const char *port,
                                    const STACK_OF(CONF_VALUE) *headers,
                                    const char *host, const char *content_type,
                                    const ASN1_ITEM *it, ASN1_VALUE *req,
                                    long timeout, int maxline)
{
    OSSL_HTTP_REQ_CTX *rctx = NULL;
    int i;
    int add_host = 1;

    if (bio == NULL || path == NULL) {
        HTTPerr(0, ERR_R_PASSED_NULL_PARAMETER);
        return 0;
    }

    rctx = OSSL_HTTP_REQ_CTX_new(bio, timeout, maxline);
    if (rctx == NULL)
        return NULL;

    if (!OSSL_HTTP_REQ_CTX_header(rctx, "POST", path, server, port))
        goto err;

    for (i = 0; i < sk_CONF_VALUE_num(headers); i++) {
        CONF_VALUE *hdr = sk_CONF_VALUE_value(headers, i);

        if (add_host && strcasecmp("host", hdr->name) == 0)
            add_host = 0;
        if (!OSSL_HTTP_REQ_CTX_add1_header(rctx, hdr->name, hdr->value))
            goto err;
    }

    if (add_host && OSSL_HTTP_REQ_CTX_add1_header(rctx, "Host", host) == 0)
        goto err;

    if (req != NULL && !OSSL_HTTP_REQ_CTX_i2d(rctx, content_type, it, req))
        goto err;

    return rctx;

 err:
    OSSL_HTTP_REQ_CTX_free(rctx);
    return NULL;
}

/*
 * Parse the HTTP response. This will look like this: "HTTP/1.0 200 OK". We
 * need to obtain the numeric code and (optional) informational message.
 */

static int parse_http_line1(char *line)
{
    int retcode;
    char *p, *q, *r;
    /* Skip to first white space (passed protocol info) */

    for (p = line; *p && !ossl_isspace(*p); p++)
        continue;
    if (*p == '\0') {
        HTTPerr(0, HTTP_R_SERVER_RESPONSE_PARSE_ERROR);
        return 0;
    }

    /* Skip past white space to start of response code */
    while (*p && ossl_isspace(*p))
        p++;

    if (*p == '\0') {
        HTTPerr(0, HTTP_R_SERVER_RESPONSE_PARSE_ERROR);
        return 0;
    }

    /* Find end of response code: first whitespace after start of code */
    for (q = p; *q && !ossl_isspace(*q); q++)
        continue;

    if (*q == '\0') {
        HTTPerr(0, HTTP_R_SERVER_RESPONSE_PARSE_ERROR);
        return 0;
    }

    /* Set end of response code and start of message */
    *q++ = 0;

    /* Attempt to parse numeric code */
    retcode = strtoul(p, &r, 10);

    if (*r)
        return 0;

    /* Skip over any leading white space in message */
    while (*q && ossl_isspace(*q))
        q++;

    if (*q) {
        /*
         * Finally zap any trailing white space in message (include CRLF)
         */

        /* We know q has a non white space character so this is OK */
        for (r = q + strlen(q) - 1; ossl_isspace(*r); r--)
            *r = 0;
    }
    if (retcode != 200) {
        HTTPerr(0, HTTP_R_SERVER_RESPONSE_ERROR);
        if (*q == '\0')
            ERR_add_error_data(2, "Code=", p);
        else
            ERR_add_error_data(4, "Code=", p, ",Reason=", q);
        return 0;
    }

    return 1;

}

/*
 * Try exchanging ASN.1 request and response via HTTP on (non-)blocking BIO
 * returns 1 on success, 0 on error, -1 on BIO_should_retry
 */
int OSSL_HTTP_REQ_CTX_nbio(OSSL_HTTP_REQ_CTX *rctx)
{
    int i, n;
    const unsigned char *p;

    if (rctx == NULL) {
        HTTPerr(0, ERR_R_PASSED_NULL_PARAMETER);
        return 0;
    }

 next_io:
    if (!(rctx->state & OHS_NOREAD)) {
        n = BIO_read(rctx->io, rctx->iobuf, rctx->iobuflen);

        if (n <= 0) {
            if (BIO_should_retry(rctx->io))
                return -1;
            return 0;
        }

        /* Write data to memory BIO */
        if (BIO_write(rctx->mem, rctx->iobuf, n) != n)
            return 0;
    }

    switch (rctx->state) {
    case OHS_HTTP_HEADER:
        /* Last operation was adding headers: need a final \r\n */
        if (BIO_write(rctx->mem, "\r\n", 2) != 2) {
            rctx->state = OHS_ERROR;
            return 0;
        }
        rctx->state = OHS_ASN1_WRITE_INIT;

        /* fall thru */
    case OHS_ASN1_WRITE_INIT:
        rctx->asn1_len = BIO_get_mem_data(rctx->mem, NULL);
        rctx->state = OHS_ASN1_WRITE;

        /* fall thru */
    case OHS_ASN1_WRITE:
        n = BIO_get_mem_data(rctx->mem, &p);

        i = BIO_write(rctx->io, p + (n - rctx->asn1_len), rctx->asn1_len);

        if (i <= 0) {
            if (BIO_should_retry(rctx->io))
                return -1;
            rctx->state = OHS_ERROR;
            return 0;
        }

        rctx->asn1_len -= i;

        if (rctx->asn1_len > 0)
            goto next_io;

        rctx->state = OHS_ASN1_FLUSH;

        (void)BIO_reset(rctx->mem);

        /* fall thru */
    case OHS_ASN1_FLUSH:

        i = BIO_flush(rctx->io);

        if (i > 0) {
            rctx->state = OHS_FIRSTLINE;
            goto next_io;
        }

        if (BIO_should_retry(rctx->io))
            return -1;

        rctx->state = OHS_ERROR;
        return 0;

    case OHS_ERROR:
        return 0;

    case OHS_FIRSTLINE:
    case OHS_HEADERS:

        /* Attempt to read a line in */

 next_line:
        /*
         * Due to &%^*$" memory BIO behaviour with BIO_gets we have to check
         * there's a complete line in there before calling BIO_gets or we'll
         * just get a partial read.
         */
        n = BIO_get_mem_data(rctx->mem, &p);
        if ((n <= 0) || !memchr(p, '\n', n)) {
            if (n >= rctx->iobuflen) {
                rctx->state = OHS_ERROR;
                return 0;
            }
            goto next_io;
        }
        n = BIO_gets(rctx->mem, (char *)rctx->iobuf, rctx->iobuflen);

        if (n <= 0) {
            if (BIO_should_retry(rctx->mem))
                goto next_io;
            rctx->state = OHS_ERROR;
            return 0;
        }

        /* Don't allow excessive lines */
        if (n == rctx->iobuflen) {
            rctx->state = OHS_ERROR;
            return 0;
        }

        /* First line */
        if (rctx->state == OHS_FIRSTLINE) {
            if (parse_http_line1((char *)rctx->iobuf)) {
                rctx->state = OHS_HEADERS;
                goto next_line;
            } else {
                rctx->state = OHS_ERROR;
                return 0;
            }
        } else {
            /* Look for blank line: end of headers */
            for (p = rctx->iobuf; *p; p++) {
                if ((*p != '\r') && (*p != '\n'))
                    break;
            }
            if (*p)
                goto next_line;

            rctx->state = OHS_ASN1_HEADER;

        }

        /* Fall thru */

    case OHS_ASN1_HEADER:
        /*
         * Now reading ASN1 header: can read at least 2 bytes which is enough
         * for ASN1 SEQUENCE header and either length field or at least the
         * length of the length field.
         */
        n = BIO_get_mem_data(rctx->mem, &p);
        if (n < 2)
            goto next_io;

        /* Check it is an ASN1 SEQUENCE */
        if (*p++ != (V_ASN1_SEQUENCE | V_ASN1_CONSTRUCTED)) {
            rctx->state = OHS_ERROR;
            return 0;
        }

        /* Check out length field */
        if (*p & 0x80) {
            /*
             * If MSB set on initial length octet we can now always read 6
             * octets: make sure we have them.
             */
            if (n < 6)
                goto next_io;
            n = *p & 0x7F;
            /* Not NDEF or excessive length */
            if (!n || (n > 4)) {
                rctx->state = OHS_ERROR;
                return 0;
            }
            p++;
            rctx->asn1_len = 0;
            for (i = 0; i < n; i++) {
                rctx->asn1_len <<= 8;
                rctx->asn1_len |= *p++;
            }

            if (rctx->asn1_len > rctx->max_resp_len) {
                rctx->state = OHS_ERROR;
                return 0;
            }

            rctx->asn1_len += n + 2;
        } else
            rctx->asn1_len = *p + 2;

        rctx->state = OHS_ASN1_CONTENT;

        /* Fall thru */

    case OHS_ASN1_CONTENT:
        n = BIO_get_mem_data(rctx->mem, NULL);
        if (n < (int)rctx->asn1_len)
            goto next_io;

        rctx->state = OHS_DONE;
        return 1;

    case OHS_DONE:
        return 1;

    }

    return 0;

}

#ifndef OPENSSL_NO_SOCK

/* set up a new connection BIO, to HTTP server or to HTTP proxy if given */
static BIO *HTTP_new_bio(const char *server, const char *server_port,
                         const char *proxy, const char *proxy_port)
{
    const char *host = proxy;
    const char *port = proxy_port;
    BIO *cbio = NULL;

    if (server == NULL) {
        HTTPerr(0, ERR_R_PASSED_NULL_PARAMETER);
        return NULL;
    }

    if (proxy == NULL) {
        host = server;
        port = server_port;
    }
    cbio = BIO_new_connect(host);
    if (cbio == NULL)
        goto end;
    if (port != NULL)
        (void)BIO_set_conn_port(cbio, port);

 end:
    return cbio;
}

/* Exchange ASN.1-encoded request and response via HTTP on (non-)blocking BIO */
ASN1_VALUE *OSSL_HTTP_REQ_CTX_sendreq_d2i(OSSL_HTTP_REQ_CTX *rctx,
                                          const ASN1_ITEM *it)
{
    int sending = 1;
    int blocking = rctx->max_time == 0;
    int rv, len;
    const unsigned char *p;
    ASN1_VALUE *resp = NULL;

    if (rctx == NULL || it == NULL) {
        HTTPerr(0, ERR_R_PASSED_NULL_PARAMETER);
        return NULL;
    }

 retry:
    rv = OSSL_HTTP_REQ_CTX_nbio(rctx);
    if (rv == -1) {
        /* BIO_should_retry was true */
        sending = 0;
        if (!blocking && BIO_wait(rctx->io, rctx->max_time - time(NULL)) <= 0)
            return NULL;
        goto retry;
    }

    if (rv != 1) { /* an error occurred */
        if (sending && !blocking)
            HTTPerr(0, HTTP_R_ERROR_SENDING);
        else
            HTTPerr(0, HTTP_R_ERROR_RECEIVING);
        return NULL;
    }

    len = BIO_get_mem_data(rctx->mem, &p);
    resp = ASN1_item_d2i(NULL, &p, len, it);
    if (resp == NULL) {
        rctx->state = OHS_ERROR;
        HTTPerr(0, HTTP_R_SERVER_RESPONSE_PARSE_ERROR);
        return NULL;
    }
    return resp;
}

/*
 * Get ASN.1-encoded response via HTTP from server at given URL.
 * Assign the received message to *presp on success, else NULL.
 */
ASN1_VALUE *OSSL_HTTP_get_asn1(const char *url,
                               const char *proxy, const char *proxy_port,
                               int timeout, const ASN1_ITEM *it)
{
    char *host = NULL;
    char *port = NULL;
    char *path = NULL;
    BIO *bio = NULL;
    OSSL_HTTP_REQ_CTX *rctx = NULL;
    int use_ssl;
    ASN1_VALUE *resp = NULL;

    if (url == NULL || it == NULL) {
        HTTPerr(0, ERR_R_PASSED_NULL_PARAMETER);
        return NULL;
    }

    if (!OSSL_HTTP_parse_url(url, &host, &port, &path, &use_ssl))
        goto err;
    if (use_ssl) {
        HTTPerr(0, HTTP_R_TLS_NOT_SUPPORTED);
        goto err;
    }
    bio = HTTP_new_bio(host, port, proxy, proxy_port);
    if (bio == NULL)
        goto err;

    rctx = OSSL_HTTP_REQ_CTX_new(bio, timeout, 1024);
    if (rctx == NULL)
        goto err;

    if (BIO_connect_retry(bio, timeout /* still same timeout */) <= 0)
        goto err;
    if (!OSSL_HTTP_REQ_CTX_header(rctx, "GET", path,
                                  proxy != NULL ? host : NULL, port))
        goto err;
    if (!OSSL_HTTP_REQ_CTX_add1_header(rctx, "Host", host))
        goto err;

    resp = OSSL_HTTP_REQ_CTX_sendreq_d2i(rctx, it);

 err:
    OPENSSL_free(host);
    OPENSSL_free(path);
    OPENSSL_free(port);
    BIO_free_all(bio);
    OSSL_HTTP_REQ_CTX_free(rctx);
    return resp;
}

/*-
 * Exchange ASN.1-encoded request and response via given BIO.
 *
 * bio_update_fn is an optional BIO connect/disconnect callback function,
 * which has the prototype
 *   typedef BIO *(*HTTP_bio_cb_t) (void *ctx, BIO *bio, unsigned long detail);
 * The callback may modify the HTTP BIO provided in the bio argument,
 * whereby it may make use of any custom defined argument 'ctx'.
 * During connection establishment, just after BIO_connect_retry(),
 * the callback function is invoked with the 'detail' argument being 1.
 * On disconnect 'detail' is 0 if no error occurred or else the last error code.
 * For instance, on connect the funct may prepend a TLS BIO to implement HTTPS;
 * after disconnect it may do some error diagnostics and/or specific cleanup.
 * The function should return NULL to indicate failure.
 * After disconnect the modified BIO will be deallocated using BIO_free_all().
 *
 * Server name and port must be given if and only if a proxy is used.
 */
ASN1_VALUE *HTTP_sendreq_bio(BIO *bio,
                             HTTP_bio_cb_t bio_update_fn, void *arg,
                             const char *server,
                             const char *port, const char *path,
                             const STACK_OF(CONF_VALUE) *headers,
                             const char *host, const char *content_type,
                             ASN1_VALUE *req, const ASN1_ITEM *req_it,
                             int timeout, int maxline, const ASN1_ITEM *rsp_it)
{
    OSSL_HTTP_REQ_CTX *rctx = NULL;
    ASN1_VALUE *rsp = NULL;
    time_t start_time = timeout > 0 ? time(NULL) : 0;
    long elapsed_time;
    unsigned long err;

    if (BIO_connect_retry(bio, timeout) <= 0)
        return NULL;

    /* callback can be used to wrap or prepend TLS session */
    if (bio_update_fn != NULL) {
        bio = (*bio_update_fn)(arg, bio, 1 /* connect */);
        if (bio == NULL)
            return NULL;
    }

    elapsed_time = timeout > 0 ? (long)time(NULL) - start_time : 0;
    rctx = HTTP_sendreq_new(bio, path, server, port, headers, host,
                            content_type, req_it, req,
                            timeout > 0 ? timeout - elapsed_time : 0,
                            maxline);
    if (rctx == NULL)
        return NULL;

    rsp = OSSL_HTTP_REQ_CTX_sendreq_d2i(rctx, rsp_it);
    err = ERR_peek_error();

    if (rsp == NULL) {
        char buf[200];

        if (ERR_GET_LIB(err) == ERR_LIB_SSL
                || ERR_GET_REASON(err) == BIO_R_CONNECT_TIMEOUT
                || ERR_GET_REASON(err) == BIO_R_CONNECT_ERROR) {
            BIO_snprintf(buf, 200, "host '%s' port %s", server, port);
            ERR_add_error_data(1, buf);
            if (err == 0) {
                BIO_snprintf(buf, 200, "server has disconnected%s",
                             arg != NULL ? " violating the protocol" :
                             ", likely because it requires the use of TLS");
                ERR_add_error_data(1, buf);
            }
        }
    }

    /* callback can be used to clean up TLS session. err does not equal 1 */
    if (bio_update_fn != NULL
            && (*bio_update_fn)(arg, bio, err) == NULL) {
        ASN1_item_free(rsp, rsp_it);
        rsp = NULL;
    }

    OSSL_HTTP_REQ_CTX_free(rctx);
    return rsp;
}

ASN1_VALUE *OSSL_HTTP_post_asn1(const char *host, const char *port,
                                HTTP_bio_cb_t bio_update_fn, void *arg,
                                const char *path,
                                const char *proxy, const char *proxy_port,
                                const STACK_OF(CONF_VALUE) *headers,
                                const char *content_type,
                                ASN1_VALUE *req, const ASN1_ITEM *req_it,
                                int timeout, int maxline,
                                const ASN1_ITEM *rsp_it)
{
    BIO *bio = HTTP_new_bio(host, port, proxy, proxy_port);
    ASN1_VALUE *res;

    if (bio == NULL)
        return NULL;
    res = HTTP_sendreq_bio(bio, bio_update_fn, arg,
                           bio_update_fn == NULL && proxy != NULL ? host : NULL,
                           port, path, headers, host, content_type,
                           req, req_it, timeout, maxline, rsp_it);
    /*
     * Use BIO_free_all() because bio_update_fn may prepend or append to bio.
     * This also frees any (e.g., SSL/TLS) BIOs linked with bio and,
     * like BIO_reset(bio), calls SSL_shutdown() to notify/alert the peer.
     */
    BIO_free_all(bio);

    return res;
}

/* BASE64 encoder used for encoding basic proxy authentication credentials */
static char *base64encode(const void *buf, size_t len)
{
    int i;
    size_t outl;
    char *out;

    /* Calculate size of encoded data */
    outl = (len / 3);
    if (len % 3 > 0)
        outl++;
    outl <<= 2;
    out = OPENSSL_malloc(outl + 1);
    if (out == NULL)
        return 0;

    i = EVP_EncodeBlock((unsigned char *)out, buf, len);
    if (!ossl_assert(i <= (int)outl)) {
        OPENSSL_free(out);
        return NULL;
    }
    if (i < 0)
        *out = '\0';
    return out;
}

/* Promote the given connection BIO via the CONNECT method, used for TLS. */
/* This is typically called by an app, so bio_err and prog are used if given */
int OSSL_HTTP_proxy_connect(BIO *bio, const char *server, const char *port,
                            const char *proxyuser, const char *proxypass,
                            long timeout, BIO *bio_err, const char *prog)
{
# undef BUF_SIZE
# define BUF_SIZE (8 * 1024)
    char *mbuf = OPENSSL_malloc(BUF_SIZE);
    char *mbufp;
    int mbuf_len = 0;
    int rv;
    int ret = 0;
    BIO *fbio = BIO_new(BIO_f_buffer());
    time_t max_time = timeout > 0 ? time(NULL) + timeout : 0;

    if (bio == NULL || server == NULL || port == NULL)
        return 0;

    if (prog == NULL)
        prog = "<unknown>";
    if (mbuf == NULL || fbio == NULL) {
        BIO_printf(bio_err /* may be NULL */, "%s: out of memory", prog);
        goto end;
    }
    BIO_push(fbio, bio);
    /* CONNECT seems only to be specified for HTTP/1.1 in RFC 2817/7231 */
    BIO_printf(fbio, "CONNECT %s:%d "HTTP_PREFIX"1.1\r\n", server, port);

    /*
     * Workaround for broken proxies which would otherwise close
     * the connection when entering tunnel mode (e.g., Squid 2.6)
     */
    BIO_printf(fbio, "Proxy-Connection: Keep-Alive\r\n");

    /* Support for basic (base64) proxy authentication */
    if (proxyuser != NULL) {
        size_t l = strlen(proxyuser);
        char *proxyauth, *proxyauthenc;

        if (proxypass != NULL)
            l += strlen(proxypass);
        proxyauth = OPENSSL_malloc(l + 2);
        BIO_snprintf(proxyauth, l + 2, "%s:%s", proxyuser,
                     (proxypass != NULL) ? proxypass : "");
        proxyauthenc = base64encode(proxyauth, strlen(proxyauth));
        if (proxyauthenc != NULL)
            BIO_printf(fbio, "Proxy-Authorization: Basic %s\r\n", proxyauthenc);
        OPENSSL_clear_free(proxyauth, strlen(proxyauth));
        OPENSSL_clear_free(proxyauthenc, strlen(proxyauthenc));
        if (proxyauthenc == NULL)
            goto end;
    }

    /* Terminate the HTTP CONNECT request */
    BIO_printf(fbio, "\r\n");

 flush_retry:
    if (!BIO_flush(fbio)) {
        /* potentially needs to be retried if BIO is non-blocking */
        if (BIO_should_retry(fbio))
            goto flush_retry;
    }

 retry:
    rv = BIO_wait(fbio, max_time - time(NULL));
    if (rv <= 0) {
        BIO_printf(bio_err, "%s: HTTP CONNECT %s\n", prog,
                   rv == 0 ? "timed out" : "failed waiting for data");
        goto end;
    }

    /*
     * The first line is the HTTP response.  According to RFC 7230,
     * it's formatted exactly like this:
     *
     * HTTP/d.d ddd Reason text\r\n
     */
    mbuf_len = BIO_gets(fbio, mbuf, BUF_SIZE);
    /* as the BIO may not block, we need to wait that the first line comes in */
    if (mbuf_len < (int)strlen(HTTP_PREFIX""HTTP_VERSION_PATT" 200"))
        goto retry;

    /* RFC 7231 4.3.6: any 2xx status code is valid */
    if (strncmp(mbuf, HTTP_PREFIX, strlen(HTTP_PREFIX) != 0)) {
        BIO_printf(bio_err, "%s: HTTP CONNECT failed, non-HTTP response\n",
                   prog);
        /* Wrong protocol, not even HTTP, so stop reading headers */
        goto end;
    }
    mbufp = mbuf + strlen(HTTP_PREFIX);
    if (strncmp(mbufp, HTTP_VERSION_PATT, strlen(HTTP_VERSION_PATT)) != 0) {
        BIO_printf(bio_err, "%s: HTTP CONNECT failed, bad HTTP version %.*s\n",
                   prog, HTTP_VERSION_MAX_LEN, mbufp);
    } else {
        mbufp += HTTP_VERSION_MAX_LEN;
        if (strncmp(mbufp, " 2", strlen(" 2")) != 0) {
            mbufp += 1;
            BIO_printf(bio_err, "%s: HTTP CONNECT failed: %.*s ",
                       prog, (int)(mbuf_len - (mbufp - mbuf)), mbufp);
        } else {
            ret = 1;
        }
    }

    /*
     * TODO: this does not necessarily catch the case when the full HTTP
     * response came in in more than a single TCP message
     * Read past all following headers
     */
    do
        mbuf_len = BIO_gets(fbio, mbuf, BUF_SIZE);
    while (mbuf_len > 2);

 end:
    if (fbio != NULL) {
        (void)BIO_flush(fbio);
        BIO_pop(fbio);
        BIO_free(fbio);
    }
    OPENSSL_free(mbuf);
    return ret;
# undef BUF_SIZE
}

#endif /* !defined(OPENSSL_NO_SOCK) */
