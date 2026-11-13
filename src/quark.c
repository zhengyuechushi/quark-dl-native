#include "quark.h"

#include "boot.h"

#include "cookie.h"
#include "httpc.h"
#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* Where the web client itself talks. Read from pan.quark.cn's front-end bundle
 * (bizHost / API host) and confirmed against the live service: asking
 * /1/clouddrive/file/sort without a session answers
 * {"status":401,"code":31001,"message":"require login [guest]"}, which is the
 * whole reason the paths below are trusted and the field names are not. */
#define QUARK_API "https://drive-pc.quark.cn"
#define QUARK_ORIGIN "https://pan.quark.cn"

#define QUARK_UA                                                        \
  "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "        \
  "(KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36"

#define QUARK_RAW_MAX 600

static char g_data[512];
static char g_raw[QUARK_RAW_MAX + 1];
static char g_err[300];

/* The cookie the links in hand were minted with, and the switch that makes
 * call() fold a rotation into it. Only the direct-link path turns the switch on,
 * and only for the one call that matters: every other endpoint rotates __puus
 * too, but those links are not used for a download, and keeping the stored
 * session untouched is what makes a failed listing leave no trace behind.
 *
 * It is carried forward from call to call rather than reset from the stored
 * session each time. A folder resolves in many requests now, and every one of
 * them rotates __puus; re-seeding from the file would send the oldest token we
 * own to the second half of the folder. quark_logout and quark_set_cookie clear
 * it, so a new login can never inherit the previous account's jar. */
static char g_dl_cookie[QUARK_COOKIE_MAX];
static int g_dl_capture;
static int g_dl_rot_logged;

/* ------------------------------------------------------------- utilities */

const char *
quark_last_raw(void) {
  return g_raw;
}

const char *
quark_last_error(void) {
  return g_err;
}

const char *
quark_download_cookie(void) {
  return g_dl_cookie[0] ? g_dl_cookie : NULL;
}

/* FNV-1a, used only to answer "did this response change the jar?" - a 7 KB
 * strcmp against a copy would need another 7 KB of stack for the same answer. */
static unsigned long long
fnv1a(const char *s) {
  unsigned long long h = 1469598103934665603ULL;

  while(s && *s) {
    h ^= (unsigned char)*s++;
    h *= 1099511628211ULL;
  }
  return h;
}

/* Keeps the head of a response so a failure can quote what actually arrived.
 * Bodies here are small JSON documents, so 600 bytes is enough to see a status
 * code, a message and the shape of the payload. */
static void
keep_raw(const char *body, size_t len) {
  size_t n = len < QUARK_RAW_MAX ? len : QUARK_RAW_MAX;

  if(!body) {
    g_raw[0] = 0;
    return;
  }
  memcpy(g_raw, body, n);
  g_raw[n] = 0;
  /* Newlines would wreck the one-line status field this ends up in. */
  for(n = 0; g_raw[n]; n++) {
    if(g_raw[n] == '\r' || g_raw[n] == '\n' || g_raw[n] == '\t') {
      g_raw[n] = ' ';
    }
  }
}

/* Copies at most `max` bytes of src, never splitting a UTF-8 sequence.
 *
 * The message a refusal carries is the only statement of *why* the service said
 * no, and it is Chinese: cutting it at a byte count leaves half a character at
 * the end, which the page renders as a replacement glyph right where the reason
 * was. */
static void
clip_utf8(char *out, size_t cap, const char *src, size_t max) {
  size_t i = 0;
  size_t keep = 0;

  if(cap == 0) {
    return;
  }
  out[0] = 0;
  if(!src) {
    return;
  }
  while(src[i] && i < max) {
    unsigned char c = (unsigned char)src[i];
    size_t len = c < 0x80 ? 1
                 : (c >> 5) == 0x6 ? 2
                 : (c >> 4) == 0xE ? 3
                 : (c >> 3) == 0x1E ? 4
                                    : 1;

    if(i + len > max || i + len >= cap) {
      break;
    }
    i += len;
    keep = i;
  }
  memcpy(out, src, keep);
  out[keep] = 0;
}

/* A growable body buffer. Listings of 50 entries run to tens of kilobytes, so
 * this cannot be a fixed array on a thread stack. */
typedef struct {
  char *p;
  size_t len;
  size_t cap;
  int oom;
} body_buf;

static int
body_write(void *ctx, const void *data, size_t len) {
  body_buf *b = (body_buf *)ctx;
  size_t need;

  if(b->oom) {
    return -1;
  }
  need = b->len + len + 1;
  if(need > b->cap) {
    size_t want = b->cap ? b->cap : 4096;
    char *np;

    while(want < need) {
      want *= 2;
    }
    /* A runaway response is a bug or an attack, not a big directory. */
    if(want > 8u * 1024u * 1024u) {
      b->oom = 1;
      return -1;
    }
    np = (char *)realloc(b->p, want);
    if(!np) {
      b->oom = 1;
      return -1;
    }
    b->p = np;
    b->cap = want;
  }
  memcpy(b->p + b->len, data, len);
  b->len += len;
  b->p[b->len] = 0;
  return 0;
}

/* ---------------------------------------------------------------- session */

int
quark_init(const char *data_dir) {
  snprintf(g_data, sizeof(g_data), "%s", data_dir ? data_dir : ".");
  return 0;
}

static void
session_path(char *out, size_t cap) {
  snprintf(out, cap, "%s/cookie.txt", g_data);
}

static void
account_path(char *out, size_t cap) {
  snprintf(out, cap, "%s/account.txt", g_data);
}

/* Two files rather than one. cookie.txt holds nothing but the cookie, so it can
 * be opened, replaced or deleted by hand and still mean exactly one thing;
 * account.txt holds the nickname and quota, which are display data that may
 * legitimately be stale and must never be mistaken for credentials. */
int
quark_load(quark_session *s) {
  char path[600];
  FILE *f;
  size_t n;

  memset(s, 0, sizeof(*s));
  session_path(path, sizeof(path));
  f = fopen(path, "r");
  if(!f) {
    return -1;
  }
  n = fread(s->cookie, 1, sizeof(s->cookie) - 1, f);
  fclose(f);
  s->cookie[n] = 0;
  while(n && (s->cookie[n - 1] == '\n' || s->cookie[n - 1] == '\r')) {
    s->cookie[--n] = 0;
  }
  if(n < 8) {
    s->cookie[0] = 0;
    return -1;
  }
  s->valid = 1;
  /* A stored cookie is assumed good until a call says otherwise. Verifying it
   * at start-up would put a network round trip on the load path of every boot,
   * including the ones where nobody opens the page. */
  s->verified = 1;

  account_path(path, sizeof(path));
  f = fopen(path, "r");
  if(f) {
    char line[300];

    if(fgets(line, sizeof(line), f)) {
      line[strcspn(line, "\r\n")] = 0;
      snprintf(s->nickname, sizeof(s->nickname), "%s", line);
    }
    if(fgets(line, sizeof(line), f)) {
      /* "total used" on one line; either half may be missing. */
      long long a = 0;
      long long b = 0;

      if(sscanf(line, "%lld %lld", &a, &b) == 2) {
        s->total_bytes = a;
        s->used_bytes = b;
      }
    }
    fclose(f);
  }
  return 0;
}

int
quark_save(const quark_session *s) {
  char path[600];
  FILE *f;

  if(!s->valid || !s->cookie[0]) {
    return -1;
  }
  session_path(path, sizeof(path));
  f = fopen(path, "w");
  if(!f) {
    return -1;
  }
  /* A session cookie is a bearer token in every sense that matters: anyone who
   * reads this file is logged in as the user. The permission is applied right
   * after creation, inside the same open, so there is no window in which the
   * file exists world-readable. */
  fchmod(fileno(f), 0600);
  fwrite(s->cookie, 1, strlen(s->cookie), f);
  fclose(f);
  chmod(path, 0600);

  account_path(path, sizeof(path));
  f = fopen(path, "w");
  if(f) {
    fchmod(fileno(f), 0600);
    if(s->nickname[0]) {
      fprintf(f, "%s\n", s->nickname);
    } else {
      fprintf(f, "\n");
    }
    fprintf(f, "%lld %lld\n", s->total_bytes, s->used_bytes);
    fclose(f);
    chmod(path, 0600);
  }
  return 0;
}

void
quark_logout(void) {
  char path[600];

  session_path(path, sizeof(path));
  unlink(path);
  account_path(path, sizeof(path));
  unlink(path);
  /* The download jar belongs to the account that just left. Keeping it would
   * let the next login mint links under the previous session's __puus. */
  g_dl_cookie[0] = 0;
}

/* ---------------------------------------------------------- cookie import */

/* The paste parser itself lives in cookie.c, because it is the one piece of this
 * file that can be tested without a console, a network or an account. What is
 * left here is turning its answer into a session. */

int
quark_set_cookie(const char *raw, quark_session *s, char *err, size_t cap) {
  char clean[QUARK_COOKIE_MAX];
  int n;

  if(!raw) {
    snprintf(err, cap, "内容为空");
    return -1;
  }
  n = cookie_extract(raw, clean, sizeof(clean));
  if(n == 0) {
    /* Two different failures, two different fixes, so they say different
     * things. Zero found means the text had no cookie in it at all, which in
     * practice means the request being inspected never carried one — the
     * developer tools say "Provisional headers are shown" when that happens. */
    snprintf(err, cap,
             "这段内容里没有 cookie。它应该长这样：__pus=xxx; __puus=yyy; "
             "__kp=zzz。若开发者工具显示「Provisional headers are shown」，"
             "说明那条请求没真正发出（或来自缓存），换一条状态 200 的再复制。");
    return -1;
  }
  if(n < 8) {
    snprintf(err, cap, "cookie 太短了（%d 字节），应该是完整的一串 a=b; c=d",
             n);
    return -1;
  }
  memset(s, 0, sizeof(*s));
  snprintf(s->cookie, sizeof(s->cookie), "%s", clean);
  s->valid = 1;
  s->verified = 0;
  /* A freshly pasted cookie is a different session than the one the jar holds. */
  g_dl_cookie[0] = 0;
  return 0;
}

int
quark_cookie_keys(const char *cookie, char *out, size_t cap, int *count) {
  const char *p = cookie;
  size_t o = 0;
  int n = 0;

  if(out && cap) out[0] = 0;
  if(count) *count = 0;
  if(!cookie) return 0;

  while(*p) {
    const char *name = p;
    size_t len;

    while(*p && *p != '=' && *p != ';') p++;
    len = (size_t)(p - name);
    while(len && (*name == ' ' || *name == '\t')) {
      name++;
      len--;
    }
    if(len) {
      n++;
      /* Names only, no values: this is meant to be shown on screen and a cookie
       * value is a credential. */
      if(out && o + len + 2 < cap) {
        if(o) out[o++] = ',';
        memcpy(out + o, name, len);
        o += len;
        out[o] = 0;
      }
    }
    while(*p && *p != ';') p++;
    if(*p == ';') p++;
  }
  if(count) *count = n;
  return n;
}

/* ------------------------------------------------------------- scan login */

/* Four requests, taken from pan.quark.cn's own front-end bundle and confirmed
 * against the live service (the poll endpoint answers {"status":50004001,
 * "message":"Query result is empty"} for a token nobody has scanned yet):
 *
 *   1. GET  uop.quark.cn/cas/ajax/getTokenForQrcodeLogin
 *            -> data.members.token
 *   2. the QR image content is a URL on su.quark.cn carrying that token
 *   3. GET  uop.quark.cn/cas/ajax/getServiceTicketByQrcodeToken?token=..
 *            -> 2000000 + data.members.service_ticket once the phone confirms
 *   4. GET  pan.quark.cn/account/info?st=<ticket>&lw=scan
 *            -> the session arrives in Set-Cookie, nothing else
 *
 * Step 4 is why httpc grew a cookie jar. There is no JSON body worth reading
 * there, and no other endpoint in this program ever needs Set-Cookie. */

/* The host of an absolute URL, for error messages. Never the full URL: the
 * query strings here are one-shot tokens. */
static void
fetch_host(const char *url, char *out, size_t cap) {
  const char *p = strstr(url, "://");
  size_t n = 0;

  p = p ? p + 3 : url;
  while(p[n] && p[n] != '/' && p[n] != '?' && n + 1 < cap) {
    out[n] = p[n];
    n++;
  }
  out[n] = 0;
}

/* One request to an arbitrary host, with or without a session. Every listing
 * call goes through call(), which is pinned to the API host and refuses to run
 * without a cookie - both of which the login flow predates. `out` may be NULL
 * when the answer's body is not wanted. */
static int
fetch(const char *url, const char *cookie, const char *accept, char **out,
      http_result *meta, char *err, size_t errcap) {
  http_req req;
  body_buf buf;
  char *hdr = NULL;
  int rc;

  memset(&req, 0, sizeof(req));
  memset(&buf, 0, sizeof(buf));
  memset(meta, 0, sizeof(*meta));
  if(out) {
    *out = NULL;
  }

  if(cookie && *cookie) {
    /* Heap, not stack: the header is a few kilobytes of credential and this
     * runs on an HTTP worker thread whose stack is not ours to gamble with. */
    size_t need = strlen(cookie) + 200;

    hdr = (char *)malloc(need);
    if(!hdr) {
      snprintf(err, errcap, "内存不足");
      return -1;
    }
    snprintf(hdr, need,
             "Cookie: %s\r\nAccept: %s\r\nOrigin: " QUARK_ORIGIN
             "\r\nReferer: " QUARK_ORIGIN "/\r\n"
             "Accept-Language: zh-CN,zh;q=0.9\r\n",
             cookie, accept ? accept : "application/json, text/plain, */*");
    req.extra_headers = hdr;
  } else if(accept) {
    size_t need = strlen(accept) + 160;

    hdr = (char *)malloc(need);
    if(!hdr) {
      snprintf(err, errcap, "内存不足");
      return -1;
    }
    snprintf(hdr, need, "Accept: %s\r\nReferer: " QUARK_ORIGIN "/\r\n", accept);
    req.extra_headers = hdr;
  }

  req.url = url;
  req.user_agent = QUARK_UA;
  req.timeout_ms = 20000;
  if(out) {
    req.write = body_write;
    req.write_ctx = &buf;
  }

  rc = http_perform(&req, meta);
  free(hdr);
  if(rc != 0) {
    snprintf(err, errcap, "%s", meta->err[0] ? meta->err : "请求失败");
    free(buf.p);
    return -1;
  }
  if(meta->status != 200) {
    /* Host only: the query string of every URL here carries a one-shot token,
     * and an error message ends up on a screen that gets photographed. */
    char host[80];

    fetch_host(url, host, sizeof(host));
    snprintf(err, errcap, "HTTP %d（%s）", meta->status, host);
    free(buf.p);
    return -1;
  }
  if(out) {
    if(!buf.p) {
      snprintf(err, errcap, "服务器返回了空内容");
      return -1;
    }
    *out = buf.p;
  } else {
    free(buf.p);
  }
  return 0;
}

int
quark_qr_new(char *url, size_t urlcap, char *token, size_t tokcap, char *err,
             size_t errcap) {
  char full[600];
  char rid[64];
  char jerr[120];
  char *body = NULL;
  static http_result meta;
  json_value *v;
  const char *t;
  long long code;

  snprintf(rid, sizeof(rid), "%lx%04lx", (unsigned long)time(NULL),
           (unsigned long)(getpid() & 0xffff));
  snprintf(full, sizeof(full),
           "https://uop.quark.cn/cas/ajax/getTokenForQrcodeLogin"
           "?client_id=" QUARK_CLIENT_ID "&v=1.2&request_id=%s",
           rid);

  if(fetch(full, NULL, "application/json, text/plain, */*", &body, &meta, err,
           errcap)) {
    return -1;
  }
  v = json_parse(body, jerr, sizeof(jerr));
  free(body);
  if(!v) {
    snprintf(err, errcap, "取扫码 token 的返回不是 JSON：%s", jerr);
    return -1;
  }
  code = json_int(json_get(v, "status"), -1);
  t = json_str(json_path(v, "data.members.token"), NULL);
  if(code != 2000000 || !t || !*t) {
    snprintf(err, errcap, "取扫码 token 失败（status=%lld %.60s）", code,
             json_str(json_get(v, "message"), ""));
    json_free(v);
    return -1;
  }
  snprintf(token, tokcap, "%s", t);
  json_free(v);

  /* The token is alphanumeric, so it goes into the query without escaping.
   * Everything after ssb is what the official page sends; the app reads it to
   * pick the scan-login layout, and leaving it out changes the landing page. */
  snprintf(url, urlcap,
           "https://su.quark.cn/4_eMHBJ?token=%s&client_id=" QUARK_CLIENT_ID
           "&ssb=weblogin&uc_param_str=&uc_biz_str=S:custom|OPT:SAREA@0|OPT:"
           "IMMERSIVE@1|OPT:BACK_BTN_STYLE@0",
           token);
  return 0;
}

/* Step 4: the ticket is worth a session only once, immediately, so it is
 * exchanged here rather than handed back to the page. */
static int
qr_exchange(const char *ticket, quark_session *s, char *err, size_t errcap) {
  char url[900];
  static http_result meta;

  snprintf(url, sizeof(url), QUARK_ORIGIN "/account/info?st=%s&lw=scan",
           ticket);
  /* The ticket itself never reaches the log: it is a one-shot credential and
   * this file is readable over FTP. Its length is enough to tell "empty" from
   * "truncated". */
  boot_trace("换会话: 票据 %d 字节, 请求 %s", (int)strlen(ticket), QUARK_ORIGIN);
  if(fetch(url, NULL, "text/html,application/json,*/*", NULL, &meta, err,
           errcap)) {
    boot_trace("换会话失败: %s", err);
    return -1;
  }
  boot_trace("换会话返回 status=%d, Set-Cookie %d 字节", meta.status,
             (int)strlen(meta.set_cookie));
  if(!meta.set_cookie[0]) {
    snprintf(err, errcap, "登录响应里没有 Set-Cookie，扫码未生效");
    return -1;
  }
  /* Without these two the service answers every later call with 401, and the
   * page would show "登录成功" followed by an empty drive. */
  if(!strstr(meta.set_cookie, "__pus") && !strstr(meta.set_cookie, "__puus")) {
    /* No value in the message: a session cookie on a screen is a credential in
     * a photograph. The names alone say what went wrong. */
    snprintf(err, errcap,
             "登录响应里没有 __pus/__puus（只有其它 Cookie），扫码未生效");
    return -1;
  }
  memset(s, 0, sizeof(*s));
  snprintf(s->cookie, sizeof(s->cookie), "%s", meta.set_cookie);
  s->valid = 1;
  /* A new login means a new session; any jar rotated under the old one is not
   * just stale, it belongs to somebody else. */
  g_dl_cookie[0] = 0;
  /* The service has just confirmed who this is; asking again would be a second
   * round trip for information the caller already has. */
  s->verified = 1;
  if(quark_save(s)) {
    boot_trace("写 cookie.txt 失败");
    snprintf(err, errcap, "会话已拿到但写入 cookie.txt 失败");
    return -1;
  }
  boot_trace("登录完成, 会话 %d 字节已落盘", (int)strlen(s->cookie));
  return 0;
}

int
quark_qr_poll(const char *token, quark_session *s, char *err, size_t errcap) {
  char url[700];
  char rid[64];
  char jerr[120];
  char ticket[600];
  char *body = NULL;
  static http_result meta;
  json_value *v;
  const char *t;
  long long code;

  if(!token || !*token) {
    snprintf(err, errcap, "没有扫码 token");
    return -1;
  }
  snprintf(rid, sizeof(rid), "%lx%04lx", (unsigned long)time(NULL),
           (unsigned long)(getpid() & 0xffff));
  snprintf(url, sizeof(url),
           "https://uop.quark.cn/cas/ajax/getServiceTicketByQrcodeToken"
           "?client_id=" QUARK_CLIENT_ID "&v=1.2&token=%s&request_id=%s",
           token, rid);

  /* The page polls this every two seconds, and the report was "the QR code
   * appears, then the next request fails and the process is gone". So both ends
   * of each poll are recorded: whichever line is last tells whether it died
   * inside the request, inside the parse, or on the way back. No token, no
   * body - this file is readable over FTP. */
  {
    static int npolls;

    npolls++;
    boot_trace("qr poll #%d 开始", npolls);
  }
  if(fetch(url, NULL, "application/json, text/plain, */*", &body, &meta, err,
           errcap)) {
    boot_trace("qr poll 请求失败: %s", err);
    return -1;
  }
  boot_trace("qr poll 返回 status=%d body=%d 字节", meta.status,
             body ? (int)strlen(body) : -1);
  v = json_parse(body, jerr, sizeof(jerr));
  free(body);
  if(!v) {
    snprintf(err, errcap, "扫码状态的返回不是 JSON：%s", jerr);
    return -1;
  }
  code = json_int(json_get(v, "status"), -1);
  t = json_str(json_path(v, "data.members.service_ticket"), NULL);
  if(code == 2000000 && t && *t) {
    snprintf(ticket, sizeof(ticket), "%s", t);
    json_free(v);
    if(qr_exchange(ticket, s, err, errcap)) {
      return -1;
    }
    return QUARK_QR_OK;
  }
  boot_trace("qr poll 状态 %ld", (long)code);
  json_free(v);

  /* The waiting codes are the ones the live service actually returns: 50004001
   * before anyone scans. The 8000500x family comes from the same client code
   * and covers "scanned, not confirmed yet". A code that is not in either list
   * is reported rather than guessed at, because silently polling forever is how
   * a login page ends up stuck at "等待扫码". */
  switch(code) {
    case 2000000:
    case 50004001:
    case 80005000:
    case 80005001:
    case 80005002:
      return QUARK_QR_WAITING;
    case 50004002:
    case 80005003:
    case 80005004:
      return QUARK_QR_EXPIRED;
    default:
      snprintf(err, errcap, "扫码状态未知（status=%lld）", code);
      return -1;
  }
}

/* -------------------------------------------------------------- transport */

/* Runs one request against the API host. `body` non-NULL makes it a POST with
 * a JSON content type. The answer is left in out (caller frees), and the head
 * of it is kept in g_raw for error reporting. */
static int
call(const quark_session *s, const char *method, const char *path,
     const char *body, char **out, char *err, size_t errcap) {
  char url[1600];
  /* 7 KB of Cookie header, same reasoning as the session above. */
  static char headers[QUARK_COOKIE_MAX + 640];
  http_req req;
  static http_result res;
  body_buf buf;
  int rc;
  char *q;

  if(!s || !s->valid || !s->cookie[0]) {
    snprintf(err, errcap, "还没有登录，先粘贴 cookie");
    return -1;
  }
  if(path[0] != '/') {
    snprintf(err, errcap, "接口路径必须以 / 开头");
    return -1;
  }
  snprintf(url, sizeof(url), "%s%s", QUARK_API, path);
  snprintf(headers, sizeof(headers),
           "Cookie: %s\r\n"
           "Accept: application/json, text/plain, */*\r\n"
           "Origin: " QUARK_ORIGIN "\r\n"
           "Referer: " QUARK_ORIGIN "/\r\n"
           "Accept-Language: zh-CN,zh;q=0.9\r\n",
           s->cookie);
  if(body) {
    size_t used = strlen(headers);

    snprintf(headers + used, sizeof(headers) - used,
             "Content-Type: application/json;charset=UTF-8\r\n");
  }

  memset(&req, 0, sizeof(req));
  req.url = url;
  req.method = method;
  req.extra_headers = headers;
  req.user_agent = QUARK_UA;
  req.body = body;
  req.body_len = body ? strlen(body) : 0;
  req.timeout_ms = 20000;
  /* Without these two the response body goes nowhere and every call reports an
   * empty answer - which reads exactly like an account with no files. */
  req.write = body_write;
  req.write_ctx = &buf;
  memset(&res, 0, sizeof(res));
  memset(&buf, 0, sizeof(buf));

  rc = http_perform(&req, &res);
  if(rc != 0) {
    keep_raw(NULL, 0);
    snprintf(g_err, sizeof(g_err), "%s", res.err);
    snprintf(err, errcap, "%s", res.err[0] ? res.err : "请求失败");
    free(buf.p);
    return -1;
  }
  /* A renewal that arrives with an error status is still the newest value the
   * service knows, so the merge is not conditional on 200.
   *
   * Merged pair by pair: set_cookie is already a jar, and cookie_jar_put would
   * read it as one Set-Cookie line and drop everything after the first pair -
   * which is how __puus gets lost while every log line still says "rotated". */
  if(g_dl_capture && res.set_cookie[0]) {
    cookie_jar_merge(g_dl_cookie, sizeof(g_dl_cookie), res.set_cookie);
  }
  keep_raw(buf.p, buf.len);
  if(res.status != 200) {
    /* The body is quoted here on purpose. A 400 from this service *is* an
     * explanation ("参数错误", "fids 超过上限", ...) and it arrives in the very
     * answer we were about to throw away: without it, a refused request reads
     * as "HTTP 400（不是网络问题）" and the next question is unanswerable. */
    char why[220];

    clip_utf8(why, sizeof(why), g_raw, 170);
    snprintf(g_err, sizeof(g_err), "HTTP %d: %.200s", res.status,
             buf.p ? buf.p : "");
    snprintf(err, errcap, "HTTP %d（请求被拒）%s%s", res.status,
             why[0] ? "：" : "", why);
    free(buf.p);
    return -1;
  }
  if(!buf.p) {
    snprintf(err, errcap, "服务器返回了空内容");
    return -1;
  }
  /* The service likes to hand back JSON with a charset prefix and, once, a
   * UTF-8 BOM. Both break a parser that starts at byte zero. */
  q = buf.p;
  while(*q == '\r' || *q == '\n' || *q == ' ' || *q == '\t') {
    q++;
  }
  if((unsigned char)q[0] == 0xef && (unsigned char)q[1] == 0xbb &&
     (unsigned char)q[2] == 0xbf) {
    q += 3;
  }
  if(*q != '{' && *q != '[') {
    snprintf(err, errcap, "返回的不是 JSON：%.120s", q);
    snprintf(g_err, sizeof(g_err), "返回的不是 JSON：%.200s", q);
    free(buf.p);
    return -1;
  }
  *out = buf.p;
  return 0;
}

/* Turns a business-level failure into a sentence. Quark reports "you are not
 * logged in" as status 401 / code 31001 inside a 200 response, so the HTTP
 * status alone would call that a success. */
static int
check_ok(const json_value *root, char *err, size_t errcap) {
  long long status = json_int(json_get(root, "status"), -1);
  long long code = json_int(json_get(root, "code"), -1);
  const char *msg = json_str(json_get(root, "message"), "");

  /* Success is status 200 with a zero code. Anything else - including a
   * document with neither field, which means the answer is not what this code
   * thinks it is - is a failure. */
  if(status == 200 && (code == 0 || code < 0)) {
    return 0;
  }
  if(status == 401 || code == 31001) {
    snprintf(err, errcap, "登录已失效，请重新粘贴 cookie");
    snprintf(g_err, sizeof(g_err), "401/31001 require login");
    return -1;
  }
  snprintf(err, errcap, "接口返回失败：status=%lld code=%lld %.100s", status,
           code, msg);
  snprintf(g_err, sizeof(g_err), "status=%lld code=%lld %.120s", status, code,
           msg);
  return -1;
}

/* ------------------------------------------------------------------ probe */

int
quark_probe(const quark_session *s, const char *path, char *out, size_t cap,
            char *err, size_t errcap) {
  char *body = NULL;

  if(!path || path[0] != '/') {
    snprintf(err, errcap, "路径要以 / 开头，例如 /1/clouddrive/member?%s",
             QUARK_COMMON);
    return -1;
  }
  if(call(s, "GET", path, NULL, &body, err, errcap)) {
    /* Nothing is written to out on failure: the caller has to escape the error
     * and the raw body before putting them in a JSON string, and doing it here
     * would mean a second escaper in this file. A response body full of quotes
     * is exactly what this is here to show, so it must not be pasted in raw. */
    return -1;
  }
  snprintf(out, cap, "%.*s", (int)(cap - 1), body);
  free(body);
  return 0;
}

/* -------------------------------------------------------------- who am I */

/* Quark has renamed these fields between client versions, so each one is
 * looked for under the names it has actually used. A field that is absent
 * leaves the value at zero instead of failing the whole call: a nickname is
 * cosmetic, and refusing to work because it did not arrive would be silly. */
static long long
first_int(const json_value *root, const char *const *names, int n) {
  int i;

  for(i = 0; i < n; i++) {
    const json_value *v = json_path(root, names[i]);

    if(v && v->type == JSON_NUMBER) {
      return json_int(v, 0);
    }
  }
  return 0;
}

static const char *
first_str(const json_value *root, const char *const *names, int n) {
  int i;

  for(i = 0; i < n; i++) {
    const json_value *v = json_path(root, names[i]);

    if(v && v->type == JSON_STRING && v->string && v->string[0]) {
      return v->string;
    }
  }
  return NULL;
}

int
quark_whoami(quark_session *s, char *err, size_t cap) {
  static const char *const names[] = {"data.nickname", "data.nick_name",
                                      "data.user_name", "nickname"};
  static const char *const totals[] = {"data.total_capacity",
                                       "data.totalCapacity",
                                       "data.capacity", "total_capacity"};
  static const char *const useds[] = {"data.use_capacity", "data.used_capacity",
                                      "data.usedCapacity", "use_capacity"};
  char *body = NULL;
  json_value *root;
  char perr[160];
  const char *nick;

  if(call(s, "GET", "/1/clouddrive/member?" QUARK_COMMON, NULL, &body, err, cap)) {
    return -1;
  }
  root = json_parse(body, perr, sizeof(perr));
  if(!root) {
    snprintf(err, cap, "会员接口返回的内容解析不了：%s", perr);
    free(body);
    return -1;
  }
  if(check_ok(root, err, cap)) {
    json_free(root);
    free(body);
    return -1;
  }
  nick = first_str(root, names, (int)(sizeof(names) / sizeof(names[0])));
  if(nick) {
    snprintf(s->nickname, sizeof(s->nickname), "%s", nick);
  }
  s->total_bytes = first_int(root, totals, (int)(sizeof(totals) / sizeof(totals[0])));
  s->used_bytes = first_int(root, useds, (int)(sizeof(useds) / sizeof(useds[0])));
  s->valid = 1;
  s->verified = 1;
  json_free(root);
  free(body);
  return 0;
}

/* ------------------------------------------------------------------ list */

int
quark_list(const quark_session *s, const char *pdir, int page, int size,
           quark_entry *out, int cap, int *total, char *err, size_t errcap) {
  char path[700];
  char *body = NULL;
  json_value *root;
  const json_value *list;
  char perr[160];
  int n;
  int i;

  if(!pdir || !pdir[0]) {
    pdir = "0";
  }
  if(page < 1) {
    page = 1;
  }
  if(size < 1 || size > 200) {
    size = 50;
  }
  snprintf(path, sizeof(path),
           "/1/clouddrive/file/sort?%s&pdir_fid=%s&_page=%d&_size=%d"
           "&_fetch_total=1&_sort=file_type:asc,updated_at:desc",
           QUARK_COMMON, pdir, page, size);
  if(call(s, "GET", path, NULL, &body, err, errcap)) {
    return -1;
  }
  root = json_parse(body, perr, sizeof(perr));
  if(!root) {
    snprintf(err, errcap, "目录列表解析失败：%s｜原始：%.200s", perr, body);
    free(body);
    return -1;
  }
  if(check_ok(root, err, errcap)) {
    json_free(root);
    free(body);
    return -1;
  }
  list = json_path(root, "data.list");
  if(!list) {
    /* Not an error the user can act on, but it must not look like an empty
     * folder: a changed field name would otherwise read as "你网盘是空的". */
    snprintf(err, errcap, "返回里找不到 data.list｜原始：%.240s", body);
    json_free(root);
    free(body);
    return -1;
  }
  if(total) {
    const json_value *t = json_path(root, "metadata._total");

    if(!t) {
      t = json_path(root, "data.metadata._total");
    }
    *total = (int)json_int(t, json_count(list));
  }
  n = json_count(list);
  if(n > cap) {
    n = cap;
  }
  for(i = 0; i < n; i++) {
    const json_value *item = json_at(list, i);
    const json_value *dirv;
    quark_entry *e = &out[i];

    memset(e, 0, sizeof(*e));
    snprintf(e->fid, sizeof(e->fid), "%s",
             json_str(json_get(item, "fid"), ""));
    snprintf(e->name, sizeof(e->name), "%s",
             json_str(json_get(item, "file_name"), ""));
    if(!e->name[0]) {
      snprintf(e->name, sizeof(e->name), "%s",
               json_str(json_get(item, "name"), "(未命名)"));
    }
    dirv = json_get(item, "dir");
    if(dirv) {
      e->is_dir = json_bool(dirv, 0);
    } else {
      /* Older payloads use file_type: 1 for a folder. */
      e->is_dir = json_int(json_get(item, "file_type"), 0) == 1;
    }
    e->size = json_int(json_get(item, "size"), 0);
    e->updated_at = json_int(json_get(item, "updated_at"), 0);
  }
  json_free(root);
  free(body);
  return n;
}

/* ---------------------------------------------------------- direct links */

int
quark_direct(const quark_session *s, const char *fid, char *url, size_t cap,
             char *err, size_t errcap) {
  char body[QUARK_FID_MAX + 32];
  char *resp = NULL;
  json_value *root;
  const json_value *item;
  const char *u;
  char perr[160];

  if(!fid || !fid[0]) {
    snprintf(err, errcap, "文件 id 为空");
    return -1;
  }
  snprintf(body, sizeof(body), "{\"fids\":[\"%s\"]}", fid);
  /* GET is refused with 405 by the service, so this must be a POST with a JSON
   * body even though it reads like a query. */
  if(call(s, "POST", "/1/clouddrive/file/download?" QUARK_COMMON, body, &resp,
          err, errcap)) {
    return -1;
  }
  root = json_parse(resp, perr, sizeof(perr));
  if(!root) {
    snprintf(err, errcap, "直链接口解析失败：%s｜原始：%.200s", perr, resp);
    free(resp);
    return -1;
  }
  if(check_ok(root, err, errcap)) {
    json_free(root);
    free(resp);
    return -1;
  }
  item = json_at(json_path(root, "data"), 0);
  u = item ? json_str(json_get(item, "download_url"), NULL) : NULL;
  if(!u || !u[0]) {
    snprintf(err, errcap, "直链没有返回｜原始：%.240s", resp);
    json_free(root);
    free(resp);
    return -1;
  }
  snprintf(url, cap, "%s", u);
  json_free(root);
  free(resp);
  return 0;
}

/* Exchanges exactly `n` fids for URLs in one request.
 *
 * Whether the service will take a group of them is another matter: see the
 * note on g_batch_max below. This function is one attempt, nothing more. */
static int
dl_fetch(const quark_session *s, const char *const *fids, int n,
         char urls[][QUARK_URL_MAX], char *err, size_t errcap) {
  char *body;
  size_t cap = (size_t)n * (QUARK_FID_MAX + 8) + 64;
  size_t used = 0;
  char *resp = NULL;
  json_value *root;
  const json_value *data;
  char perr[160];
  int i;
  int rc;
  int got = 0;
  int too_long = 0;

  if(n <= 0) {
    snprintf(err, errcap, "没有要换的文件");
    return -1;
  }
  body = (char *)malloc(cap);
  if(!body) {
    snprintf(err, errcap, "内存不足");
    return -1;
  }
  used += (size_t)snprintf(body + used, cap - used, "{\"fids\":[");
  for(i = 0; i < n; i++) {
    if(!fids[i] || !fids[i][0]) {
      free(body);
      snprintf(err, errcap, "第 %d 个文件没有 id", i + 1);
      return -1;
    }
    /* fids come from the listing, not from the user, but a quote in one would
     * still break the request body - and the body is what the service reads. */
    if(strchr(fids[i], '"') || strchr(fids[i], '\\')) {
      free(body);
      snprintf(err, errcap, "文件 id 含非法字符");
      return -1;
    }
    used += (size_t)snprintf(body + used, cap - used, "%s\"%s\"", i ? "," : "",
                             fids[i]);
  }
  snprintf(body + used, cap - used, "]}");

  /* Seeded here rather than before the call so a request that never leaves the
   * machine cannot publish a stale jar as "the one for this batch". Once a jar
   * exists it is the newest one the account has given us, so it is reused. */
  {
    unsigned long long before;

    if(!g_dl_cookie[0]) {
      snprintf(g_dl_cookie, sizeof(g_dl_cookie), "%s", s->cookie);
    }
    before = fnv1a(g_dl_cookie);
    g_dl_capture = 1;
    rc = call(s, "POST", "/1/clouddrive/file/download?" QUARK_COMMON, body,
              &resp, err, errcap);
    g_dl_capture = 0;
    if(rc) {
      free(body);
      return -1;
    }
    free(body);
    /* Every one of these responses rotates __puus, so a folder of fifty files
     * would write fifty identical lines over the trail that matters. The first
     * few are the evidence; the rest is noise. */
    if(fnv1a(g_dl_cookie) != before && g_dl_rot_logged < 3) {
      g_dl_rot_logged++;
      boot_trace("直链: 响应轮换了会话 cookie（%u 字节），下载将用它",
                 (unsigned)strlen(g_dl_cookie));
    }
  }
  root = json_parse(resp, perr, sizeof(perr));
  if(!root) {
    snprintf(err, errcap, "直链接口解析失败：%s｜原始：%.200s", perr, resp);
    free(resp);
    return -1;
  }
  if(check_ok(root, err, errcap)) {
    json_free(root);
    free(resp);
    return -1;
  }
  data = json_path(root, "data");
  for(i = 0; i < json_count(data); i++) {
    const json_value *item = json_at(data, i);
    const char *fid = json_str(json_get(item, "fid"), NULL);
    const char *u = json_str(json_get(item, "download_url"), NULL);
    int k;

    if(!fid || !u || !u[0]) {
      continue;
    }
    /* The answer is not promised to come back in the order it was asked, so
     * the fid is what decides which URL belongs to which file. */
    for(k = 0; k < n; k++) {
      if(fids[k] && !strcmp(fids[k], fid)) {
        /* A cut-off direct link is worse than no link: it is answered 412 or
         * "SignatureDoesNotMatch" instead of "not here", so it reads like the
         * session is wrong. Reported, not shortened. */
        if(strlen(u) >= QUARK_URL_MAX) {
          too_long = 1;
          boot_trace("直链过长：%u 字节（上限 %d），已跳过 %s", (unsigned)strlen(u),
                     QUARK_URL_MAX - 1, fids[k]);
          break;
        }
        snprintf(urls[k], QUARK_URL_MAX, "%s", u);
        got++;
        break;
      }
    }
  }
  json_free(root);
  free(resp);
  if(!got) {
    if(too_long) {
      snprintf(err, errcap, "直链超过 %d 字节上限", QUARK_URL_MAX - 1);
    } else {
      snprintf(err, errcap, "一个直链都没换到｜原始：%.240s", quark_last_raw());
    }
    return -1;
  }
  return got;
}

/* How many fids the service takes in one request, learned at run time.
 *
 * A folder of nineteen files was refused with HTTP 400 while the same account
 * resolved a single file a minute earlier, and a folder of five failed the same
 * way, so the number of fids in the body is what the service objects to - but
 * what number it will take is not documented anywhere and is not stated in the
 * refusal either. Guessing a constant is a guess in one of two directions:
 * too big and folders stay broken, too small and every folder costs a round
 * trip per file forever.
 *
 * So it is measured. A group that is refused is halved and retried, down to one
 * fid - a group of one is known to work, it is how the single-file download
 * succeeded. Whatever size survives is remembered for the rest of the process,
 * so the second folder pays nothing for the first one's discovery.
 *
 * The important part is the floor: a refusal costs round trips, never the
 * download. Falling back to one fid at a time is what makes a folder usable
 * even when the answer is "the service will not take two of these at once". */
static int g_batch_max = 50;

static int
dl_resolve(const quark_session *s, const char *const *fids, int n,
           char urls[][QUARK_URL_MAX], int depth, char *err, size_t errcap) {
  int got;
  int half;
  int a;
  int b;
  char tail[260];

  if(n <= 0) {
    return 0;
  }
  /* depth only grows by halving, so it is a guard against a logic error, not
   * against the service: 8 levels already covers a group of 2000. */
  if(depth >= 8) {
    snprintf(err, errcap, "直链拆分过深（%d 个文件）", n);
    return -1;
  }
  got = dl_fetch(s, fids, n, urls, err, errcap);
  if(got >= 0) {
    return got;
  }
  if(n == 1) {
    /* One fid, refused on its own. Nothing to split: this file is the problem,
     * and err already says what the service called it. */
    boot_trace("直链: 单个文件被拒（%s）", err);
    return -1;
  }

  half = n / 2;
  if(half < 1) {
    half = 1;
  }
  if(half < g_batch_max) {
    g_batch_max = half;
    boot_trace("直链: %d 个一起请求被拒（%.120s），改成每次最多 %d 个", n, err,
               g_batch_max);
  }
  a = dl_resolve(s, fids, half, urls, depth + 1, err, errcap);
  tail[0] = 0;
  b = dl_resolve(s, fids + half, n - half, (char(*)[QUARK_URL_MAX])urls + half,
                 depth + 1, tail, sizeof(tail));
  if(a < 0 && b < 0) {
    /* Report the half that failed last: with per-file requests that is the
     * same message, and it is the one that names the file. */
    if(tail[0]) {
      snprintf(err, errcap, "%s", tail);
    }
    return -1;
  }
  return (a > 0 ? a : 0) + (b > 0 ? b : 0);
}

int
quark_direct_many(const quark_session *s, const char *const *fids, int n,
                  char urls[][QUARK_URL_MAX], char *err, size_t errcap) {
  int base = 0;
  int total = 0;
  int hinted = 0;
  char last[260];

  last[0] = 0;
  if(n <= 0) {
    snprintf(err, errcap, "没有要换的文件");
    return -1;
  }
  while(base < n) {
    int take = g_batch_max > 0 ? g_batch_max : 1;
    int got;

    if(take > n - base) {
      take = n - base;
    }
    got = dl_resolve(s, fids + base, take, (char(*)[QUARK_URL_MAX])urls + base,
                     0, err, errcap);
    if(got < 0) {
      snprintf(last, sizeof(last), "%s", err);
    } else {
      total += got;
    }
    /* The size can shrink inside dl_resolve, so the next chunk is measured
     * again instead of trusting the one this iteration started with. */
    if(g_batch_max < 50 && !hinted) {
      hinted = 1;
      boot_trace("直链: 本次按每个请求最多 %d 个文件换直链", g_batch_max);
    }
    base += take;
  }
  if(total == 0) {
    if(!last[0]) {
      snprintf(last, sizeof(last), "一个直链都没换到｜原始：%.200s",
               quark_last_raw());
    }
    snprintf(err, errcap, "%s", last);
    boot_trace("直链: %d 个文件全部失败（%.160s）", n, last);
    return -1;
  }
  if(total < n) {
    boot_trace("直链: %d 个文件里换到 %d 条", n, total);
  }
  return total;
}

/* ------------------------------------------------------------------ walk */

int
quark_walk(const quark_session *s, const char *pdir, int depth_limit,
           quark_visit_cb cb, void *ctx, int *visited, char *err,
           size_t errcap) {
  /* An explicit stack rather than recursion: a deep tree on a console thread
   * with 8 KB of request headers already in the frame is a bad place to
   * recurse, and the pending list is what makes the depth limit expressible. */
  typedef struct {
    char fid[QUARK_FID_MAX];
    /* The path from the walked root down to this directory, using the folder
     * names as Quark listed them ("剧集/第01话"). Empty string at the root. */
    char rel[QUARK_NAME_MAX * 4];
    int depth;
  } pending;
  pending *stack;
  int sp = 0;
  int cap = 64;
  int seen = 0;
  int failed = 0;
  quark_entry *page;

  stack = (pending *)malloc((size_t)cap * sizeof(*stack));
  page = (quark_entry *)malloc(200 * sizeof(*page));
  if(!stack || !page) {
    free(stack);
    free(page);
    snprintf(err, errcap, "内存不足");
    return -1;
  }
  snprintf(stack[0].fid, sizeof(stack[0].fid), "%s", (pdir && pdir[0]) ? pdir : "0");
  stack[0].rel[0] = 0;
  stack[0].depth = 0;
  sp = 1;

  while(sp > 0) {
    char cur[QUARK_FID_MAX];
    char currel[sizeof(stack[0].rel)];
    int depth = stack[--sp].depth;
    int page_no = 1;
    int total = 0;

    snprintf(cur, sizeof(cur), "%s", stack[sp].fid);
    snprintf(currel, sizeof(currel), "%s", stack[sp].rel);
    for(;;) {
      int n = quark_list(s, cur, page_no, 200, page, 200, &total, err, errcap);
      int i;

      if(n < 0) {
        failed = 1;
        break;
      }
      for(i = 0; i < n; i++) {
        if(page[i].is_dir) {
          if(depth + 1 > depth_limit) {
            /* Stopping silently here would produce a download that looks
             * complete and is not, so it is recorded as a failure instead. */
            snprintf(err, errcap, "目录层级超过 %d 层（%s），没有继续深入",
                     depth_limit, page[i].name);
            failed = 1;
            break;
          }
          if(sp >= cap) {
            pending *ns;

            cap *= 2;
            ns = (pending *)realloc(stack, (size_t)cap * sizeof(*stack));
            if(!ns) {
              snprintf(err, errcap, "内存不足");
              failed = 1;
              break;
            }
            stack = ns;
          }
          snprintf(stack[sp].fid, sizeof(stack[sp].fid), "%s", page[i].fid);
          /* Extend the parent's relative path with this folder's name, so the
           * files inside come back with a path they can be saved under. */
          if(currel[0]) {
            snprintf(stack[sp].rel, sizeof(stack[sp].rel), "%s/%s", currel,
                     page[i].name);
          } else {
            snprintf(stack[sp].rel, sizeof(stack[sp].rel), "%s", page[i].name);
          }
          stack[sp].depth = depth + 1;
          sp++;
        } else {
          if(cb && cb(ctx, &page[i], currel)) {
            snprintf(err, errcap, "回调中止");
            failed = 1;
            break;
          }
          seen++;
        }
      }
      if(failed) {
        break;
      }
      if(n < 200 || page_no * 200 >= total) {
        break;
      }
      page_no++;
      if(page_no > 200) {
        snprintf(err, errcap, "目录分页过多，可能有问题");
        failed = 1;
        break;
      }
    }
    if(failed) {
      break;
    }
  }
  free(stack);
  free(page);
  if(visited) {
    *visited = seen;
  }
  return failed ? -1 : 0;
}
