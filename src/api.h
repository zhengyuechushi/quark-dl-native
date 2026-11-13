#pragma once

#include <stddef.h>
#include <microhttpd.h>

void api_init(void);
void api_set_port(unsigned short port);

/* Handles every request under the api prefix. Returns MHD_YES when a response
 * was queued. */
enum MHD_Result api_request(struct MHD_Connection *conn, const char *url,
                            const char *method, const char *body, size_t size);
