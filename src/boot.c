/* See boot.h for why this exists.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 3, or (at your option) any
 * later version.
 */

#include "boot.h"

#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef __SCE__
#include <stdlib.h>
#endif

#define BOOT_DIR_MAX 280
#define BOOT_ROTATE  120000

static void
boot_dir(char *out, size_t cap) {
#ifdef __SCE__
  snprintf(out, cap, "/data/quark-dl");
#else
  const char *env = getenv("QDL_DATA_DIR");

  /* Same directory as api.c's data_dir(): the log belongs next to the queue and
   * the cookie, so one folder is all a reset has to delete. */
  snprintf(out, cap, "%s", (env && *env) ? env : "./.quark-dl-data");
#endif
}

/* Opened, written and closed per line. A buffered stream would lose the last
 * line before a crash, which is the one that says where it died. */
static int
boot_open(void) {
  char dir[BOOT_DIR_MAX];
  char path[BOOT_DIR_MAX + 16];
  int fd;

  boot_dir(dir, sizeof(dir));
  snprintf(path, sizeof(path), "%s/boot.log", dir);

  fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
  if(fd >= 0) {
    return fd;
  }
  /* First run on a clean console: the directory is made by dl_init, which has
   * not run yet when the first line is written. */
  mkdir(dir, 0755);
  return open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
}

void
boot_trace(const char *fmt, ...) {
  char msg[280];
  char line[400];
  va_list ap;
  int n;
  int fd;

  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);

  n = snprintf(line, sizeof(line), "[%ld] %s\n", (long)time(0), msg);
  if(n <= 0) {
    return;
  }
  if(n > (int)sizeof(line) - 1) {
    n = (int)sizeof(line) - 1;
  }

  fd = boot_open();
  if(fd < 0) {
    return;
  }
  if(write(fd, line, (size_t)n) < 0) {
    /* Nothing useful to do: the whole point of this file is that it is the last
     * resort, and it must never be the reason the payload stops. */
  }
  close(fd);
}

void
boot_reset(void) {
  char dir[BOOT_DIR_MAX];
  char path[BOOT_DIR_MAX + 16];
  char old[BOOT_DIR_MAX + 24];
  struct stat st;

  boot_dir(dir, sizeof(dir));
  snprintf(path, sizeof(path), "%s/boot.log", dir);
  if(stat(path, &st) != 0 || st.st_size < BOOT_ROTATE) {
    return;
  }
  /* Keep one generation: the interesting run is usually the one that just
   * failed, and it is read after the next start has already begun. */
  snprintf(old, sizeof(old), "%s/boot.log.old", dir);
  rename(path, old);
}

static const char *
boot_signal_name(int sig) {
  switch(sig) {
    case SIGSEGV: return "SIGSEGV";
    case SIGBUS:  return "SIGBUS";
    case SIGILL:  return "SIGILL";
    case SIGABRT: return "SIGABRT";
    case SIGFPE:  return "SIGFPE";
    default:      return "signal";
  }
}

static void
boot_on_crash(int sig) {
  const char *name = boot_signal_name(sig);
  int fd;

  /* Written with plain write() and a fixed string: a crash handler that calls
   * printf can crash on the way to reporting a crash. */
  fd = boot_open();
  if(fd >= 0) {
    static const char pre[] = "FATAL ";
    static const char post[] = " — the line above is where it stopped\n";

    (void)!write(fd, pre, sizeof(pre) - 1);
    (void)!write(fd, name, strlen(name));
    (void)!write(fd, post, sizeof(post) - 1);
    close(fd);
  }
  /* Take the default action, so the console sees a crash rather than a process
   * that quietly decided to exit. */
  signal(sig, SIG_DFL);
  raise(sig);
}

void
boot_watch_signals(void) {
  static const int sigs[] = {SIGSEGV, SIGBUS, SIGILL, SIGABRT, SIGFPE};
  struct sigaction sa;

  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = boot_on_crash;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_RESETHAND;
  for(size_t i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++) {
    sigaction(sigs[i], &sa, 0);
  }
}
