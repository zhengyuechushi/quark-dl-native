/* Minimal API surface for the skeleton build.
 *
 * Stage 1 has one job: prove the payload loads, carries the right process name
 * and can be stopped on purpose. Everything the console cannot show (kernel
 * thread name, instance count, pid, build) is exposed here so a plain browser
 * request is enough evidence.
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

static struct timespec g_started;
static unsigned short g_port;
static volatile int g_exiting;

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

static enum MHD_Result
send_json(struct MHD_Connection *conn, unsigned int status, const char *body) {
  struct MHD_Response *resp;
  enum MHD_Result ret;

  resp = MHD_create_response_from_buffer(strlen(body), (void *)body,
                                         MHD_RESPMEM_MUST_COPY);
  if(!resp) {
    return MHD_NO;
  }
  MHD_add_response_header(resp, MHD_HTTP_HEADER_CONTENT_TYPE,
                          "application/json; charset=utf-8");
  ret = websrv_queue_response(conn, status, resp);
  MHD_destroy_response(resp);
  return ret;
}

static void *
stop_later(void *arg) {
  (void)arg;
  /* Let the response reach the browser before accept() stops returning. */
  usleep(250000);
  websrv_stop();
  return 0;
}

static enum MHD_Result
api_health(struct MHD_Connection *conn) {
  char body[512];

  snprintf(body, sizeof(body),
           "{\"ok\":true,\"app\":\"%s\",\"version\":\"%s\",\"build\":\"%s\","
           "\"pid\":%d,\"port\":%u,\"uptime_ms\":%ld,\"name\":\"%s\"}",
           APP_NAME, APP_VERSION, BUILD_ID, qdl_self_pid(), (unsigned)g_port,
           elapsed_ms(), PROCESS_NAME);
  return send_json(conn, MHD_HTTP_OK, body);
}

static enum MHD_Result
api_diag(struct MHD_Connection *conn) {
  char kernel_name[256];
  char safe_name[384];
  char body[2048];
  int named;
  int instances;

  named = qdl_kernel_name(kernel_name, sizeof(kernel_name)) == 0;
  if(!named) {
    snprintf(kernel_name, sizeof(kernel_name), "unknown");
  }
  json_escape(safe_name, sizeof(safe_name), kernel_name);
  instances = qdl_count_named(PROCESS_NAME);

  snprintf(body, sizeof(body),
           "{\"app\":\"%s\",\"version\":\"%s\",\"build\":\"%s\","
           "\"pid\":%d,\"ppid\":%d,\"port\":%u,\"uptime_ms\":%ld,"
           "\"name\":{\"expected\":\"%s\",\"kernel\":\"%s\",\"readable\":%s,"
           "\"ok\":%s,\"instances\":%d},"
           "\"exiting\":%s}",
           APP_NAME, APP_VERSION, BUILD_ID,
           qdl_self_pid(), qdl_parent_pid(), (unsigned)g_port, elapsed_ms(),
           PROCESS_NAME, safe_name,
           named ? "true" : "false",
           (named && !strcmp(kernel_name, PROCESS_NAME)) ? "true" : "false",
           instances,
           g_exiting ? "true" : "false");
  return send_json(conn, MHD_HTTP_OK, body);
}

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

void
api_init(void) {
  clock_gettime(CLOCK_MONOTONIC, &g_started);
  g_exiting = 0;
}

void
api_set_port(unsigned short port) {
  g_port = port;
}

enum MHD_Result
api_request(struct MHD_Connection *conn, const char *url, const char *method,
            const char *body, size_t size) {
  int is_post = !strcmp(method, MHD_HTTP_METHOD_POST);
  int is_get = !strcmp(method, MHD_HTTP_METHOD_GET) ||
               !strcmp(method, MHD_HTTP_METHOD_HEAD);

  (void)body;
  (void)size;

  if(is_get && !strcmp(url, "/api/health")) {
    return api_health(conn);
  }
  if(is_get && !strcmp(url, "/api/diag")) {
    return api_diag(conn);
  }
  if(is_post && !strcmp(url, "/api/exit")) {
    return api_exit(conn);
  }

  return send_json(conn, MHD_HTTP_NOT_FOUND,
                   "{\"ok\":false,\"error\":\"unknown endpoint\"}");
}
