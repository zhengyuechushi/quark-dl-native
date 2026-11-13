/* Portions of this file are taken from owendswang/ps5-web-file-manager
 * (GPL-3.0-or-later), the payload whose process name and /api/exit behaviour are
 * confirmed working on real hardware. The startup sequence is deliberately kept
 * byte-for-byte equivalent where possible: name first, then reap old instances,
 * then bind. Anything that has to happen before serving goes below that block.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 3, or (at your option) any
 * later version.
 */

#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#ifdef __SCE__
#include <sys/syscall.h>
#include <sys/sysctl.h>
#endif

#include "api.h"
#include "build.h"
#include "notify.h"
#include "websrv.h"

/* The process list renders this. Keep the .elf suffix. */
#define PROCESS_NAME "quark-dl.elf"
#define DEFAULT_PORT 8888

static int
port_available(unsigned short port) {
  struct sockaddr_in addr;
  int fd;
  int ret;

  if((fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
    perror("socket");
    return -1;
  }
  if(setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &(int){1}, sizeof(int)) < 0) {
    perror("setsockopt");
    close(fd);
    return -1;
  }
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(port);

  ret = !bind(fd, (struct sockaddr *)&addr, sizeof(addr));
  close(fd);
  return ret;
}

static unsigned short
find_available_port(unsigned short start) {
  unsigned int port;

  for(port = start; port <= 65535; port++) {
    int available = port_available((unsigned short)port);
    if(available < 0) {
      return 0;
    }
    if(available) {
      return (unsigned short)port;
    }
  }
  return 0;
}

#ifdef __SCE__
/* Walk the kernel process table and return the pid of another process whose
 * ki_tdname equals `name`. Same offsets as the reference payload: 72 = ki_pid,
 * 447 = ki_tdname, and the first int of each record is ki_structsize. */
static pid_t
find_pid(const char *name) {
  int mib[4] = {1, 14, 8, 0};
  pid_t mypid = getpid();
  pid_t pid = -1;
  size_t buf_size = 0;
  uint8_t *buf;

  if(sysctl(mib, 4, 0, &buf_size, 0, 0)) {
    perror("sysctl");
    return -1;
  }
  if(buf_size < 512 || !(buf = malloc(buf_size))) {
    return -1;
  }
  if(sysctl(mib, 4, buf, &buf_size, 0, 0)) {
    perror("sysctl");
    free(buf);
    return -1;
  }

  for(uint8_t *ptr = buf; ptr + 512 <= buf + buf_size;) {
    int ki_structsize = *(int *)ptr;
    pid_t ki_pid = *(pid_t *)&ptr[72];
    char *ki_tdname = (char *)&ptr[447];

    if(ki_structsize < 512 || ptr + ki_structsize > buf + buf_size) {
      break;
    }
    ptr += ki_structsize;
    if(!strcmp(name, ki_tdname) && ki_pid != mypid) {
      pid = ki_pid;
    }
  }

  free(buf);
  return pid;
}
#endif

int
main(int argc, char **argv) {
  unsigned short port;
#ifdef __SCE__
  unsigned short notified_port = 0;
  pid_t pid;
#endif

  (void)argc;
  (void)argv;

#ifdef __SCE__
  /* Statement one, exactly like the reference payload. Nothing may run before
   * this: a single early return used to leave the loader's name in place and
   * made the rename look impossible. */
  syscall(SYS_thr_set_name, -1, PROCESS_NAME);

  /* One instance per console. A stale copy that moved to 8889 would otherwise
   * keep serving an older build on the same data directory. */
  while((pid = find_pid(PROCESS_NAME)) > 0) {
    if(kill(pid, SIGKILL)) {
      perror("kill");
      break;
    }
    sleep(1);
  }
#endif

  puts(PROCESS_NAME);
  printf("build: %s\n", BUILD_ID);
  fflush(stdout);

  signal(SIGPIPE, SIG_IGN);
  signal(SIGCHLD, SIG_IGN);

  api_init();

  while(1) {
    port = find_available_port(DEFAULT_PORT);
    if(!port) {
      fprintf(stderr, "no available port from %u\n", DEFAULT_PORT);
      sleep(3);
      continue;
    }

    printf("listening on port %u\n", port);
    fflush(stdout);
    api_set_port(port);

#ifdef __SCE__
    if(notified_port != port) {
      notify_user("Quark DL %s\n端口: %u\n进程: %s",
                  BUILD_ID, port, PROCESS_NAME);
      notified_port = port;
    }
#endif

    websrv_listen(port);
    if(websrv_stop_requested()) {
      break;
    }
    /* websrv_listen returned on its own (accept error). Retry like the
     * reference payload does instead of dying silently. */
    sleep(3);
  }

  printf("exiting on request\n");
  fflush(stdout);
  return 0;
}
