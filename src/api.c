/* Minimal API surface for the skeleton build.
 *
 * Stage 1 has one job: prove the payload loads, carries the right process name
 * and can be stopped on purpose. Everything the console cannot show (kernel
 * thread name, instance count, pid, build) is exposed here so a plain browser
 * request is enough evidence.
 *
 * Two things learned from the first console run:
 *
 *  - The page worked while typing /api/diag into a browser did not. Same
 *    handler, so the difference is presentation: "application/json" makes the
 *    console and phone browsers offer a download or render nothing at all.
 *    Every diagnostic endpoint therefore answers text/html to browsers that say
 *    they accept it, and JSON to everything else (curl, the page itself).
 *  - Routing compared the raw request target with strcmp, so "/api/diag?x",
 *    "/api/diag/" and "/api/diag " all fell through to 404. The path is now
 *    normalised (query, fragment and trailing slashes removed) before matching.
 */

#include "api.h"

#include "boot.h"

#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "build.h"
#include "dl.h"
#include "httpc.h"
#include "kern.h"
#include "net.h"
#include "notify.h"
#include "quark.h"
#include "websrv.h"

#define PROCESS_NAME "quark-dl.elf"

#define API_JSON "application/json; charset=utf-8"
#define API_HTML "text/html; charset=utf-8"
#define API_TEXT "text/plain; charset=utf-8"

static struct timespec g_started;
static unsigned short g_port;
static volatile int g_exiting;
static unsigned g_requests;
static char g_last_request[80];
static char g_host[72];

static long
elapsed_ms(void) {
  struct timespec now;

  if(clock_gettime(CLOCK_MONOTONIC, &now)) {
    return 0;
  }
  return (now.tv_sec - g_started.tv_sec) * 1000L +
         (now.tv_nsec - g_started.tv_nsec) / 1000000L;
}

static void
json_escape(char *dst, size_t cap, const char *src) {
  size_t o = 0;

  if(!cap) {
    return;
  }
  for(; src && *src && o + 2 < cap; src++) {
    unsigned char c = (unsigned char)*src;
    if(c == '"' || c == '\\') {
      dst[o++] = '\\';
      dst[o++] = (char)c;
    } else if(c < 0x20) {
      dst[o++] = ' ';
    } else {
      dst[o++] = (char)c;
    }
  }
  dst[o] = 0;
}

/* The kernel name and the Host header are the only values from outside this
 * file, and both end up inside HTML. */
static void
html_escape(char *dst, size_t cap, const char *src) {
  size_t o = 0;

  if(!cap) {
    return;
  }
  for(; src && *src && o + 7 < cap; src++) {
    switch(*src) {
      case '<': memcpy(dst + o, "&lt;", 4); o += 4; break;
      case '>': memcpy(dst + o, "&gt;", 4); o += 4; break;
      case '&': memcpy(dst + o, "&amp;", 5); o += 5; break;
      case '"': memcpy(dst + o, "&quot;", 6); o += 6; break;
      default: dst[o++] = *src; break;
    }
  }
  dst[o] = 0;
}

static enum MHD_Result
send_body(struct MHD_Connection *conn, unsigned int status, const char *ctype,
          const char *body) {
  struct MHD_Response *resp;
  enum MHD_Result ret;

  resp = MHD_create_response_from_buffer(strlen(body), (void *)body,
                                         MHD_RESPMEM_MUST_COPY);
  if(!resp) {
    return MHD_NO;
  }
  MHD_add_response_header(resp, MHD_HTTP_HEADER_CONTENT_TYPE, ctype);
  /* Every one of these answers describes the console right now: how many
   * downloads are running, which build is loaded, whether the login is still
   * good. A cached copy is never merely stale, it is a lie. */
  MHD_add_response_header(resp, MHD_HTTP_HEADER_CACHE_CONTROL, "no-store");
  ret = websrv_queue_response(conn, status, resp);
  MHD_destroy_response(resp);
  return ret;
}

static enum MHD_Result
send_json(struct MHD_Connection *conn, unsigned int status, const char *body) {
  return send_body(conn, status, API_JSON, body);
}

/* Up to the first '?' or '#', then without trailing slashes. "/api/diag?x=1",
 * "/api/diag/" and "/api/diag" are the same endpoint. */
static void
normalize_path(char *dst, size_t cap, const char *url) {
  size_t o = 0;

  if(!cap) {
    return;
  }
  for(; url && *url && o + 1 < cap; url++) {
    if(*url == '?' || *url == '#') {
      break;
    }
    dst[o++] = *url;
  }
  while(o > 1 && dst[o - 1] == '/') {
    o--;
  }
  if(!o) {
    dst[o++] = '/';
  }
  dst[o] = 0;
}

/* Browsers advertise text/html; curl does not. The page's own requests are
 * XHR, which sends a wildcard Accept header, so the JSON path stays the default
 * for scripts. Never write a wildcard MIME type inside a comment: the slash-star
 * in it closes the comment, which is how this file first failed to compile. */
static int
wants_html(struct MHD_Connection *conn) {
  const char *accept =
    MHD_lookup_connection_value(conn, MHD_HEADER_KIND, "Accept");
  const char *format =
    MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "format");

  if(format && (!strcmp(format, "html") || !strcmp(format, "htm"))) {
    return 1;
  }
  if(format && (!strcmp(format, "json") || !strcmp(format, "text"))) {
    return 0;
  }
  return accept && strstr(accept, "text/html") ? 1 : 0;
}

static void
*stop_later(void *arg) {
  (void)arg;
  /* Let the response reach the browser before accept() stops returning. */
  usleep(250000);
  websrv_stop();
  return 0;
}

/* ------------------------------------------------------------------ diag --- */

typedef struct diag_view {
  char build[64];
  char kernel[256];
  char host[72];
  int named;
  int instances;
  int pid;
  int ppid;
  unsigned port;
  long uptime;
  unsigned requests;
  char last[80];
} diag_view_t;

static void
collect_diag(diag_view_t *d) {
  memset(d, 0, sizeof(*d));
  d->named = qdl_kernel_name(d->kernel, sizeof(d->kernel)) == 0;
  if(!d->named) {
    snprintf(d->kernel, sizeof(d->kernel), "unknown");
  }
  d->instances = qdl_count_named(PROCESS_NAME);
  d->pid = qdl_self_pid();
  d->ppid = qdl_parent_pid();
  d->port = (unsigned)g_port;
  d->uptime = elapsed_ms();
  d->requests = g_requests;
  snprintf(d->build, sizeof(d->build), "%s", BUILD_ID);
  snprintf(d->host, sizeof(d->host), "%s", g_host[0] ? g_host : "unknown");
  snprintf(d->last, sizeof(d->last), "%s", g_last_request);
}

static enum MHD_Result
api_diag_json(struct MHD_Connection *conn) {
  char kernel[384];
  char host[160];
  char last[160];
  char tls[420];
  char qdir[700];
  char body[3072];
  diag_view_t d;

  collect_diag(&d);
  json_escape(kernel, sizeof(kernel), d.kernel);
  json_escape(host, sizeof(host), d.host);
  json_escape(last, sizeof(last), d.last);
  json_escape(tls, sizeof(tls), net_status());
  json_escape(qdir, sizeof(qdir), dl_default_dir());

  snprintf(body, sizeof(body),
           "{\"app\":\"%s\",\"version\":\"%s\",\"build\":\"%s\","
           "\"pid\":%d,\"ppid\":%d,\"port\":%u,\"uptime_ms\":%ld,"
           "\"name\":{\"expected\":\"%s\",\"kernel\":\"%s\",\"readable\":%s,"
           "\"ok\":%s,\"instances\":%d},"
           "\"host\":\"%s\",\"requests\":%u,\"last_request\":\"%s\","
           "\"exiting\":%s,"
           "\"net\":{\"tls\":\"%s\",\"resolver\":\"%s\"},"
           "\"dl\":{\"running\":%d,\"tasks\":%d,\"dir\":\"%s\"}}",
           APP_NAME, APP_VERSION, d.build,
           d.pid, d.ppid, d.port, d.uptime,
           PROCESS_NAME, kernel,
           d.named ? "true" : "false",
           (d.named && !strcmp(d.kernel, PROCESS_NAME)) ? "true" : "false",
           d.instances,
           host, d.requests, last,
           g_exiting ? "true" : "false",
           tls, net_resolver(),
           dl_running(), dl_count(), qdir);
  return send_json(conn, MHD_HTTP_OK, body);
}

static enum MHD_Result
api_diag_text(struct MHD_Connection *conn) {
  char body[1024];
  diag_view_t d;

  collect_diag(&d);
  snprintf(body, sizeof(body),
           "app       %s %s\n"
           "build     %s\n"
           "pid       %d (parent %d)\n"
           "port      %u\n"
           "host      %s\n"
           "name      %s  (expected %s)\n"
           "instances %d\n"
           "uptime    %ld ms\n"
           "requests  %u, last %s\n"
           "exiting   %s\n"
           "tls       %s\n"
           "resolver  %s\n"
           "downloads %d running, %d known -> %s\n",
           APP_NAME, APP_VERSION,
           d.build,
           d.pid, d.ppid,
           d.port,
           d.host,
           d.kernel, PROCESS_NAME,
           d.instances,
           d.uptime,
           d.requests, d.last[0] ? d.last : "-",
           g_exiting ? "yes" : "no",
           net_status(), net_resolver(),
           dl_running(), dl_count(), dl_default_dir());
  return send_body(conn, MHD_HTTP_OK, API_TEXT, body);
}

static enum MHD_Result
api_diag_html(struct MHD_Connection *conn) {
  char kernel[512];
  char host[160];
  char tls[600];
  char qdir[900];
  char body[6144];
  diag_view_t d;

  collect_diag(&d);
  html_escape(kernel, sizeof(kernel), d.kernel);
  html_escape(host, sizeof(host), d.host);
  html_escape(tls, sizeof(tls), net_status());
  html_escape(qdir, sizeof(qdir), dl_default_dir());

  snprintf(body, sizeof(body),
           "<!doctype html><html lang=\"zh-CN\"><head><meta charset=\"utf-8\">"
           "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
           "<title>Quark DL 诊断</title><style>"
           "body{margin:0;padding:28px 18px;background:#0f1115;color:#e8ecf1;"
           "font:15px/1.6 -apple-system,'Segoe UI','PingFang SC',sans-serif}"
           ".w{max-width:640px;margin:0 auto}h1{font-size:24px;margin:0 0 4px}"
           "p.s{color:#8b97a5;font-size:13px;margin:0 0 22px}"
           "table{width:100%%;border-collapse:collapse;background:#161a20;"
           "border:1px solid #232a33;border-radius:12px;overflow:hidden}"
           "td{padding:10px 16px;border-bottom:1px solid #232a33;font-size:14px}"
           "tr:last-child td{border-bottom:0}"
           "td.k{color:#8b97a5;width:44%%}"
           "td.v{font-family:ui-monospace,Menlo,Consolas,monospace;text-align:right;"
           "word-break:break-all}"
           ".ok{color:#4ade80}.bad{color:#f87171}"
           "a{color:#7aa2ff;text-decoration:none;margin-right:18px}"
           ".f{margin-top:18px;font-size:13px}"
           "</style></head><body><div class=\"w\">"
           "<h1>Quark DL 诊断</h1>"
           "<p class=\"s\">这个页面就是 <code>/api/diag</code> 的内容，"
           "给浏览器看的是网页版。脚本仍然拿 JSON。</p><table>"
           "<tr><td class=\"k\">进程名（进程列表显示）</td><td class=\"v %s\">%s</td></tr>"
           "<tr><td class=\"k\">期望的名字</td><td class=\"v\">%s</td></tr>"
           "<tr><td class=\"k\">PID / 父进程</td><td class=\"v\">%d / %d</td></tr>"
           "<tr><td class=\"k\">端口</td><td class=\"v\">%u</td></tr>"
           "<tr><td class=\"k\">你连的地址（Host）</td><td class=\"v\">%s</td></tr>"
           "<tr><td class=\"k\">构建</td><td class=\"v\">%s</td></tr>"
           "<tr><td class=\"k\">已运行</td><td class=\"v\">%ld 秒</td></tr>"
           "<tr><td class=\"k\">同名实例</td><td class=\"v\">%d</td></tr>"
           "<tr><td class=\"k\">已处理请求</td><td class=\"v\">%u</td></tr>"
           "<tr><td class=\"k\">TLS</td><td class=\"v\">%s</td></tr>"
           "<tr><td class=\"k\">下载队列</td><td class=\"v\">%d 个进行中 / %d 个任务</td></tr>"
           "<tr><td class=\"k\">默认保存到</td><td class=\"v\">%s</td></tr>"
           "</table><p class=\"f\">"
           "<a href=\"/api/net/selftest\">网络自检</a>"
           "<a href=\"/api/diag?format=json\">看 JSON 原始输出</a>"
           "<a href=\"/\">回首页</a>"
           "<a href=\"/api/health\">health</a></p></div></body></html>",
           (d.named && !strcmp(d.kernel, PROCESS_NAME)) ? "ok" : "bad",
           kernel, PROCESS_NAME,
           d.pid, d.ppid,
           d.port,
           host,
           d.build,
           d.uptime / 1000,
           d.instances,
           d.requests,
           tls,
           dl_running(), dl_count(),
           qdir);
  return send_body(conn, MHD_HTTP_OK, API_HTML, body);
}

/* ----------------------------------------------------------------- health -- */

static enum MHD_Result
api_health(struct MHD_Connection *conn) {
  char body[640];

  snprintf(body, sizeof(body),
           "{\"ok\":true,\"app\":\"%s\",\"version\":\"%s\",\"build\":\"%s\","
           "\"pid\":%d,\"port\":%u,\"uptime_ms\":%ld,\"name\":\"%s\","
           "\"host\":\"%s\"}",
           APP_NAME, APP_VERSION, BUILD_ID, qdl_self_pid(), (unsigned)g_port,
           elapsed_ms(), PROCESS_NAME, g_host[0] ? g_host : "unknown");
  return send_json(conn, MHD_HTTP_OK, body);
}

/* ------------------------------------------------------- stage 2: network -- */

/* Network self-test. This exists because the one question the console cannot
 * answer on its own is "can this payload reach the internet at all": DNS,
 * TCP, TLS, certificate and HTTP are five different failures that all look
 * like "下载失败" from the queue. Naming the stage is the difference between a
 * fixable report and another round of guessing. */
static enum MHD_Result
api_net_selftest(struct MHD_Connection *conn) {
  const char *arg = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "url");
  char url[600];
  char host[200];
  char path[400];
  char ip[64];
  char json[2800];
  char page[5800];
  char escaped[2900];
  char eurl[520];
  char ehost[220];
  char etls[140];
  char ecert[260];
  char eissuer[260];
  char eerr[420];
  unsigned short port = 0;
  int tls = 0;
  long long t_dns = 0;
  long long t_http = 0;
  int dns_ok;
  int http_ok = 0;
  static __thread http_result res;
  http_req req;

  /* The default has to be a host that actually serves a valid certificate, or
   * the first click is a red cross on a machine that is perfectly fine.
   * api.quark.cn was the obvious-looking choice and the wrong one: it resolves,
   * but its ingress presents the "Kubernetes Ingress Controller Fake
   * Certificate" and nothing else, so every health check started with a
   * certificate failure that said nothing about this console. pan.quark.cn is
   * on the *.quark.cn wildcard and is the host the next stage will talk to. */
  snprintf(url, sizeof(url), "%s",
           (arg && *arg) ? arg : "https://pan.quark.cn/");
  /* Escaped once, used by every answer below. The URL is whatever the user
   * typed and the host is whatever it parsed to; either can carry a quote. */
  json_escape(eurl, sizeof(eurl), url);
  memset(&res, 0, sizeof(res));

  if(http_url_parse(url, &tls, host, sizeof(host), &port, path, sizeof(path))) {
    snprintf(json, sizeof(json),
             "{\"ok\":false,\"url\":\"%s\",\"error\":\"URL 无法解析\"}", eurl);
    return send_json(conn, MHD_HTTP_BAD_REQUEST, json);
  }
  json_escape(ehost, sizeof(ehost), host);

  {
    long long mark = elapsed_ms();
    dns_ok = net_resolve(host, ip, sizeof(ip), NULL) == 0;
    t_dns = elapsed_ms() - mark;
  }
  if(!dns_ok) {
    snprintf(json, sizeof(json),
             "{\"ok\":false,\"url\":\"%s\",\"stage\":\"dns\",\"host\":\"%s\","
             "\"error\":\"域名解析失败\",\"resolver\":\"%s\",\"dns_ms\":%lld}",
             eurl, ehost, net_resolver(), t_dns);
    return send_json(conn, MHD_HTTP_OK, json);
  }

  memset(&req, 0, sizeof(req));
  req.url = url;
  req.method = "GET";
  req.timeout_ms = 12000;
  req.max_redirects = 3;
  {
    long long mark = elapsed_ms();
    http_ok = http_perform(&req, &res) == 0;
    t_http = elapsed_ms() - mark;
  }

  /* The certificate subject comes off the wire and the failure text is whatever
   * mbedTLS decided. Unescaped, either one turns this answer into invalid JSON,
   * which the page then reports as "not JSON" — the exact symptom that hides
   * the real network error. */
  json_escape(etls, sizeof(etls), res.tls_info);
  json_escape(ecert, sizeof(ecert), res.tls_subject);
  json_escape(eissuer, sizeof(eissuer), res.tls_issuer);
  json_escape(eerr, sizeof(eerr), res.err[0] ? res.err : "");

  snprintf(json, sizeof(json),
           "{\"ok\":%s,\"url\":\"%s\",\"host\":\"%s\",\"ip\":\"%s\","
           "\"resolver\":\"%s\",\"dns_ms\":%lld,\"tls\":\"%s\",\"cert\":\"%s\","
           "\"cert_issuer\":\"%s\",\"cert_expires\":\"%s\",\"status\":%d,"
           "\"bytes\":%lld,\"connect_ms\":%lld,\"total_ms\":%lld,"
           "\"stage\":\"%s\",\"error\":\"%s\"}",
           http_ok ? "true" : "false", eurl, ehost, ip, net_resolver(), t_dns,
           etls, ecert, eissuer, res.tls_expires, res.status, res.received,
           res.connect_ms, t_http,
           http_ok ? "done" : (res.status ? "http" : "connect"), eerr);

  if(!wants_html(conn)) {
    return send_json(conn, MHD_HTTP_OK, json);
  }
  html_escape(escaped, sizeof(escaped), json);
  snprintf(page, sizeof(page),
           "<!doctype html><html lang=\"zh-CN\"><head><meta charset=\"utf-8\">"
           "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
           "<title>Quark DL 网络自检</title><style>"
           "body{margin:0;padding:24px 18px;background:#0f1115;color:#e8ecf1;"
           "font:15px/1.6 -apple-system,'Segoe UI','PingFang SC',sans-serif}"
           "pre{white-space:pre-wrap;word-break:break-all;background:#161a20;"
           "border:1px solid #232a33;border-radius:12px;padding:16px;font-size:13px}"
           "a{color:#7aa2ff}</style></head><body>"
           "<h1 style=\"font-size:20px;margin:0 0 12px\">网络自检</h1>"
           "<pre>%s</pre><p><a href=\"/\">回首页</a></p></body></html>",
           escaped);
  return send_body(conn, MHD_HTTP_OK, API_HTML, page);
}

/* ---------------------------------------------------- stage 2: form input -- */

static int
hexval(char c) {
  if(c >= '0' && c <= '9') {
    return c - '0';
  }
  if(c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if(c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

static void
url_decode(char *dst, size_t cap, const char *src, size_t len) {
  size_t o = 0;
  size_t i;

  for(i = 0; i < len && o + 1 < cap; i++) {
    if(src[i] == '+') {
      dst[o++] = ' ';
    } else if(src[i] == '%' && i + 2 < len) {
      int hi = hexval(src[i + 1]);
      int lo = hexval(src[i + 2]);

      if(hi >= 0 && lo >= 0) {
        dst[o++] = (char)((hi << 4) | lo);
        i += 2;
      } else {
        dst[o++] = src[i];
      }
    } else {
      dst[o++] = src[i];
    }
  }
  dst[o] = 0;
}

/* Browser forms are application/x-www-form-urlencoded, and MHD's own POST
 * processor is not in the picture because the body was consumed by websrv. */
static int
form_field(const char *body, size_t len, const char *key, char *out,
           size_t cap) {
  size_t klen = strlen(key);
  size_t i = 0;

  out[0] = 0;
  if(!body) {
    return 0;
  }
  while(i < len) {
    size_t start = i;
    size_t end;

    while(i < len && body[i] != '&') {
      i++;
    }
    end = i;
    if(i < len) {
      i++;
    }
    if(end > start + klen && !strncmp(body + start, key, klen) &&
       body[start + klen] == '=') {
      url_decode(out, cap, body + start + klen + 1,
                 end - start - klen - 1);
      return 1;
    }
  }
  return 0;
}

/* One "url" field may carry a whole list, one per line: the textarea on the
 * page is the natural place to paste several links at once. */
static int
form_urls(const char *body, size_t len, char out[][600], int max) {
  size_t i = 0;
  int n = 0;

  while(i < len && n < max) {
    size_t start = i;
    size_t end;

    while(i < len && body[i] != '&') {
      i++;
    }
    end = i;
    if(i < len) {
      i++;
    }
    if(end > start + 4 && !strncmp(body + start, "url=", 4)) {
      char tmp[1600];
      char *p = tmp;

      url_decode(tmp, sizeof(tmp), body + start + 4, end - start - 4);
      while(*p && n < max) {
        char *nl = strpbrk(p, "\r\n");

        if(nl) {
          *nl = 0;
        }
        while(*p == ' ' || *p == '\t') {
          p++;
        }
        if(*p) {
          snprintf(out[n], 600, "%s", p);
          n++;
        }
        if(!nl) {
          break;
        }
        p = nl + 1;
      }
    }
  }
  return n;
}

/* ------------------------------------------------------- stage 2: the queue */

static enum MHD_Result
api_dl_list(struct MHD_Connection *conn) {
  char *body = malloc(96 * 1024);

  if(!body) {
    return send_json(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                     "{\"ok\":false,\"error\":\"内存不足\"}");
  }
  if(dl_json(body, 96 * 1024) != 0) {
    free(body);
    return send_json(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                     "{\"ok\":false,\"error\":\"队列过大\"}");
  }
  {
    enum MHD_Result ret = send_json(conn, MHD_HTTP_OK, body);

    free(body);
    return ret;
  }
}

static enum MHD_Result
api_dl_add(struct MHD_Connection *conn, const char *body, size_t size) {
  char urls[20][600];
  char name[200];
  char dir[340];
  char ids[400];
  char errors[1200];
  char msg[200];
  char reply[2200];
  size_t idlen = 0;
  size_t errlen = 0;
  int added = 0;
  int n;
  int i;

  form_field(body, size, "name", name, sizeof(name));
  form_field(body, size, "dir", dir, sizeof(dir));
  n = form_urls(body, size, urls, 20);
  if(!n) {
    return send_json(conn, MHD_HTTP_BAD_REQUEST,
                     "{\"ok\":false,\"error\":\"表单里没有 url 字段\"}");
  }

  ids[0] = 0;
  errors[0] = 0;
  for(i = 0; i < n; i++) {
    int id = dl_add(urls[i], name[0] ? name : NULL, dir[0] ? dir : NULL, msg,
                    sizeof(msg));

    if(id < 0) {
      char safe[400];

      json_escape(safe, sizeof(safe), msg);
      if(errlen + 400 < sizeof(errors)) {
        errlen += (size_t)snprintf(errors + errlen, sizeof(errors) - errlen,
                                   "%s{\"url\":\"%.160s\",\"error\":\"%s\"}",
                                   errlen ? "," : "", urls[i], safe);
      }
    } else {
      added++;
      if(idlen + 24 < sizeof(ids)) {
        idlen += (size_t)snprintf(ids + idlen, sizeof(ids) - idlen, "%s%d",
                                  idlen ? "," : "", id);
      }
    }
  }
  snprintf(reply, sizeof(reply),
           "{\"ok\":%s,\"added\":%d,\"ids\":[%s],\"errors\":[%s]}",
           added ? "true" : "false", added, ids, errors);
  return send_json(conn, added ? MHD_HTTP_OK : MHD_HTTP_BAD_REQUEST, reply);
}

static enum MHD_Result
api_dl_cancel(struct MHD_Connection *conn, const char *method) {
  const char *arg = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "id");
  char body[160];
  int n;

  if(strcmp(method, MHD_HTTP_METHOD_POST)) {
    return send_json(conn, MHD_HTTP_METHOD_NOT_ALLOWED,
                     "{\"ok\":false,\"error\":\"use POST\"}");
  }
  n = (arg && !strcmp(arg, "all")) ? dl_cancel(-1)
                                   : dl_cancel(arg ? atoi(arg) : -1);
  snprintf(body, sizeof(body), "{\"ok\":true,\"canceled\":%d}", n);
  return send_json(conn, MHD_HTTP_OK, body);
}

static enum MHD_Result
api_dl_retry(struct MHD_Connection *conn, const char *method) {
  const char *arg = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "id");
  char body[160];
  int n;

  if(strcmp(method, MHD_HTTP_METHOD_POST)) {
    return send_json(conn, MHD_HTTP_METHOD_NOT_ALLOWED,
                     "{\"ok\":false,\"error\":\"use POST\"}");
  }
  n = dl_retry(arg ? atoi(arg) : -1);
  snprintf(body, sizeof(body), "{\"ok\":true,\"queued\":%d}", n);
  return send_json(conn, MHD_HTTP_OK, body);
}

static enum MHD_Result
api_dl_clear(struct MHD_Connection *conn, const char *method) {
  char body[120];

  if(strcmp(method, MHD_HTTP_METHOD_POST)) {
    return send_json(conn, MHD_HTTP_METHOD_NOT_ALLOWED,
                     "{\"ok\":false,\"error\":\"use POST\"}");
  }
  snprintf(body, sizeof(body), "{\"ok\":true,\"cleared\":%d}",
           dl_clear_finished());
  return send_json(conn, MHD_HTTP_OK, body);
}

static enum MHD_Result
api_settings(struct MHD_Connection *conn, const char *body, size_t size,
             const char *method) {
  char dir[340];
  char reply[700];
  char safe[400];
  char err[220];

  if(!strcmp(method, MHD_HTTP_METHOD_POST)) {
    if(!form_field(body, size, "dir", dir, sizeof(dir)) || !dir[0]) {
      return send_json(conn, MHD_HTTP_BAD_REQUEST,
                       "{\"ok\":false,\"error\":\"缺少 dir 字段\"}");
    }
    if(dl_set_default_dir(dir, err, sizeof(err))) {
      json_escape(safe, sizeof(safe), err);
      snprintf(reply, sizeof(reply), "{\"ok\":false,\"error\":\"%s\"}", safe);
      return send_json(conn, MHD_HTTP_BAD_REQUEST, reply);
    }
  }
  json_escape(safe, sizeof(safe), dl_default_dir());
  snprintf(reply, sizeof(reply),
           "{\"ok\":true,\"dir\":\"%s\",\"limit\":%d,\"tasks\":%d,\"running\":%d}",
           safe, DL_LIMIT, dl_count(), dl_running());
  return send_json(conn, MHD_HTTP_OK, reply);
}

/* A directory listing, so the page can offer somewhere to save into instead of
 * asking the user to type a path from memory. */
static enum MHD_Result
api_fs_list(struct MHD_Connection *conn) {
  const char *arg = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "path");
  char path[340];
  char body[8192];
  char safe[400];
  size_t used = 0;
  int entries = 0;
  int truncated = 0;
  DIR *dir;
  struct dirent *de;

  snprintf(path, sizeof(path), "%s", (arg && *arg == '/') ? arg : dl_default_dir());
  dir = opendir(path);
  json_escape(safe, sizeof(safe), path);
  if(!dir) {
    snprintf(body, sizeof(body),
             "{\"ok\":false,\"path\":\"%s\",\"error\":\"无法打开目录\"}", safe);
    return send_json(conn, MHD_HTTP_OK, body);
  }
  used += (size_t)snprintf(body + used, sizeof(body) - used,
                           "{\"ok\":true,\"path\":\"%s\",\"entries\":[", safe);
  while((de = readdir(dir)) != NULL) {
    struct stat st;
    char full[600];
    memset(&st, 0, sizeof(st));
    char name[400];
    int is_dir;

    if(!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) {
      continue;
    }
    if(entries >= 200 || used + 400 > sizeof(body)) {
      truncated = 1;
      break;
    }
    snprintf(full, sizeof(full), "%s/%s", path, de->d_name);
    is_dir = (stat(full, &st) == 0) ? S_ISDIR(st.st_mode) : 0;
    json_escape(name, sizeof(name), de->d_name);
    used += (size_t)snprintf(body + used, sizeof(body) - used,
                             "%s{\"name\":\"%s\",\"dir\":%s,\"size\":%lld}",
                             entries ? "," : "", name, is_dir ? "true" : "false",
                             is_dir ? 0LL : (long long)st.st_size);
    entries++;
  }
  closedir(dir);
  used += (size_t)snprintf(body + used, sizeof(body) - used,
                           "],\"count\":%d,\"truncated\":%s}", entries,
                           truncated ? "true" : "false");
  return send_json(conn, MHD_HTTP_OK, body);
}

/* ------------------------------------------------------------------- exit -- */

static enum MHD_Result
api_exit(struct MHD_Connection *conn) {
  pthread_t thread;

  if(g_exiting) {
    return send_json(conn, MHD_HTTP_OK,
                     "{\"ok\":true,\"already\":true}");
  }
  g_exiting = 1;
  if(pthread_create(&thread, 0, stop_later, 0)) {
    g_exiting = 0;
    return send_json(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                     "{\"ok\":false,\"error\":\"cannot stop\"}");
  }
  pthread_detach(thread);

  /* No marker file, no supervisor hook: the process just returns from main.
   * Loading the ELF again starts it again — which is exactly what a payload
   * needs, and what the console UI's "terminate" fails to deliver. */
  return send_json(conn, MHD_HTTP_OK,
                   "{\"ok\":true,\"exit\":\"pending\",\"in_ms\":250}");
}

static enum MHD_Result
api_exit_needs_post(struct MHD_Connection *conn) {
  if(wants_html(conn)) {
    return send_body(conn, MHD_HTTP_METHOD_NOT_ALLOWED, API_HTML,
                     "<!doctype html><meta charset=\"utf-8\">"
                     "<body style=\"background:#0f1115;color:#e8ecf1;"
                     "font:15px/1.6 sans-serif;padding:28px\">"
                     "<h1>需要用 POST</h1>"
                     "<p>/api/exit 只接受 POST。请回首页点「结束运行」。</p>"
                     "<p><a style=\"color:#7aa2ff\" href=\"/\">回首页</a></p>");
  }
  return send_json(conn, MHD_HTTP_METHOD_NOT_ALLOWED,
                   "{\"ok\":false,\"error\":\"use POST\"}");
}

/* -------------------------------------------------------------------- 404 -- */

static enum MHD_Result
api_not_found(struct MHD_Connection *conn, const char *path) {
  char safe[320];
  char quoted[320];
  char body[1600];

  if(wants_html(conn)) {
    html_escape(safe, sizeof(safe), path);
    snprintf(body, sizeof(body),
             "<!doctype html><html lang=\"zh-CN\"><head><meta charset=\"utf-8\">"
             "<title>404</title></head>"
             "<body style=\"background:#0f1115;color:#e8ecf1;"
             "font:15px/1.6 -apple-system,'PingFang SC',sans-serif;padding:28px\">"
             "<h1 style=\"font-size:22px\">没有这个地址</h1>"
             "<p style=\"color:#8b97a5\">请求的路径：<code>%s</code></p>"
             "<p>这个 payload 只有三个接口：</p>"
             "<ul style=\"color:#c9d2dc\">"
             "<li><a style=\"color:#7aa2ff\" href=\"/\">/</a> 控制页</li>"
             "<li><a style=\"color:#7aa2ff\" href=\"/api/diag\">/api/diag</a> 诊断</li>"
             "<li><a style=\"color:#7aa2ff\" href=\"/api/health\">/api/health</a> 存活</li>"
             "</ul></body></html>",
             safe);
    return send_body(conn, MHD_HTTP_NOT_FOUND, API_HTML, body);
  }
  json_escape(quoted, sizeof(quoted), path);
  snprintf(body, sizeof(body),
           "{\"ok\":false,\"error\":\"unknown endpoint\",\"path\":\"%s\","
           "\"endpoints\":[\"/\",\"/api/diag\",\"/api/health\",\"/api/exit (POST)\"]}",
           quoted);
  return send_json(conn, MHD_HTTP_NOT_FOUND, body);
}

/* ------------------------------------------------------------------ entry -- */

/* Where the queue and settings live. The console path matches everything else
 * this payload writes; the host build keeps its state next to the binary so a
 * test run cannot read the console's files by accident. */
static const char *
data_dir(void) {
#ifdef __SCE__
  return "/data/quark-dl";
#else
  const char *env = getenv("QDL_DATA_DIR");

  return (env && *env) ? env : "./.quark-dl-data";
#endif
}

void
api_init(void) {
  char err[220];

  clock_gettime(CLOCK_MONOTONIC, &g_started);
  g_exiting = 0;
  g_requests = 0;
  g_last_request[0] = 0;
  g_host[0] = 0;

  if(net_init(err, sizeof(err)) != 0) {
    fprintf(stderr, "quark-dl: 网络层初始化失败: %s\n", err);
    boot_trace("网络层初始化失败: %s", err);
  } else {
    boot_trace("网络层就绪");
  }
  boot_trace("dl_init 开始 (%s)", data_dir());
  dl_init(data_dir());
  boot_trace("dl_init 完成");
  /* Same directory as the queue: cookie.txt and account.txt sit next to
   * downloads.txt so one folder holds everything a reset has to delete. */
  quark_init(data_dir());
  boot_trace("quark_init 完成");
  /* <data>/ca.pem is the console's only way to add a root: there is no shell to
   * export QDL_CA_FILE from. Missing file is the normal case, not an error. */
  {
    char ca[400];
    char why[220];

    snprintf(ca, sizeof(ca), "%s/ca.pem", data_dir());
    if(access(ca, R_OK) == 0) {
      int added = net_add_ca(ca, why, sizeof(why));

      if(added > 0) {
        fprintf(stderr, "quark-dl: 已附加 %d 张证书: %s\n", added, ca);
      } else {
        fprintf(stderr, "quark-dl: %s\n", why);
      }
    }
  }
}

void
api_set_port(unsigned short port) {
  g_port = port;
}

/* ------------------------------------------------------------ quark drive */

static const char *
path_arg(struct MHD_Connection *conn, const char *name) {
  const char *v = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, name);

  return (v && *v) ? v : NULL;
}

static enum MHD_Result
api_quark_qr(struct MHD_Connection *conn) {
  const char *act = path_arg(conn, "act");
  char eurl[900];
  char eerr[700];
  char enick[260];
  char json[1600];
  /* Per-thread rather than per-frame: this struct carries a 7 KB cookie, and
   * these handlers run on libmicrohttpd worker threads whose stack is small
   * and not ours to set. Each thread serves one connection at a time, so a
   * thread-local copy is as private as a stack copy and costs no stack. */
  static __thread quark_session s;

  if(!act || !strcmp(act, "start")) {
    char url[600];
    char token[160];
    char err[400];

    if(quark_qr_new(url, sizeof(url), token, sizeof(token), err, sizeof(err))) {
      json_escape(eerr, sizeof(eerr), err);
      snprintf(json, sizeof(json), "{\"ok\":false,\"error\":\"%s\"}", eerr);
      return send_json(conn, MHD_HTTP_OK, json);
    }
    json_escape(eurl, sizeof(eurl), url);
    snprintf(json, sizeof(json), "{\"ok\":true,\"token\":\"%s\",\"url\":\"%s\"}",
             token, eurl);
    return send_json(conn, MHD_HTTP_OK, json);
  }

  if(!strcmp(act, "poll")) {
    const char *token = path_arg(conn, "token");
    char err[400];
    int rc;

    if(!token) {
      return send_json(conn, MHD_HTTP_BAD_REQUEST,
                       "{\"ok\":false,\"error\":\"缺少 token\"}");
    }
    memset(&s, 0, sizeof(s));
    rc = quark_qr_poll(token, &s, err, sizeof(err));
    if(rc < 0) {
      json_escape(eerr, sizeof(eerr), err);
      snprintf(json, sizeof(json), "{\"ok\":false,\"error\":\"%s\"}", eerr);
      return send_json(conn, MHD_HTTP_OK, json);
    }
    if(rc == QUARK_QR_WAITING) {
      return send_json(conn, MHD_HTTP_OK,
                       "{\"ok\":true,\"state\":\"waiting\"}");
    }
    if(rc == QUARK_QR_EXPIRED) {
      return send_json(conn, MHD_HTTP_OK,
                       "{\"ok\":true,\"state\":\"expired\"}");
    }
    /* The session is already on disk. The nickname is a nicety: if the call
     * fails the console is still logged in, so it must not turn into an error. */
    quark_load(&s);
    if(quark_whoami(&s, err, sizeof(err)) == 0) {
      quark_save(&s);
    }
    json_escape(enick, sizeof(enick), s.nickname);
    snprintf(json, sizeof(json),
             "{\"ok\":true,\"state\":\"ok\",\"nickname\":\"%s\","
             "\"total_bytes\":%lld,\"used_bytes\":%lld}",
             enick, s.total_bytes, s.used_bytes);
    return send_json(conn, MHD_HTTP_OK, json);
  }

  return send_json(conn, MHD_HTTP_BAD_REQUEST,
                   "{\"ok\":false,\"error\":\"act 只认 start 和 poll\"}");
}

static enum MHD_Result
api_quark_status(struct MHD_Connection *conn) {
  /* Per-thread rather than per-frame: this struct carries a 7 KB cookie, and
   * these handlers run on libmicrohttpd worker threads whose stack is small
   * and not ours to set. Each thread serves one connection at a time, so a
   * thread-local copy is as private as a stack copy and costs no stack. */
  static __thread quark_session s;
  char enick[260];
  char eerr[520];
  char json[1100];

  quark_load(&s);
  json_escape(enick, sizeof(enick), s.nickname);
  json_escape(eerr, sizeof(eerr), quark_last_error());
  snprintf(json, sizeof(json),
           "{\"ok\":true,\"logged_in\":%s,\"nickname\":\"%s\","
           "\"total_bytes\":%lld,\"used_bytes\":%lld,\"last_error\":\"%s\"}",
           s.valid ? "true" : "false", enick, s.total_bytes, s.used_bytes,
           eerr);
  return send_json(conn, MHD_HTTP_OK, json);
}

/* Body field "cookie". Verifies against the service immediately: storing a
 * typo and reporting success would leave the user staring at an empty folder
 * wondering which of the two things is broken. */
static enum MHD_Result
api_quark_login(struct MHD_Connection *conn, const char *body, size_t size) {
  /* Far larger than the cookie itself: a "Copy as cURL" paste carries every
   * header of the request, and truncating it would cut the cookie in half. */
  char raw[16 * 1024];
  char err[600];
  char eerr[700];
  char enick[260];
  char keys[400];
  char ekeys[500];
  char json[2400];
  /* Per-thread rather than per-frame: this struct carries a 7 KB cookie, and
   * these handlers run on libmicrohttpd worker threads whose stack is small
   * and not ours to set. Each thread serves one connection at a time, so a
   * thread-local copy is as private as a stack copy and costs no stack. */
  static __thread quark_session s;
  int nkeys = 0;
  int saved;

  if(!form_field(body, size, "cookie", raw, sizeof(raw))) {
    return send_json(conn, MHD_HTTP_BAD_REQUEST,
                     "{\"ok\":false,\"error\":\"没有收到 cookie 字段\"}");
  }
  if(quark_set_cookie(raw, &s, err, sizeof(err))) {
    json_escape(eerr, sizeof(eerr), err);
    snprintf(json, sizeof(json), "{\"ok\":false,\"error\":\"%s\"}", eerr);
    return send_json(conn, MHD_HTTP_OK, json);
  }
  /* Names of what was actually parsed. The page shows them, because "0 fields"
   * is the fastest way to see that a paste landed empty. */
  quark_cookie_keys(s.cookie, keys, sizeof(keys), &nkeys);
  json_escape(ekeys, sizeof(ekeys), keys);

  if(quark_whoami(&s, err, sizeof(err))) {
    /* The cookie is kept anyway. It may be the network that is down, and
     * throwing away a probably-good cookie because the console could not reach
     * the service would make the user paste it again for nothing. */
    char eraw[700];

    saved = quark_save(&s);
    json_escape(eerr, sizeof(eerr), err);
    json_escape(eraw, sizeof(eraw), quark_last_raw());
    snprintf(json, sizeof(json),
             "{\"ok\":false,\"saved\":%s,\"error\":\"%s\",\"raw\":\"%s\","
             "\"count\":%d,\"keys\":\"%s\"}",
             saved == 0 ? "true" : "false", eerr, eraw, nkeys, ekeys);
    return send_json(conn, MHD_HTTP_OK, json);
  }
  quark_save(&s);
  json_escape(enick, sizeof(enick), s.nickname);
  snprintf(json, sizeof(json),
           "{\"ok\":true,\"nickname\":\"%s\",\"total_bytes\":%lld,"
           "\"used_bytes\":%lld,\"count\":%d,\"keys\":\"%s\"}",
           enick, s.total_bytes, s.used_bytes, nkeys, ekeys);
  return send_json(conn, MHD_HTTP_OK, json);
}

static enum MHD_Result
api_quark_logout(struct MHD_Connection *conn, const char *method) {
  if(strcmp(method, MHD_HTTP_METHOD_POST)) {
    return api_not_found(conn, "/api/quark/logout");
  }
  quark_logout();
  return send_json(conn, MHD_HTTP_OK, "{\"ok\":true}");
}

static enum MHD_Result
api_quark_list(struct MHD_Connection *conn) {
  const char *pdir = path_arg(conn, "pdir");
  const char *pages = path_arg(conn, "page");
  /* Per-thread rather than per-frame: this struct carries a 7 KB cookie, and
   * these handlers run on libmicrohttpd worker threads whose stack is small
   * and not ours to set. Each thread serves one connection at a time, so a
   * thread-local copy is as private as a stack copy and costs no stack. */
  static __thread quark_session s;
  quark_entry *entries;
  char err[400];
  char eerr[900];
  char ename[900];
  char efid[120];
  char *out;
  size_t cap = 320u * 1024u;
  size_t used = 0;
  int page = pages ? atoi(pages) : 1;
  int total = 0;
  int n;
  int i;

  quark_load(&s);
  if(!s.valid) {
    return send_json(conn, MHD_HTTP_OK,
                     "{\"ok\":false,\"need_login\":true,"
                     "\"error\":\"还没有登录\"}");
  }
  entries = (quark_entry *)calloc(200, sizeof(*entries));
  out = (char *)malloc(cap);
  if(!entries || !out) {
    free(entries);
    free(out);
    return send_json(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                     "{\"ok\":false,\"error\":\"内存不足\"}");
  }
  n = quark_list(&s, pdir ? pdir : "0", page, 200, entries, 200, &total, err,
                 sizeof(err));
  if(n < 0) {
    enum MHD_Result rc;
    char eraw[900];

    json_escape(eerr, sizeof(eerr), err);
    /* The raw head of the response is a JSON document full of quotes, so it has
     * to be escaped before it becomes a string inside another JSON document. */
    json_escape(eraw, sizeof(eraw), quark_last_raw());
    snprintf(out, cap,
             "{\"ok\":false,\"need_login\":%s,\"error\":\"%s\",\"raw\":\"%s\"}",
             strstr(err, "登录") ? "true" : "false", eerr, eraw);
    free(entries);
    rc = send_json(conn, MHD_HTTP_OK, out);
    free(out);
    return rc;
  }
  used += (size_t)snprintf(out + used, cap - used,
                           "{\"ok\":true,\"pdir\":\"%s\",\"page\":%d,"
                           "\"total\":%d,\"entries\":[",
                           pdir ? pdir : "0", page, total);
  for(i = 0; i < n && used + 1600 < cap; i++) {
    json_escape(efid, sizeof(efid), entries[i].fid);
    json_escape(ename, sizeof(ename), entries[i].name);
    used += (size_t)snprintf(out + used, cap - used,
                             "%s{\"fid\":\"%s\",\"name\":\"%s\",\"dir\":%s,"
                             "\"size\":%lld,\"updated_at\":%lld}",
                             i ? "," : "", efid, ename,
                             entries[i].is_dir ? "true" : "false",
                             entries[i].size, entries[i].updated_at);
  }
  snprintf(out + used, cap - used, "]}");
  free(entries);
  {
    enum MHD_Result rc = send_json(conn, MHD_HTTP_OK, out);

    free(out);
    return rc;
  }
}

/* Body fields:
 *   items - one "fid<TAB>name<TAB>d" per line. The trailing "d" marks a folder,
 *           which is expanded recursively; without it the line is one file.
 *   dir   - destination directory, may be empty
 * A tab separates the fields because a file name may contain anything else,
 * including the pipe that "fid|name" would have used. */
#define QUARK_ADD_MAX 2000
#define QUARK_BATCH 50

typedef struct {
  char fid[QUARK_FID_MAX];
  char name[QUARK_NAME_MAX];
} add_item;

typedef struct {
  add_item *out;
  int count;
  int cap;
  int overflow;
} collect_ctx;

static int
collect_cb(void *ctx, const quark_entry *e, const char *pdir) {
  collect_ctx *c = (collect_ctx *)ctx;

  (void)pdir;
  if(c->count >= c->cap) {
    /* Returning non-zero stops the walk, which is the only way to report that
     * a folder holds more than this endpoint will take. */
    c->overflow = 1;
    return 1;
  }
  snprintf(c->out[c->count].fid, QUARK_FID_MAX, "%s", e->fid);
  snprintf(c->out[c->count].name, QUARK_NAME_MAX, "%s", e->name);
  c->count++;
  return 0;
}

static enum MHD_Result
api_quark_add(struct MHD_Connection *conn, const char *body, size_t size) {
  char *items;
  char dest[512];
  char err[400];
  char eerr[700];
  char first[400];
  char json[1600];
  /* Per-thread rather than per-frame: this struct carries a 7 KB cookie, and
   * these handlers run on libmicrohttpd worker threads whose stack is small
   * and not ours to set. Each thread serves one connection at a time, so a
   * thread-local copy is as private as a stack copy and costs no stack. */
  static __thread quark_session s;
  collect_ctx cc;
  add_item *arr;
  char *line;
  int queued = 0;
  int failed = 0;
  int dirs = 0;

  items = (char *)malloc(256u * 1024u);
  arr = (add_item *)calloc(QUARK_ADD_MAX, sizeof(*arr));
  if(!items || !arr) {
    free(items);
    free(arr);
    return send_json(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                     "{\"ok\":false,\"error\":\"内存不足\"}");
  }
  if(!form_field(body, size, "items", items, 256u * 1024u)) {
    free(items);
    free(arr);
    return send_json(conn, MHD_HTTP_BAD_REQUEST,
                     "{\"ok\":false,\"error\":\"没有收到 items 字段\"}");
  }
  dest[0] = 0;
  if(form_field(body, size, "dir", dest, sizeof(dest)) && dest[0] != '/') {
    dest[0] = 0;
  }
  first[0] = 0;
  quark_load(&s);
  if(!s.valid) {
    free(items);
    free(arr);
    return send_json(conn, MHD_HTTP_OK,
                     "{\"ok\":false,\"need_login\":true,"
                     "\"error\":\"还没有登录\"}");
  }
  cc.out = arr;
  cc.count = 0;
  cc.cap = QUARK_ADD_MAX;
  cc.overflow = 0;

  for(line = strtok(items, "\n"); line; line = strtok(NULL, "\n")) {
    char *tab = strchr(line, '\t');
    char *flag = NULL;
    const char *fid = line;
    const char *name = NULL;

    if(tab) {
      *tab = 0;
      name = tab + 1;
      flag = strchr(tab + 1, '\t');
      if(flag) {
        *flag = 0;
        flag++;
      }
    }
    if(!line[0]) {
      continue;
    }
    if(flag && flag[0] == 'd') {
      int before = cc.count;

      dirs++;
      if(quark_walk(&s, fid, 32, collect_cb, &cc, NULL, err, sizeof(err))) {
        failed++;
        if(!first[0]) {
          snprintf(first, sizeof(first), "%s: %s", name ? name : fid, err);
        }
        continue;
      }
      if(cc.count == before) {
        /* A folder that expands to nothing is not a failure, but saying
         * "加入 0 个文件" with no reason would be baffling. */
        failed++;
        if(!first[0]) {
          snprintf(first, sizeof(first), "%s: 这个目录里没有文件",
                   name ? name : fid);
        }
      }
      continue;
    }
    if(cc.count >= cc.cap) {
      cc.overflow = 1;
      break;
    }
    snprintf(arr[cc.count].fid, QUARK_FID_MAX, "%s", fid);
    snprintf(arr[cc.count].name, QUARK_NAME_MAX, "%s", name ? name : "");
    cc.count++;
  }
  free(items);

  if(cc.overflow) {
    failed++;
    if(!first[0]) {
      snprintf(first, sizeof(first), "一次最多 %d 个文件，请分批加入",
               QUARK_ADD_MAX);
    }
  }

  /* Direct links come in batches: one request per file would mean a round trip
   * and a spinner that does not move for every single file in a folder.
   * The URL table is on the heap - fifty 2 KB strings is a hundred kilobytes,
   * which is not something to put in a console thread's frame. */
  {
    int base = 0;
    char(*urls)[QUARK_URL_MAX] =
        (char(*)[QUARK_URL_MAX])malloc((size_t)QUARK_BATCH * QUARK_URL_MAX);

    if(!urls) {
      free(arr);
      return send_json(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                       "{\"ok\":false,\"error\":\"内存不足\"}");
    }
    while(base < cc.count) {
      const char *fids[QUARK_BATCH];
      int n = cc.count - base;
      int i;
      int got;

      if(n > QUARK_BATCH) {
        n = QUARK_BATCH;
      }
      for(i = 0; i < n; i++) {
        fids[i] = arr[base + i].fid;
        urls[i][0] = 0;
      }
      got = quark_direct_many(&s, fids, n, (char(*)[QUARK_URL_MAX])urls, err,
                              sizeof(err));
      if(got < 0) {
        failed += n;
        if(!first[0]) {
          snprintf(first, sizeof(first), "%s", err);
        }
        base += n;
        continue;
      }
      for(i = 0; i < n; i++) {
        if(!urls[i][0]) {
          failed++;
          if(!first[0]) {
            snprintf(first, sizeof(first), "%s: 没有拿到直链",
                     arr[base + i].name);
          }
          continue;
        }
        if(dl_add(urls[i], arr[base + i].name[0] ? arr[base + i].name : NULL,
                  dest[0] ? dest : NULL, err, sizeof(err)) < 0) {
          failed++;
          if(!first[0]) {
            snprintf(first, sizeof(first), "%s: %s", arr[base + i].name, err);
          }
          continue;
        }
        queued++;
      }
      base += n;
    }
    free(urls);
  }
  free(arr);
  json_escape(eerr, sizeof(eerr), first);
  {
    char eraw[520];

    json_escape(eraw, sizeof(eraw), failed ? quark_last_raw() : "");
    snprintf(json, sizeof(json),
             "{\"ok\":true,\"queued\":%d,\"failed\":%d,\"dirs\":%d,"
             "\"error\":\"%s\",\"raw\":\"%s\"}",
             queued, failed, dirs, eerr, eraw);
    return send_json(conn, MHD_HTTP_OK, json);
  }
}

/* Prints whatever an API path answers, as a JSON string. Deliberately not
 * prettified: when a field name turns out to be wrong, the raw answer is what
 * makes it obvious, and guessing at the shape is how this would otherwise cost
 * another round trip. */
static enum MHD_Result
api_quark_probe(struct MHD_Connection *conn) {
  const char *path = path_arg(conn, "path");
  /* Per-thread rather than per-frame: this struct carries a 7 KB cookie, and
   * these handlers run on libmicrohttpd worker threads whose stack is small
   * and not ours to set. Each thread serves one connection at a time, so a
   * thread-local copy is as private as a stack copy and costs no stack. */
  static __thread quark_session s;
  char *out;
  char err[400];

  quark_load(&s);
  if(!s.valid) {
    return send_json(conn, MHD_HTTP_OK,
                     "{\"ok\":false,\"need_login\":true,"
                     "\"error\":\"还没有登录\"}");
  }
  out = (char *)malloc(320u * 1024u);
  if(!out) {
    return send_json(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                     "{\"ok\":false,\"error\":\"内存不足\"}");
  }
  if(quark_probe(&s, path ? path : "/1/clouddrive/member?" QUARK_COMMON, out,
                 320u * 1024u, err, sizeof(err))) {
    /* The failure is wrapped here, not in quark.c, because this is where the
     * escaper lives - and an API error message routinely contains the quotes
     * and braces of the very response being debugged. */
    char eerr[700];
    char eraw[520];

    json_escape(eerr, sizeof(eerr), err);
    json_escape(eraw, sizeof(eraw), quark_last_raw());
    snprintf(out, 320u * 1024u,
             "{\"ok\":false,\"error\":\"%s\",\"raw\":\"%s\"}", eerr, eraw);
  }
  {
    enum MHD_Result rc = send_json(conn, MHD_HTTP_OK, out);

    free(out);
    return rc;
  }
}

enum MHD_Result
api_request(struct MHD_Connection *conn, const char *url, const char *method,
            const char *body, size_t size) {
  char path[256];
  int is_post = !strcmp(method, MHD_HTTP_METHOD_POST);
  int is_get = !strcmp(method, MHD_HTTP_METHOD_GET) ||
               !strcmp(method, MHD_HTTP_METHOD_HEAD);
  const char *host;

  normalize_path(path, sizeof(path), url);

  /* Why a request was answered the way it was: when a browser says "cannot
   * open", /api/diag?format=json already shows whether we ever saw it. */
  g_requests++;
  snprintf(g_last_request, sizeof(g_last_request), "%s %s", method, path);
  host = MHD_lookup_connection_value(conn, MHD_HEADER_KIND, "Host");
  if(host) {
    snprintf(g_host, sizeof(g_host), "%s", host);
  }

  if(!strcmp(path, "/api/health") || !strcmp(path, "/api/health.json")) {
    return is_get ? api_health(conn) : api_not_found(conn, path);
  }
  if(!strcmp(path, "/api/diag") || !strcmp(path, "/api/diag.json")) {
    if(!is_get) {
      return api_not_found(conn, path);
    }
    return wants_html(conn) ? api_diag_html(conn) : api_diag_json(conn);
  }
  if(!strcmp(path, "/api/diag.txt")) {
    return is_get ? api_diag_text(conn) : api_not_found(conn, path);
  }
  if(!strcmp(path, "/api/exit")) {
    return is_post ? api_exit(conn) : api_exit_needs_post(conn);
  }
  if(!strcmp(path, "/api/net/selftest") || !strcmp(path, "/api/net")) {
    return is_get ? api_net_selftest(conn) : api_not_found(conn, path);
  }
  if(!strcmp(path, "/api/dl/list")) {
    return is_get ? api_dl_list(conn) : api_not_found(conn, path);
  }
  if(!strcmp(path, "/api/dl/add")) {
    return is_post ? api_dl_add(conn, body, size) : api_not_found(conn, path);
  }
  if(!strcmp(path, "/api/dl/cancel")) {
    return api_dl_cancel(conn, method);
  }
  if(!strcmp(path, "/api/dl/retry")) {
    return api_dl_retry(conn, method);
  }
  if(!strcmp(path, "/api/dl/clear")) {
    return api_dl_clear(conn, method);
  }
  if(!strcmp(path, "/api/settings")) {
    return (is_get || is_post) ? api_settings(conn, body, size, method)
                               : api_not_found(conn, path);
  }
  if(!strcmp(path, "/api/fs/list")) {
    return is_get ? api_fs_list(conn) : api_not_found(conn, path);
  }
  if(!strcmp(path, "/api/quark/status")) {
    return is_get ? api_quark_status(conn) : api_not_found(conn, path);
  }
  if(!strcmp(path, "/api/quark/login")) {
    return is_post ? api_quark_login(conn, body, size)
                   : api_not_found(conn, path);
  }
  if(!strcmp(path, "/api/quark/qr")) {
    return is_get ? api_quark_qr(conn) : api_not_found(conn, path);
  }
  if(!strcmp(path, "/api/quark/logout")) {
    return api_quark_logout(conn, method);
  }
  if(!strcmp(path, "/api/quark/list")) {
    return is_get ? api_quark_list(conn) : api_not_found(conn, path);
  }
  if(!strcmp(path, "/api/quark/add")) {
    return is_post ? api_quark_add(conn, body, size)
                   : api_not_found(conn, path);
  }
  if(!strcmp(path, "/api/quark/probe")) {
    return is_get ? api_quark_probe(conn) : api_not_found(conn, path);
  }

  return api_not_found(conn, path);
}
