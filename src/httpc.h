#pragma once

#include <stddef.h>

/* A small HTTP/1.1 client: one request, one connection, closed afterwards.
 *
 * Written by hand rather than pulled in as libcurl because the payload only
 * needs a narrow slice of it (GET/POST, Range resume, redirects, chunked
 * decoding) and because a second cross-compiled autoconf library is a second
 * way for the console build to break. `Connection: close` keeps it simple:
 * there is no pool to get wrong, and a stalled download can never leave a
 * half-reused socket behind. */

/* Return 0 to keep receiving. Non-zero aborts the transfer. */
typedef int (*http_write_cb)(void *ctx, const void *data, size_t len);

typedef struct {
  int status;              /* last HTTP status, 0 when no response arrived */
  long long length;        /* bytes in this body, -1 when unknown */
  long long total;         /* whole resource, from Content-Range; -1 if absent */
  long long received;      /* bytes handed to the callback */
  int redirects;
  int chunked;
  long long connect_ms;
  long long total_ms;
  char ctype[64];
  char location[512];
  char final_url[1024];
  char tls_info[96];       /* "TLSv1.3 / <cipher>", or "plain" */
  char tls_subject[128];
  char tls_issuer[128];
  char tls_expires[24];
  char err[220];           /* transport-level failure, "" when none */
} http_result;

typedef struct {
  const char *url;         /* absolute, http:// or https:// */
  const char *method;      /* NULL means GET */
  const char *body;
  size_t body_len;
  const char *extra_headers; /* raw "Name: value\r\n" lines, may be NULL */
  const char *user_agent;  /* NULL means "<APP_NAME>/<APP_VERSION>" */
  long long range_from;    /* >0 sends Range: bytes=N- */
  http_write_cb write;     /* NULL discards the body */
  void *write_ctx;
  volatile int *abort;     /* polled between reads */
  int timeout_ms;          /* connect timeout, 0 = 15 s */
  int max_redirects;       /* 0 = 5 */
} http_req;

/* 0 when a full response was read (any status), -1 on transport failure with
 * out->err explaining which stage. */
int http_perform(const http_req *req, http_result *out);

/* Splits an absolute URL. Returns 0 on success. Relative URLs are rejected:
 * every caller here starts from something the user typed. */
int http_url_parse(const char *url, int *tls, char *host, size_t hostcap,
                   unsigned short *port, char *path, size_t pathcap);
