#pragma once

#include <stddef.h>

/* PEM text of the trust store, compiled in from assets/ca.pem.
 *
 * Embedded rather than read from disk because a payload has no guarantee of a
 * usable /etc/ssl, and asking the user to FTP a certificate onto the console
 * would be another manual step that silently decides whether downloads work. */
const char *qdl_ca_bundle(void);
