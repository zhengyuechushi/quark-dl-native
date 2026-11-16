/* Copyright (C) 2024 John Törnblom

This program is free software; you can redistribute it and/or modify it
under the terms of the GNU General Public License as published by the
Free Software Foundation; either version 3, or (at your option) any
later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; see the file COPYING. If not, see
<http://www.gnu.org/licenses/>.  */

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include <microhttpd.h>

#include "asset.h"
#include "boot.h"
#include "websrv.h"


/**
 * File not found (404)
 **/
#define PAGE_404                      \
  "<html>"                            \
    "<head>"                          \
      "<title>File not found</title>" \
    "</head>"                         \
    "<body>File not found</body>"     \
  "</html>"


typedef struct asset {
  const char   *path;
  const char   *mime;
  const char   *encoding;
  const void   *data;
  size_t        size;
  struct asset *next;
} asset_t;


static asset_t* g_asset_head = 0;


/* Copies the request target into `path`, collapsing runs of '/'.
 *
 * Returns -1 when the target does not fit. That check is the point: MHD hands
 * this function whatever the client sent (up to its connection memory limit),
 * and the buffer is PATH_MAX on the stack. Without the bound, one request with
 * a very long path walks off `path` and returns into whatever the stack holds
 * next - a remote crash, or worse, on a console that has no memory protection
 * to stop it. A target that long cannot name an asset anyway. */
static int
asset_normalize_path(const char *url, char *path, size_t cap) {
  size_t len = strlen(url);
  size_t o = 0;

  if(!len || len >= cap) {
    return -1;
  }
  for(size_t i = 0; i < len; i++) {
    if(url[i] == '/' && url[i + 1] == '/') {
      continue;
    }
    if(o + 1 >= cap) {
      return -1;
    }
    path[o++] = url[i];
  }
  path[o] = '\0';
  return 0;
}


void
asset_register(const char* path, const void* data, size_t size,
               const char* mime, const char* encoding) {
  static int announced = 0;
  asset_t* a = calloc(1, sizeof(asset_t));

  /* This runs from a constructor, i.e. before main and before any notification.
   * One line here is the only difference between "the ELF was never loaded" and
   * "it loaded and died later", which look identical on the console. */
  if(!announced) {
    announced = 1;
    boot_trace("ELF 已加载, 构造函数执行, 首个资源 %s", path);
  }

  if(!a) {
    return;
  }

  a->path = path;
  a->mime = mime;
  a->encoding = encoding;
  a->data = data;
  a->size = size;
  a->next = g_asset_head;

  g_asset_head = a;
}


enum MHD_Result
asset_request(struct MHD_Connection *conn, const char* url) {
  unsigned int status = MHD_HTTP_NOT_FOUND;
  enum MHD_Result ret = MHD_NO;
  size_t size = strlen(PAGE_404);
  struct MHD_Response *resp;
  const void* data = PAGE_404;
  const char* mime = 0;
  const char* encoding = 0;
  char path[PATH_MAX];

  if(asset_normalize_path(url, path, sizeof(path)) == 0) {
    for(asset_t* a=g_asset_head; a!=0; a=a->next) {
      if(!strcmp(path, a->path)) {
        data = a->data;
        size = a->size;
        mime = a->mime;
        encoding = a->encoding;
        status = MHD_HTTP_OK;
        break;
      }
    }
  }

  if((resp=MHD_create_response_from_buffer(size, (void *)data,
					   MHD_RESPMEM_PERSISTENT))) {
    if(mime) {
      MHD_add_response_header(resp, MHD_HTTP_HEADER_CONTENT_TYPE, mime);
    }
    if(encoding) {
      MHD_add_response_header(resp, MHD_HTTP_HEADER_CONTENT_ENCODING, encoding);
    }
    /* Nobody caches this but the browser, and a cached copy is how a user ends
     * up staring at the previous release and reporting that a card is missing.
     * The whole bundle is under 200 KB over a LAN, so re-sending it costs
     * nothing; being wrong about which build is on screen costs a round trip
     * through CI. */
    MHD_add_response_header(resp, MHD_HTTP_HEADER_CACHE_CONTROL,
                            "no-store, must-revalidate");
    MHD_add_response_header(resp, "Pragma", "no-cache");
    ret = websrv_queue_response(conn, status, resp);
    MHD_destroy_response(resp);
  }

  return ret;
}
