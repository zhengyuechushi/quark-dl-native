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
/* The PC web client's own id. Everything in the scan flow is built around it. */
#define QUARK_CLIENT_ID "532"
/* Every business call carries these. The service uses them to tell a PC client
 * apart from a phone, and a request without them can be answered differently. */
#define QUARK_COMMON "pr=ucpro&fr=pc"

/* What a direct link has to be *fetched* with.
 *
 * A download URL is not a public file: the CDN behind it (dl-pc-*.drive.quark.cn
 * and the quarkcdn.com mirrors) repeats the anti-hotlink checks of the browser
 * download the link was minted for. A request without the client UA, without a
 * pan.quark.cn Referer, or without the session cookie is answered HTTP 412 -
 * "precondition failed" - even though the link is valid and seconds old. That
 * is the same wall aria2 and Motrix users hit when they paste a link into a
 * command line, and it is why the download engine sends all three.
 *
 * The UA is the PC client's own: the value the working exFAT build shipped, and
 * the one every public quark downloader uses. */
#define QUARK_DL_UA                                                       \
  "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "         \
  "(KHTML, like Gecko) quark-cloud-drive/3.14.2 Chrome/112.0.5615.165 "   \
  "Electron/24.1.3.8 Safari/537.36 Channel/pckk_other_ch"
#define QUARK_DL_REFERER "https://pan.quark.cn/"

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

/* Accepts whatever a user pastes: a bare "a=b; c=d" string, a full
 * "Cookie: a=b; c=d" header line, a whole request header block, or a cURL
 * command copied with "Copy as cURL". Rejects input that obviously is not a
 * cookie instead of storing rubbish that fails later with a misleading 401. */
int quark_set_cookie(const char *raw, quark_session *s, char *err, size_t cap);

/* Comma-separated names of the cookies in `cookie`, values omitted. This is
 * display data: seeing "__pus,__puus,__kp" proves the paste arrived, and an
 * empty list proves it did not. Returns the number of names found. */
int quark_cookie_keys(const char *cookie, char *out, size_t cap, int *count);

/* Scan login, in two calls because the phone is in the middle of it.
 *
 * quark_qr_new asks for a one-shot token and returns both the token to poll
 * with and the URL the QR image encodes. quark_qr_poll is then called every two
 * seconds; on QUARK_QR_OK the session has been exchanged, saved, and is ready
 * for every other call in this header. */
#define QUARK_QR_WAITING 0
#define QUARK_QR_OK 1
#define QUARK_QR_EXPIRED 2

int quark_qr_new(char *url, size_t urlcap, char *token, size_t tokcap,
                 char *err, size_t errcap);
/* Returns a QUARK_QR_* code, or -1 with err set on a transport or protocol
 * failure. An unknown status code is a failure, not a reason to keep polling. */
int quark_qr_poll(const char *token, quark_session *s, char *err, size_t errcap);

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
 * cannot download is skipped rather than failing the whole batch.
 *
 * How many fids are asked per request is not fixed: the service refuses a group
 * with HTTP 400 in a way that depends on the account, and never says the number
 * it will take. This function starts at the last size that worked and halves on
 * a refusal, down to one fid per request - the one shape every account accepts,
 * and the one the exFAT build used from the start. */
int quark_direct_many(const quark_session *s, const char *const *fids, int n,
                      char urls[][QUARK_URL_MAX], char *err, size_t errcap);

/* The cookie the links from the last quark_direct_many were minted with: the
 * stored session with whatever that call rotated into Set-Cookie folded in.
 *
 * This is not the same string as session.cookie. Quark renews __puus on the
 * download endpoint, and the CDN wants the value the link was issued with; a
 * download that presents the older one is refused. The exFAT build learned this
 * first (it passes `link.cookie` rather than the login cookie to its downloader)
 * and so does every quark downloader in the wild. NULL before the first batch. */
const char *quark_download_cookie(void);

/* Walks a directory tree breadth-first and calls back for every file found.
 * Folders nested deeper than depth_limit are skipped rather than silently
 * half-downloaded.
 *
 * The callback's `pdir` is the *relative path* of the directory that holds the
 * file, always including the walked root itself: `root_name` (the folder's own
 * display name) is the first segment, so a file directly inside the walked
 * folder comes back as "剧集/第01话.mkv", not a bare "第01话.mkv". That is what
 * lets a download recreate the tree on disk — without it, the folder you picked
 * vanishes and its files land loose in the destination. It is a path, not a fid.
 * The file's own name is in e->name; the full relative path is
 * pdir + "/" + e->name. `root_name` may be NULL when the walked root should not
 * contribute a prefix (e.g. walking the drive root). */
typedef int (*quark_visit_cb)(void *ctx, const quark_entry *e, const char *pdir);

int quark_walk(const quark_session *s, const char *pdir, const char *root_name,
               int depth_limit, quark_visit_cb cb, void *ctx, int *visited,
               char *err, size_t errcap);
