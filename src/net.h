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

/* Trusts the certificates in a PEM file on top of the embedded bundle. This is
 * how a console behind an intercepting proxy gets that proxy's root without a
 * rebuild: drop the PEM in the data directory. Returns how many were added, or
 * -1 with err set. It can only add trust, never turn verification off. */
int net_add_ca(const char *path, char *err, size_t errcap);

/* Which resolver answered last: "literal", "getaddrinfo" or "dns <server>". */
const char *net_resolver(void);

/* Numeric address for `host`. Returns 0 and fills out/family, or -1. */
int net_resolve(const char *host, char *out, size_t cap, int *family);

/* What the server presented, as far as the handshake got. Filled even when the
 * handshake died on certificate verification, because that is exactly the
 * moment "which certificate did it actually send" is the question worth
 * answering — the alternative is asking a user to describe a TLS error they
 * cannot see. */
typedef struct {
  char info[128];     /* "TLSv1.2 / <cipher>", "plain", or "" when no TLS */
  char subject[128];
  char issuer[128];
  char expires[40];   /* "2027-03-01" from the leaf, "" when unknown */
} net_peer;

/* Connects, and performs the TLS handshake when use_tls is set. The host name
 * is used for SNI and certificate verification, never the resolved address.
 * `peer` may be NULL; when given it is zeroed first and describes whatever the
 * server presented, success or failure. */
net_conn *net_dial(const char *host, unsigned short port, int use_tls,
                   int timeout_ms, char *err, size_t errcap, net_peer *peer);

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
