/* HTTP/1.1 client: one request per connection, closed afterwards.
 *
 * Everything here is deliberately synchronous and bounded. A download worker
 * calls http_perform once per attempt and gets back either a complete response
 * or a sentence explaining which stage failed, which is more useful on a
 * console than an errno. */

#include "httpc.h"

#include <pthread.h>

#include "cookie.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "build.h"
#include "net.h"

/* Room for the request line plus every header. The worst case this client can
 * produce is a 2 KB signed request target, a 7 KB session cookie and the rest
 * of the block; 8 KB was not enough for that pair, and "请求头过长" would have
 * read like a network fault. */
#define HDR_MAX 16384

/* Room for the request target: the path plus the whole query string.
 *
 * This is not a display buffer. A quark direct link is *signed* - the query
 * carries auth_key/sign (or X-Amz-Signature on the object-storage nodes) - so
 * every byte after the '?' is part of what the far end recomputes. A target
 * that does not fit must therefore be an error, never a shorter request: cut
 * the signature off and Apache answers 412 "precondition failed" while the
 * storage node answers 403 SignatureDoesNotMatch, both of which read like a
 * credentials problem and are not. */
#define URL_MAX 2048
#define PATH_MAX_ 2048

/* Scratch buffers, one set per thread.
 *
 * Downloads run on their own threads and page requests on libmicrohttpd's, and
 * both reach this function, so a plain static would let one thread rewrite the
 * other's request while it is on the wire. A stack array of this size is what
 * the console could not afford in the first place. A pthread key gives each
 * thread a buffer that lives exactly as long as the thread. */
typedef struct {
  char head[HDR_MAX];
  char url[URL_MAX];
  char path[PATH_MAX_];
  /* The body reader's buffer. It lives here, off the stack, because a large
   * one is the difference between a big file downloading at full speed and
   * crawling: a small buffer forces one read() + one write() system call per
   * fill, which a multi-gigabyte file turns into a million of each. The web
   * workers run on a platform stack that is small and not ours to set, so the
   * reader's buffer cannot be a stack array.
   *
   * Sized to the same 256 KB the working exFAT/Rust build uses for its
   * download chunk: that build reads a 256 KB block and writes it in one
   * write(), which is what keeps a 4 GB file at full speed while a per-record
   * (16 KB) loop crawls. net_read now loops mbedtls_ssl_read until this buffer
   * is full, so one rd_fill hands the sink a whole 256 KB to write at once. */
  unsigned char body[256 * 1024];
} tls_scratch;

static pthread_key_t g_scr_key;
static pthread_once_t g_scr_once = PTHREAD_ONCE_INIT;

static void
scr_release(void *p) {
  free(p);
}

static void
scr_key_init(void) {
  pthread_key_create(&g_scr_key, scr_release);
}

static tls_scratch *
scratch(void) {
  tls_scratch *p;

  pthread_once(&g_scr_once, scr_key_init);
  p = (tls_scratch *)pthread_getspecific(g_scr_key);
  if(!p) {
    p = (tls_scratch *)calloc(1, sizeof(*p));
    if(p) {
      pthread_setspecific(g_scr_key, p);
    }
  }
  return p;
}
#define HLINE_MAX 2048

typedef struct {
  net_conn *c;
  size_t pos;
  size_t len;
  size_t cap;
  unsigned char *buf; /* points into the thread's scratch body[] */
} reader;

static long long
now_ms(void) {
  struct timespec ts;

  if(clock_gettime(CLOCK_MONOTONIC, &ts)) {
    return 0;
  }
  return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static const char *
skip_ws(const char *p) {
  while(*p == ' ' || *p == '\t') {
    p++;
  }
  return p;
}

static void
trim_copy(char *dst, size_t cap, const char *src) {
  size_t n;

  if(!cap) {
    return;
  }
  src = skip_ws(src);
  n = strlen(src);
  while(n && (src[n - 1] == '\r' || src[n - 1] == '\n' || src[n - 1] == ' ' ||
              src[n - 1] == '\t')) {
    n--;
  }
  if(n >= cap) {
    n = cap - 1;
  }
  memcpy(dst, src, n);
  dst[n] = 0;
}

static int
ci_contains(const char *hay, const char *needle) {
  size_t nl = strlen(needle);

  if(!nl) {
    return 1;
  }
  for(; *hay; hay++) {
    if(!strncasecmp(hay, needle, nl)) {
      return 1;
    }
  }
  return 0;
}

static int
app(char *buf, size_t cap, size_t *used, const char *fmt, ...) {
  va_list ap;
  int n;

  if(*used >= cap) {
    return -1;
  }
  va_start(ap, fmt);
  n = vsnprintf(buf + *used, cap - *used, fmt, ap);
  va_end(ap);
  if(n < 0 || (size_t)n >= cap - *used) {
    return -1;
  }
  *used += (size_t)n;
  return 0;
}

int
http_url_parse(const char *url, int *tls, char *host, size_t hostcap,
               unsigned short *port, char *path, size_t pathcap) {
  const char *p;
  const char *auth;
  size_t alen;

  if(!url || !*url) {
    return -1;
  }
  if(!strncasecmp(url, "https://", 8)) {
    if(tls) {
      *tls = 1;
    }
    *port = 443;
    p = url + 8;
  } else if(!strncasecmp(url, "http://", 7)) {
    if(tls) {
      *tls = 0;
    }
    *port = 80;
    p = url + 7;
  } else {
    return -1;
  }

  auth = p;
  alen = strcspn(auth, "/?#");
  if(alen == 0 || alen >= hostcap) {
    return -1;
  }
  memcpy(host, auth, alen);
  host[alen] = 0;

  if(host[0] == '[') {
    char *close = strchr(host, ']');
    size_t n;

    if(!close) {
      return -1;
    }
    n = (size_t)(close - host) - 1;
    memmove(host, host + 1, n);
    host[n] = 0;
    if(close[1] == ':') {
      long v = strtol(close + 2, NULL, 10);

      if(v <= 0 || v > 65535) {
        return -1;
      }
      *port = (unsigned short)v;
    }
  } else {
    char *colon = strrchr(host, ':');

    if(colon) {
      long v;

      *colon = 0;
      v = strtol(colon + 1, NULL, 10);
      if(v <= 0 || v > 65535) {
        return -1;
      }
      *port = (unsigned short)v;
    }
  }
  if(!host[0]) {
    return -1;
  }

  p = auth + alen;
  {
    /* "?" becomes a leading "/", and the NUL has to fit too. Refusing here is
     * the whole point: snprintf would happily leave a half a signature in the
     * buffer and the far end would call that a wrong signature. */
    size_t need = strlen(p) + (p[0] == '?' ? 2u : 1u);

    if(need > pathcap) {
      return -2;
    }
  }
  if(*p == 0) {
    snprintf(path, pathcap, "/");
  } else if(*p == '?') {
    snprintf(path, pathcap, "/%s", p);
  } else {
    snprintf(path, pathcap, "%s", p);
  }
  {
    char *hash = strchr(path, '#');

    if(hash) {
      *hash = 0;
    }
  }
  if(!path[0]) {
    snprintf(path, pathcap, "/");
  }
  return 0;
}

/* ------------------------------------------------------------- reading */

static int
rd_fill(reader *r, char *err, size_t cap) {
  long n;

  if(r->pos < r->len) {
    return 1;
  }
  r->pos = 0;
  r->len = 0;
  n = net_read(r->c, r->buf, r->cap, err, cap);
  if(n < 0) {
    return -1;
  }
  if(n == 0) {
    return 0;
  }
  r->len = (size_t)n;
  return 1;
}

static int
rd_getc(reader *r, char *err, size_t cap) {
  if(r->pos >= r->len) {
    int rc = rd_fill(r, err, cap);

    if(rc <= 0) {
      return rc;
    }
  }
  return (int)r->buf[r->pos++];
}

/* 1 = line read, 0 = clean EOF before any byte, -1 = error. */
static int
rd_line(reader *r, char *out, size_t cap, char *err, size_t ecap) {
  size_t o = 0;
  int c = rd_getc(r, err, ecap);

  if(c <= 0) {
    return c;
  }
  out[0] = 0;
  for(;;) {
    if(c == '\n') {
      break;
    }
    if(c != '\r' && o + 1 < cap) {
      out[o++] = (char)c;
    }
    c = rd_getc(r, err, ecap);
    if(c <= 0) {
      break;
    }
  }
  out[o] = 0;
  return 1;
}

/* want < 0 reads until the peer closes. */
static int
copy_body(reader *r, long long want, http_write_cb cb, void *ctx,
          long long *received, volatile int *abort, char *err, size_t ecap) {
  while(want != 0) {
    size_t avail;
    size_t take;
    int rc;

    if(abort && *abort) {
      snprintf(err, ecap, "已取消");
      return -1;
    }
    if(r->pos >= r->len) {
      rc = rd_fill(r, err, ecap);
      if(rc < 0) {
        return -1;
      }
      if(rc == 0) {
        if(want > 0) {
          snprintf(err, ecap, "连接提前关闭，还差 %lld 字节", want);
          return -1;
        }
        return 0;
      }
    }
    avail = r->len - r->pos;
    take = avail;
    if(want > 0 && (long long)take > want) {
      take = (size_t)want;
    }
    if(cb && cb(ctx, r->buf + r->pos, take) != 0) {
      snprintf(err, ecap, "写入目标文件失败");
      return -1;
    }
    r->pos += take;
    *received += (long long)take;
    if(want > 0) {
      want -= (long long)take;
    }
  }
  return 0;
}

static int
copy_chunked(reader *r, http_write_cb cb, void *ctx, long long *received,
             volatile int *abort, char *err, size_t ecap) {
  for(;;) {
    char line[64];
    long long size;

    if(rd_line(r, line, sizeof(line), err, ecap) <= 0) {
      snprintf(err, ecap, "chunked 数据中断");
      return -1;
    }
    size = strtoll(line, NULL, 16);
    if(size < 0) {
      snprintf(err, ecap, "chunked 长度无效: %.20s", line);
      return -1;
    }
    if(size == 0) {
      return 0;
    }
    if(copy_body(r, size, cb, ctx, received, abort, err, ecap) < 0) {
      return -1;
    }
    if(rd_line(r, line, sizeof(line), err, ecap) < 0) {
      snprintf(err, ecap, "chunked 结尾缺失");
      return -1;
    }
  }
}

static int
next_url(char *out, size_t cap, const char *base, const char *location) {
  if(!strncasecmp(location, "http://", 7) ||
     !strncasecmp(location, "https://", 8)) {
    if(strlen(location) >= cap) {
      return -1;
    }
    snprintf(out, cap, "%s", location);
    return 0;
  }
  if(location[0] == '/') {
    if(strlen(base) + strlen(location) >= cap) {
      return -1;
    }
    snprintf(out, cap, "%s%s", base, location);
    return 0;
  }
  if(strlen(base) + strlen(location) + 2 >= cap) {
    return -1;
  }
  snprintf(out, cap, "%s/%s", base, location);
  return 0;
}

int
http_perform(const http_req *req, http_result *out) {
  tls_scratch *sc = scratch();
  const char *method = (req->method && *req->method) ? req->method : "GET";
  int max_redirects = req->max_redirects > 0 ? req->max_redirects : 5;
  int timeout = req->timeout_ms > 0 ? req->timeout_ms : 15000;
  long long t_start = now_ms();
  int redirects = 0;

  memset(out, 0, sizeof(*out));
  out->length = -1;
  out->total = -1;
  if(!sc) {
    snprintf(out->err, sizeof(out->err), "内存不足");
    return -1;
  }
  if(!req->url || !*req->url) {
    snprintf(out->err, sizeof(out->err), "URL 为空");
    return -1;
  }
  if(strlen(req->url) >= sizeof(sc->url)) {
    snprintf(out->err, sizeof(out->err), "URL 过长：%u 字节，上限 %u",
             (unsigned)strlen(req->url), (unsigned)sizeof(sc->url) - 1);
    return -1;
  }
  snprintf(sc->url, sizeof(sc->url), "%s", req->url);

  for(;;) {
    int tls = 0;
    unsigned short port = 0;
    char host[200];
    char base[320];
    char hosthdr[240];
    char *url = sc->url;
    char *path = sc->path;
    int prc;
    /* See scratch(): off the stack, and private to this thread. */
    char *head = sc->head;
    char line[HLINE_MAX];
    size_t used = 0;
    net_conn *c;
    net_peer peer;
    reader r;
    int chunked = 0;
    int no_body;
    int rc;
    int guard;

    prc = http_url_parse(url, &tls, host, sizeof(host), &port, path,
                         PATH_MAX_);
    if(prc == -2) {
      snprintf(out->err, sizeof(out->err),
               "URL 太长：路径与参数 %u 字节，上限 %u", (unsigned)strlen(url),
               (unsigned)PATH_MAX_ - 1);
      out->total_ms = now_ms() - t_start;
      return -1;
    }
    if(prc) {
      snprintf(out->err, sizeof(out->err), "URL 无法解析: %.120s", url);
      out->total_ms = now_ms() - t_start;
      return -1;
    }
    if((tls && port == 443) || (!tls && port == 80)) {
      snprintf(hosthdr, sizeof(hosthdr), "%s", host);
    } else {
      snprintf(hosthdr, sizeof(hosthdr), "%s:%u", host, (unsigned)port);
    }
    snprintf(base, sizeof(base), "%s://%s", tls ? "https" : "http", hosthdr);
    snprintf(out->final_url, sizeof(out->final_url), "%s", url);

    out->status = 0;
    out->length = -1;
    out->total = -1;
    out->received = 0;
    out->chunked = 0;
    out->location[0] = 0;
    out->ctype[0] = 0;
    out->err[0] = 0;

    memset(&peer, 0, sizeof(peer));
    {
      long long t_conn = now_ms();

      c = net_dial(host, port, tls, timeout, out->err, sizeof(out->err), &peer);
      out->connect_ms = now_ms() - t_conn;
    }
    /* Copied before the failure check: when a certificate is refused the peer
     * is the only record of what answered, and that is what makes the report
     * actionable. */
    snprintf(out->tls_info, sizeof(out->tls_info), "%s", peer.info);
    snprintf(out->tls_subject, sizeof(out->tls_subject), "%s", peer.subject);
    snprintf(out->tls_issuer, sizeof(out->tls_issuer), "%s", peer.issuer);
    snprintf(out->tls_expires, sizeof(out->tls_expires), "%s", peer.expires);
    if(!c) {
      out->total_ms = now_ms() - t_start;
      return -1;
    }

    rc = app(head, HDR_MAX, &used, "%s %s HTTP/1.1\r\n", method, path);
    if(!rc) {
      rc = app(head, HDR_MAX, &used, "Host: %s\r\n", hosthdr);
    }
    if(!rc && req->user_agent && *req->user_agent) {
      rc = app(head, HDR_MAX, &used, "User-Agent: %s\r\n", req->user_agent);
    } else if(!rc) {
      rc = app(head, HDR_MAX, &used, "User-Agent: %s/%s\r\n", APP_NAME,
               APP_VERSION);
    }
    if(!rc) {
      rc = app(head, HDR_MAX, &used, "Accept: */*\r\n");
    }
    if(!rc) {
      rc = app(head, HDR_MAX, &used, "Accept-Encoding: identity\r\n");
    }
    if(!rc) {
      rc = app(head, HDR_MAX, &used, "Connection: close\r\n");
    }
    if(!rc && req->range_from > 0) {
      rc = app(head, HDR_MAX, &used, "Range: bytes=%lld-\r\n",
               req->range_from);
    }
    if(!rc && req->body_len > 0) {
      rc = app(head, HDR_MAX, &used, "Content-Length: %lu\r\n",
               (unsigned long)req->body_len);
    }
    if(!rc && req->extra_headers && *req->extra_headers) {
      rc = app(head, HDR_MAX, &used, "%s", req->extra_headers);
    }
    if(!rc) {
      rc = app(head, HDR_MAX, &used, "\r\n");
    }
    if(rc) {
      net_close(c);
      snprintf(out->err, sizeof(out->err), "请求头过长");
      out->total_ms = now_ms() - t_start;
      return -1;
    }

    if(net_write_all(c, head, used, out->err, sizeof(out->err)) < 0) {
      net_close(c);
      out->total_ms = now_ms() - t_start;
      return -1;
    }
    if(req->body_len > 0 &&
       net_write_all(c, req->body, req->body_len, out->err, sizeof(out->err)) <
         0) {
      net_close(c);
      out->total_ms = now_ms() - t_start;
      return -1;
    }

    memset(&r, 0, sizeof(r));
    r.c = c;
    r.cap = sizeof(sc->body);
    r.buf = sc->body;
    rc = rd_line(&r, line, sizeof(line), out->err, sizeof(out->err));
    if(rc <= 0) {
      net_close(c);
      if(rc == 0) {
        snprintf(out->err, sizeof(out->err), "服务器没有返回任何数据");
      }
      out->total_ms = now_ms() - t_start;
      return -1;
    }
    if(sscanf(line, "HTTP/%*u.%*u %d", &out->status) != 1) {
      net_close(c);
      snprintf(out->err, sizeof(out->err), "不是 HTTP 响应: %.80s", line);
      out->total_ms = now_ms() - t_start;
      return -1;
    }

    for(guard = 0; guard < 150; guard++) {
      rc = rd_line(&r, line, sizeof(line), out->err, sizeof(out->err));
      if(rc <= 0 || !line[0]) {
        break;
      }
      if(!strncasecmp(line, "Content-Length:", 15)) {
        out->length = strtoll(line + 15, NULL, 10);
      } else if(!strncasecmp(line, "Content-Range:", 14)) {
        /* "bytes 100-999/1000": the number after the slash is the size of the
         * whole file, which is what a resume needs. Reading `length` bytes from
         * here would wait for the entire file every time. */
        const char *slash = strrchr(line, '/');

        if(slash && slash[1]) {
          long long total = strtoll(slash + 1, NULL, 10);

          if(total > 0) {
            out->total = total;
          }
        }
      } else if(!strncasecmp(line, "Content-Type:", 13)) {
        trim_copy(out->ctype, sizeof(out->ctype), line + 13);
      } else if(!strncasecmp(line, "Location:", 9)) {
        trim_copy(out->location, sizeof(out->location), line + 9);
      } else if(!strncasecmp(line, "Set-Cookie:", 11)) {
        cookie_jar_put(out->set_cookie, sizeof(out->set_cookie),
                       line + 11);
      } else if(!strncasecmp(line, "Transfer-Encoding:", 18)) {
        if(ci_contains(line + 18, "chunked")) {
          chunked = 1;
        }
      }
    }
    out->chunked = chunked;

    /* Redirects are followed without downloading the body: the interesting
     * case is a download URL that points at a CDN host. */
    if(out->status >= 300 && out->status < 400 && out->location[0] &&
       redirects < max_redirects) {
      net_close(c);
      /* URL_MAX, not sizeof(url): url is a pointer into the scratch set now,
       * and sizeof would be 8 - the whole redirect would be dropped on the
       * floor with "URL too long" instead. */
      if(next_url(url, URL_MAX, base, out->location) == 0) {
        redirects++;
        continue;
      }
      out->redirects = redirects;
      out->total_ms = now_ms() - t_start;
      return 0;
    }

    no_body = !strcmp(method, "HEAD") || out->status == 204 ||
              out->status == 304 || (out->status >= 100 && out->status < 200);
    if(!no_body) {
      if(chunked) {
        rc = copy_chunked(&r, req->write, req->write_ctx, &out->received,
                          req->abort, out->err, sizeof(out->err));
      } else {
        rc = copy_body(&r, out->length, req->write, req->write_ctx,
                       &out->received, req->abort, out->err, sizeof(out->err));
      }
    } else {
      rc = 0;
    }
    net_close(c);
    out->redirects = redirects;
    out->total_ms = now_ms() - t_start;
    return rc < 0 ? -1 : 0;
  }
}
