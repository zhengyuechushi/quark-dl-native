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

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "build.h"
#include "kern.h"
#include "notify.h"
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
 * XHR, which sends "*/*", so the JSON path stays the default for scripts. */
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
  char body[2048];
  diag_view_t d;

  collect_diag(&d);
  json_escape(kernel, sizeof(kernel), d.kernel);
  json_escape(host, sizeof(host), d.host);
  json_escape(last, sizeof(last), d.last);

  snprintf(body, sizeof(body),
           "{\"app\":\"%s\",\"version\":\"%s\",\"build\":\"%s\","
           "\"pid\":%d,\"ppid\":%d,\"port\":%u,\"uptime_ms\":%ld,"
           "\"name\":{\"expected\":\"%s\",\"kernel\":\"%s\",\"readable\":%s,"
           "\"ok\":%s,\"instances\":%d},"
           "\"host\":\"%s\",\"requests\":%u,\"last_request\":\"%s\","
           "\"exiting\":%s}",
           APP_NAME, APP_VERSION, d.build,
           d.pid, d.ppid, d.port, d.uptime,
           PROCESS_NAME, kernel,
           d.named ? "true" : "false",
           (d.named && !strcmp(d.kernel, PROCESS_NAME)) ? "true" : "false",
           d.instances,
           host, d.requests, last,
           g_exiting ? "true" : "false");
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
           "exiting   %s\n",
           APP_NAME, APP_VERSION,
           d.build,
           d.pid, d.ppid,
           d.port,
           d.host,
           d.kernel, PROCESS_NAME,
           d.instances,
           d.uptime,
           d.requests, d.last[0] ? d.last : "-",
           g_exiting ? "yes" : "no");
  return send_body(conn, MHD_HTTP_OK, API_TEXT, body);
}

static enum MHD_Result
api_diag_html(struct MHD_Connection *conn) {
  char kernel[512];
  char host[160];
  char body[4096];
  diag_view_t d;

  collect_diag(&d);
  html_escape(kernel, sizeof(kernel), d.kernel);
  html_escape(host, sizeof(host), d.host);

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
           "</table><p class=\"f\">"
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
           d.requests);
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

void
api_init(void) {
  clock_gettime(CLOCK_MONOTONIC, &g_started);
  g_exiting = 0;
  g_requests = 0;
  g_last_request[0] = 0;
  g_host[0] = 0;
}

void
api_set_port(unsigned short port) {
  g_port = port;
}

enum MHD_Result
api_request(struct MHD_Connection *conn, const char *url, const char *method,
            const char *body, size_t size) {
  char path[256];
  int is_post = !strcmp(method, MHD_HTTP_METHOD_POST);
  int is_get = !strcmp(method, MHD_HTTP_METHOD_GET) ||
               !strcmp(method, MHD_HTTP_METHOD_HEAD);
  const char *host;

  (void)body;
  (void)size;

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

  return api_not_found(conn, path);
}
