#include "quark.h"

#include "httpc.h"
#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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

/* ------------------------------------------------------------- utilities */

const char *
quark_last_raw(void) {
  return g_raw;
}

const char *
quark_last_error(void) {
  return g_err;
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
}

int
quark_set_cookie(const char *raw, quark_session *s, char *err, size_t cap) {
  const char *p = raw;
  char clean[QUARK_COOKIE_MAX];
  size_t o = 0;
  int eq = 0;

  if(!raw) {
    snprintf(err, cap, "内容为空");
    return -1;
  }
  while(*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
    p++;
  }
  /* People paste either the value or the whole header line, because the
   * developer tools present it as "Cookie: ..." and that is what gets
   * selected. Both are accepted. */
  if((p[0] == 'c' || p[0] == 'C') && (p[1] == 'o' || p[1] == 'O') &&
     (p[2] == 'o' || p[2] == 'O') && (p[3] == 'k' || p[3] == 'K') &&
     (p[4] == 'i' || p[4] == 'I') && (p[5] == 'e' || p[5] == 'E') &&
     p[6] == ':') {
    p += 7;
    while(*p == ' ' || *p == '\t') {
      p++;
    }
  }
  for(; *p && o + 1 < sizeof(clean); p++) {
    /* A multi-line paste (a copied request header block) is folded to one
     * header value: a bare newline inside a header would split the request and
     * the server would see a truncated cookie with no explanation. */
    if(*p == '\r' || *p == '\n' || *p == '\t') {
      if(o && clean[o - 1] != ' ') {
        clean[o++] = ' ';
      }
      continue;
    }
    if(*p == '=') {
      eq = 1;
    }
    clean[o++] = *p;
  }
  while(o && (clean[o - 1] == ' ' || clean[o - 1] == ';')) {
    o--;
  }
  clean[o] = 0;
  if(!eq || o < 16) {
    snprintf(err, cap,
             "这段内容不像 cookie（长度 %lu，没有等号就是没粘对）。"
             "要的是浏览器开发者工具里 Cookie 那一行的值。",
             (unsigned long)o);
    return -1;
  }
  memset(s, 0, sizeof(*s));
  snprintf(s->cookie, sizeof(s->cookie), "%s", clean);
  s->valid = 1;
  s->verified = 0;
  return 0;
}

/* -------------------------------------------------------------- transport */

/* Runs one request against the API host. `body` non-NULL makes it a POST with
 * a JSON content type. The answer is left in out (caller frees), and the head
 * of it is kept in g_raw for error reporting. */
static int
call(const quark_session *s, const char *method, const char *path,
     const char *body, char **out, char *err, size_t errcap) {
  char url[1600];
  char headers[QUARK_COOKIE_MAX + 640];
  http_req req;
  http_result res;
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
  keep_raw(buf.p, buf.len);
  if(res.status != 200) {
    snprintf(g_err, sizeof(g_err), "HTTP %d: %.200s", res.status,
             buf.p ? buf.p : "");
    snprintf(err, errcap, "HTTP %d（不是网络问题，是这个请求被拒了）",
             res.status);
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
    /* Even a failure is worth showing raw: that is how a wrong path or an
     * expired session gets identified without another round trip. */
    snprintf(out, cap, "{\"ok\":false,\"error\":\"%s\",\"raw\":\"%.400s\"}",
             err, g_raw);
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

/* Exchanges up to `n` fids for URLs in one request. The service accepts a fids
 * array, and asking one at a time costs a full round trip each: selecting a
 * hundred files would mean a hundred requests and half a minute of staring at
 * a spinner that has not moved. */
int
quark_direct_many(const quark_session *s, const char *const *fids, int n,
                  char urls[][QUARK_URL_MAX], char *err, size_t errcap) {
  char *body;
  size_t cap = (size_t)n * (QUARK_FID_MAX + 8) + 64;
  size_t used = 0;
  char *resp = NULL;
  json_value *root;
  const json_value *data;
  char perr[160];
  int i;
  int got = 0;

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

  if(call(s, "POST", "/1/clouddrive/file/download?" QUARK_COMMON, body, &resp,
          err, errcap)) {
    free(body);
    return -1;
  }
  free(body);
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
        snprintf(urls[k], QUARK_URL_MAX, "%s", u);
        got++;
        break;
      }
    }
  }
  json_free(root);
  free(resp);
  if(!got) {
    snprintf(err, errcap, "一个直链都没换到｜原始：%.240s", quark_last_raw());
    return -1;
  }
  return got;
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
  stack[0].depth = 0;
  sp = 1;

  while(sp > 0) {
    char cur[QUARK_FID_MAX];
    int depth = stack[--sp].depth;
    int page_no = 1;
    int total = 0;

    snprintf(cur, sizeof(cur), "%s", stack[sp].fid);
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
          stack[sp].depth = depth + 1;
          sp++;
        } else {
          if(cb && cb(ctx, &page[i], cur)) {
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
