#pragma once

#include <stddef.h>

/* Sockets and TLS, and nothing else. Everything above this layer (HTTP, the
 * download queue, the Quark API) is written against these six calls, so the
 * console-specific parts stay in one file: raw sockets, getaddrinfo, and an
 * optional mbedTLS wrapper whose contexts are per-connection and therefore safe
 * to use from several download threads at once.
 *
 * The console could in principle use libSceSsl via the SDK stubs, but those
 * resolve against modules that must already be loaded in the process; a payload
 * has no way to know that before trying. Both reference payloads talk to the
 * network over plain sockets, which is the path proven to work. */

typedef struct net_conn net_conn;

/* Parses the embedded root store (+ $QDL_CA_FILE when set) once. Safe to call
 * more than once; returns 0 when TLS is usable or the build has no TLS at all. */
int net_init(char *err, size_t errcap);

/* One line describing what the TLS layer can do, for /api/diag. */
const char *net_status(void);

/* Which resolver answered last: "literal", "getaddrinfo" or "dns <server>". */
const char *net_resolver(void);

/* Numeric address for `host`. Returns 0 and fills out/family, or -1. */
int net_resolve(const char *host, char *out, size_t cap, int *family);

/* Connects, and performs the TLS handshake when use_tls is set. The host name
 * is used for SNI and certificate verification, never the resolved address. */
net_conn *net_dial(const char *host, unsigned short port, int use_tls,
                   int timeout_ms, char *err, size_t errcap);

/* >0 bytes read, 0 end of stream, -1 error (err filled in). */
long net_read(net_conn *c, void *buf, size_t len, char *err, size_t errcap);

/* 0 when every byte went out. */
int net_write_all(net_conn *c, const void *buf, size_t len, char *err,
                  size_t errcap);

void net_close(net_conn *c);

/* "TLSv1.3 / TLS_AES_256_GCM_SHA384" or "plain". */
const char *net_info(net_conn *c);
const char *net_cert_subject(net_conn *c);
const char *net_cert_issuer(net_conn *c);
/* "2027-03-01" from the leaf certificate, or "" when unknown. */
const char *net_cert_expires(net_conn *c);
