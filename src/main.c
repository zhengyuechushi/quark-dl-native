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
#include <errno.h>
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
#include "boot.h"
#include "build.h"
#include "dl.h"
#include "notify.h"
#include "websrv.h"

/* The process list renders this. Keep the .elf suffix. */
#define PROCESS_NAME "quark-dl.elf"

/* 8888 is taken by other payloads on this console (ps5-web-file-manager uses it
 * by default), and a silent step to 8889 was the reason a browser could reach a
 * different plugin's UI at the address it was told to open. Serve on 8520 and
 * only walk forward a short distance, so the port stays predictable: the home
 * screen tile (starter/) probes this same range when it asks for the page. */
#define DEFAULT_PORT 8520
#define PORT_SPAN 32

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
find_available_port(unsigned short start, unsigned short span) {
  unsigned int last = (unsigned int)start + span;
  unsigned int port;

  if(last > 65535) {
    last = 65535;
  }
  for(port = start; port <= last; port++) {
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
#endif

  /* From here on every step leaves a line in <data>/boot.log. The case this
   * exists for is the one where nothing happens at all: no process, no
   * notification, no page. The file then says whether the ELF never loaded,
   * died during init, or is serving somewhere else. Kept outside __SCE__ so the
   * Linux build exercises the same trail in CI. */
  boot_reset();
  boot_trace("main 进入, build %s, pid %d", BUILD_ID, (int)getpid());
  boot_watch_signals();

#ifdef __SCE__
  /* One instance per console. A stale copy that moved to the next port would
   * otherwise keep serving an older build, and requests sent to the announced
   * port could land on it instead. Bounded on purpose: if the kernel will not
   * let the old one go, serving is more useful than spinning here forever with
   * no port open. */
  for(int tries = 0; tries < 5; tries++) {
    if((pid = find_pid(PROCESS_NAME)) <= 0) {
      break;
    }
    boot_trace("发现旧实例 pid %d, 正在结束", (int)pid);
    if(kill(pid, SIGKILL)) {
      perror("kill");
      boot_trace("结束旧实例失败: %s", strerror(errno));
      break;
    }
    sleep(1);
  }
  boot_trace("旧实例清理结束");
#endif

  puts(PROCESS_NAME);
  printf("build: %s\n", BUILD_ID);
  fflush(stdout);

  signal(SIGPIPE, SIG_IGN);
  signal(SIGCHLD, SIG_IGN);

  boot_trace("api_init 开始");
  api_init();
  boot_trace("api_init 完成");

  while(1) {
    port = find_available_port(DEFAULT_PORT, PORT_SPAN);
    if(!port) {
      fprintf(stderr, "no available port in %u..%u\n", DEFAULT_PORT,
              DEFAULT_PORT + PORT_SPAN);
      boot_trace("端口 %u..%u 全被占用", DEFAULT_PORT, DEFAULT_PORT + PORT_SPAN);
      /* Nothing to serve on: say so out loud instead of retrying forever with
       * no evidence, then keep trying in case the other plugin exits. */
      notify_user("Quark DL %s\n端口 %u..%u 全被占用\n无法启动服务",
                  BUILD_ID, DEFAULT_PORT, DEFAULT_PORT + PORT_SPAN);
      sleep(5);
      continue;
    }

    printf("listening on port %u\n", (unsigned)port);
    fflush(stdout);
    boot_trace("监听端口 %u", (unsigned)port);
    api_set_port(port);

#ifdef __SCE__
    if(notified_port != port) {
      notify_user("Quark DL %s\n端口: %u\n进程: %s",
                  BUILD_ID, (unsigned)port, PROCESS_NAME);
      notified_port = port;
      boot_trace("通知已发出");
    }
#endif

    websrv_listen(port);
    boot_trace("websrv_listen 返回, stop=%d", websrv_stop_requested());
    if(websrv_stop_requested()) {
      break;
    }
    /* websrv_listen returned on its own (accept error). Retry like the
     * reference payload does instead of dying silently. */
    sleep(3);
  }

  printf("exiting on request\n");
  fflush(stdout);
  boot_trace("收到退出请求");
  /* Let running downloads see the flag and flush their progress: a restart then
   * continues from the .part instead of starting the file again. */
  dl_shutdown();
  return 0;
}
