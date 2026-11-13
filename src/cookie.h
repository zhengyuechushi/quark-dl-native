#pragma once

#include <stddef.h>

/* Getting the cookie out of a desktop browser is the hardest step of using this
 * program, and the console cannot help with it. What people actually paste:
 *
 *   1. the value alone        __pus=..; __puus=..; __kp=..
 *   2. the whole header line   Cookie: __pus=..; ...
 *   3. a whole header block   many lines, one of them starting with cookie:
 *   4. a cURL command         curl 'https://..' -H 'cookie: __pus=..'
 *
 * All four end up in the same string here. The tolerance is not politeness: a
 * paste that gets silently mangled comes back as a 401 from the service, and a
 * 401 sends people off to check whether they are logged in instead of asking
 * whether the text arrived intact.
 *
 * Returns the number of bytes written to out, 0 when nothing in the input looks
 * like a cookie. Never writes a partial result. */
int cookie_extract(const char *raw, char *out, size_t cap);

/* Folds one Set-Cookie header value into `jar`, which holds a replayable
 * "name=value; name=value" list. Attributes after the first semicolon are
 * dropped: this client has no cookie store to apply Path or Expires to, and
 * only the pair travels back on the wire. A repeated name replaces the earlier
 * value, which is what a session refresh mid-login looks like.
 *
 * Login is the only flow that needs this at all - the scan handshake delivers
 * the session in Set-Cookie and nothing else. */
void cookie_jar_put(char *jar, size_t cap, const char *line);

/* Folds one replayable jar ("a=b; c=d", the shape httpc leaves in
 * http_result.set_cookie) into another, pair by pair.
 *
 * This is not cookie_jar_put with a different name. That function reads a
 * Set-Cookie header and treats everything after the first semicolon as
 * attributes, which is right for a header and wrong for a jar: handed a jar it
 * would update the first pair and silently discard the rest - and the pair it
 * would discard is usually __puus, the only one the download CDN looks at. */
void cookie_jar_merge(char *jar, size_t cap, const char *other);
