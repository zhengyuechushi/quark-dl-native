/* Derived from owendswang/ps5-web-file-manager (GPL-3.0-or-later): the MHD
 * daemon setup, the socket tuning and the accept loop are kept as-is because
 * that payload is verified to serve correctly on real hardware. The upload
 * streaming and file-manager routing are removed; requests go to api.c.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 3, or (at your option) any
 * later version.
 */

#include <signal.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <arpa/inet.h>
#include <microhttpd.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include "api.h"
#include "asset.h"
#include "websrv.h"

#define REQUEST_BODY_MAX (4 * 1024 * 1024)
#define HTTP_CONNECTION_MEMORY_LIMIT (8 * 1024 * 1024)
#define HTTP_CONNECTION_MEMORY_INCREMENT (2 * 1024 * 1024)
#define SOCKET_BUFFER_SIZE (512 * 1024)
#define HTTP_BODY_TOO_LARGE 413

static volatile sig_atomic_t g_stop_requested;
static int g_listen_fd = -1;

static void
socket_option(int fd, int level, int option, int value, const char *name) {
  if(setsockopt(fd, level, option, &value, sizeof(value))) {
    printf("HTTP: %s failed: %s\n", name, strerror(errno));
  }
}

static void
websrv_tune_connection_socket(int fd) {
  const int sndbuf = SOCKET_BUFFER_SIZE;
  const int nodelay = 1;

  /* OrbisOS HTTP sockets need an explicit send buffer to fill a GbE link. */
  socket_option(fd, SOL_SOCKET, SO_SNDBUF, sndbuf, "SO_SNDBUF");
  socket_option(fd, IPPROTO_TCP, TCP_NODELAY, nodelay, "TCP_NODELAY");
}

typedef struct request_context {
  char *body;
  size_t size;
  int too_large;
} request_context_t;

static enum MHD_Result
websrv_body_too_large(struct MHD_Connection *conn) {
  static const char json[] =
    "{\"ok\":false,\"error\":\"request body is too large\"}";
  struct MHD_Response *resp =
    MHD_create_response_from_buffer(sizeof(json) - 1, (void *)json,
                                    MHD_RESPMEM_PERSISTENT);
  enum MHD_Result ret;

  if(!resp) {
    return MHD_NO;
  }
  MHD_add_response_header(resp, MHD_HTTP_HEADER_CONTENT_TYPE,
                          "application/json");
  ret = websrv_queue_response(conn, HTTP_BODY_TOO_LARGE, resp);
  MHD_destroy_response(resp);
  return ret;
}

enum MHD_Result
websrv_queue_response(struct MHD_Connection *conn, unsigned int status,
                      struct MHD_Response *resp) {
  MHD_add_response_header(resp, MHD_HTTP_HEADER_ACCESS_CONTROL_ALLOW_ORIGIN, "*");
  MHD_add_response_header(resp, MHD_HTTP_HEADER_CACHE_CONTROL, "no-store");
  return MHD_queue_response(conn, status, resp);
}

void
websrv_stop(void) {
  g_stop_requested = 1;
  if(g_listen_fd >= 0) {
    shutdown(g_listen_fd, SHUT_RDWR);
  }
}

int
websrv_stop_requested(void) {
  return g_stop_requested;
}

static enum MHD_Result
websrv_on_request(void *cls, struct MHD_Connection *conn, const char *url,
                  const char *method, const char *version,
                  const char *upload_data, size_t *upload_data_size,
                  void **con_cls) {
  request_context_t *ctx = *con_cls;
  (void)cls;
  (void)version;

  if(strcmp(method, MHD_HTTP_METHOD_GET) &&
     strcmp(method, MHD_HTTP_METHOD_POST) &&
     strcmp(method, MHD_HTTP_METHOD_HEAD)) {
    return MHD_NO;
  }

  if(!ctx) {
    if(!(ctx = calloc(1, sizeof(*ctx)))) {
      return MHD_NO;
    }
    *con_cls = ctx;
    return MHD_YES;
  }

  if(*upload_data_size) {
    size_t chunk_size = *upload_data_size;

    if(chunk_size > REQUEST_BODY_MAX - ctx->size) {
      ctx->too_large = 1;
    } else if(!ctx->too_large) {
      char *body = realloc(ctx->body, ctx->size + chunk_size + 1);
      if(!body) {
        return MHD_NO;
      }
      ctx->body = body;
      memcpy(ctx->body + ctx->size, upload_data, chunk_size);
      ctx->size += chunk_size;
      ctx->body[ctx->size] = 0;
    }
    *upload_data_size = 0;
    return MHD_YES;
  }

  if(ctx->too_large) {
    return websrv_body_too_large(conn);
  }

  if(!strncmp(url, "/api/", 5)) {
    return api_request(conn, url, method, ctx->body, ctx->size);
  }
  if(!strcmp(url, "/") || !url[0]) {
    return asset_request(conn, "/index.html");
  }
  if(!strcmp(url, "/favicon.ico")) {
    return asset_request(conn, "/icon0.png");
  }
  return asset_request(conn, url);
}

static void
websrv_on_completed(void *cls, struct MHD_Connection *connection,
                    void **con_cls, enum MHD_RequestTerminationCode toe) {
  (void)cls;
  (void)connection;
  (void)toe;
  request_context_t *ctx = *con_cls;

  if(ctx) {
    free(ctx->body);
    free(ctx);
  }
  *con_cls = NULL;
}

int
websrv_listen(unsigned short port) {
  struct sockaddr_in server_addr;
  struct sockaddr_in client_addr;
  struct MHD_Daemon *httpd;
  socklen_t addr_len;
  int connfd;
  int srvfd;

  signal(SIGPIPE, SIG_IGN);

  if((srvfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
    perror("socket");
    return -1;
  }

  if(setsockopt(srvfd, SOL_SOCKET, SO_REUSEADDR, &(int){1}, sizeof(int)) < 0) {
    perror("setsockopt");
    close(srvfd);
    return -1;
  }
  {
    const int rcvbuf = SOCKET_BUFFER_SIZE;

    /* Set before listen so accepted PS5 sockets inherit the larger window. */
    socket_option(srvfd, SOL_SOCKET, SO_RCVBUF, rcvbuf, "SO_RCVBUF");
  }

  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
  server_addr.sin_port = htons(port);

  if(bind(srvfd, (struct sockaddr *)&server_addr, sizeof(server_addr))) {
    perror("bind");
    close(srvfd);
    return -1;
  }
  if(listen(srvfd, 16)) {
    perror("listen");
    close(srvfd);
    return -1;
  }

  g_stop_requested = 0;
  g_listen_fd = srvfd;

  /* One polling thread, not one thread per connection.
   *
   * Two reasons, and the first one is what a console actually showed: every
   * handler here needs a handful of kilobytes of scratch space, and a worker
   * thread that is created per connection has whatever stack the platform
   * hands it, which on this payload is not something to bet a login flow on.
   * With a single thread the scratch buffers live in .bss instead of a stack.
   *
   * The second: `__thread` is not usable here. This toolchain links the ELF
   * with no PT_TLS segment, so thread-locals silently degrade to ordinary
   * globals - shared, with no diagnostic. Serialising the handlers makes them
   * genuinely private. Downloads keep their own threads and are untouched;
   * what gets serialised is page requests, which are short and few. */
  if(!(httpd = MHD_start_daemon(MHD_USE_INTERNAL_POLLING_THREAD | MHD_USE_ITC |
                                MHD_USE_NO_LISTEN_SOCKET | MHD_USE_TURBO,
                                0, NULL, NULL, &websrv_on_request, NULL,
                                MHD_OPTION_CONNECTION_MEMORY_LIMIT,
                                (size_t)HTTP_CONNECTION_MEMORY_LIMIT,
                                MHD_OPTION_CONNECTION_MEMORY_INCREMENT,
                                (size_t)HTTP_CONNECTION_MEMORY_INCREMENT,
                                MHD_OPTION_NOTIFY_COMPLETED,
                                &websrv_on_completed, NULL, MHD_OPTION_END))) {
    perror("MHD_start_daemon");
    close(srvfd);
    g_listen_fd = -1;
    return -1;
  }

  while(!g_stop_requested) {
    addr_len = sizeof(client_addr);
    if((connfd = accept(srvfd, (struct sockaddr *)&client_addr, &addr_len)) < 0) {
      if(!g_stop_requested) perror("accept");
      break;
    }
    websrv_tune_connection_socket(connfd);
    if(MHD_add_connection(httpd, connfd, (struct sockaddr *)&client_addr,
                          addr_len) != MHD_YES) {
      perror("MHD_add_connection");
      close(connfd);
      break;
    }
  }

  MHD_stop_daemon(httpd);
  g_listen_fd = -1;
  return close(srvfd);
}

unsigned short
websrv_port(void) {
  struct sockaddr_in address;
  socklen_t size = sizeof(address);
  return g_listen_fd >= 0 && !getsockname(g_listen_fd, (struct sockaddr *)&address, &size) ?
    ntohs(address.sin_port) : 0;
}
