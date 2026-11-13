#pragma once

#include <stddef.h>

/* The Quark drive client: session storage, directory listing, direct links.
 *
 * There is no public documentation for these endpoints. What is here was read
 * out of pan.quark.cn's own front-end bundle and then confirmed with requests
 * against the live service, so the paths are known good; the *field names*
 * inside the answers are the part that is inferred. Every failure therefore
 * keeps the raw response, and the /api/quark/probe endpoint exists so a wrong
 * guess shows up as the actual bytes rather than an empty list. */

#define QUARK_COOKIE_MAX 7000
#define QUARK_FID_MAX 48
#define QUARK_NAME_MAX 256
#define QUARK_URL_MAX 2048
/* Every business call carries these. The service uses them to tell a PC client
 * apart from a phone, and a request without them can be answered differently. */
#define QUARK_COMMON "pr=ucpro&fr=pc"

typedef struct {
  char cookie[QUARK_COOKIE_MAX];
  char nickname[64];
  long long total_bytes;
  long long used_bytes;
  int valid;              /* a cookie is stored */
  int verified;           /* the service accepted it at least once */
} quark_session;

typedef struct {
  char fid[QUARK_FID_MAX];
  char name[QUARK_NAME_MAX];
  int is_dir;
  long long size;
  long long updated_at;   /* unix seconds, 0 when absent */
} quark_entry;

/* Stores the data directory used for cookie.txt. Call once at start-up. */
int quark_init(const char *data_dir);

int quark_load(quark_session *s);
int quark_save(const quark_session *s);
void quark_logout(void);

/* The last listing or probe failure, for whichever surface wants to show it.
 * Kept even after the caller has moved on, because the useful moment to read it
 * is after something looked wrong. */
const char *quark_last_raw(void);
const char *quark_last_error(void);

/* Accepts whatever a user pastes: a bare "a=b; c=d" string, or a full
 * "Cookie: a=b; c=d" line copied out of the developer tools. Rejects input that
 * obviously is not a cookie instead of storing rubbish that fails later with a
 * misleading 401. */
int quark_set_cookie(const char *raw, quark_session *s, char *err, size_t cap);

/* Asks the service who we are. Fills nickname and quota when it can. */
int quark_whoami(quark_session *s, char *err, size_t cap);

/* Calls an arbitrary API path and copies the raw body out, truncated. This is
 * the debugging surface: path "/1/clouddrive/file/sort?pr=ucpro&fr=pc&pdir_fid=0"
 * and the answer is printed as-is. */
int quark_probe(const quark_session *s, const char *path, char *out, size_t cap,
                char *err, size_t errcap);

/* One page of a directory. `pdir` is "0" for the root. Returns the number of
 * entries written, or -1 with err set. */
int quark_list(const quark_session *s, const char *pdir, int page, int size,
               quark_entry *out, int cap, int *total, char *err, size_t errcap);

/* Exchanges one fid for a downloadable URL. */
int quark_direct(const quark_session *s, const char *fid, char *url, size_t cap,
                 char *err, size_t errcap);

/* Fills urls[k] for every fids[k] the service answered for, in the same order.
 * Returns how many were resolved, which may be fewer than n: a file the account
 * cannot download is skipped rather than failing the whole batch. */
int quark_direct_many(const quark_session *s, const char *const *fids, int n,
                      char urls[][QUARK_URL_MAX], char *err, size_t errcap);

/* Walks a directory tree breadth-first and calls back for every file found.
 * Folders nested deeper than depth_limit are skipped rather than silently
 * half-downloaded. */
typedef int (*quark_visit_cb)(void *ctx, const quark_entry *e, const char *pdir);

int quark_walk(const quark_session *s, const char *pdir, int depth_limit,
               quark_visit_cb cb, void *ctx, int *visited, char *err,
               size_t errcap);
