/* Process-table introspection. Offsets come from the reference payload
 * (owendswang/ps5-web-file-manager, GPL-3.0-or-later), which reads the same
 * fields to find and reap stale instances:
 *
 *   kinfo_proc[0]        ki_structsize
 *   kinfo_proc[72]       ki_pid
 *   kinfo_proc[447]      ki_tdname   <- what the console displays
 */

#include "kern.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#ifdef __SCE__
#include <sys/types.h>
#include <sys/sysctl.h>
#endif

#define KERN_OFF_PID 72
#define KERN_OFF_TDNAME 447
#define KERN_MIN_RECORD 512

#ifdef __SCE__
static uint8_t *process_table(size_t *out_size) {
  int mib[4] = {1, 14, 8, 0};
  size_t size = 0;
  uint8_t *buf;

  if(sysctl(mib, 4, 0, &size, 0, 0) || size < KERN_MIN_RECORD) {
    return 0;
  }
  if(!(buf = malloc(size))) {
    return 0;
  }
  if(sysctl(mib, 4, buf, &size, 0, 0)) {
    free(buf);
    return 0;
  }
  *out_size = size;
  return buf;
}
#endif

int
qdl_kernel_name(char *out, size_t cap) {
  if(!out || cap < 2) {
    return -1;
  }
  out[0] = 0;

#ifdef __SCE__
  size_t size = 0;
  uint8_t *buf = process_table(&size);
  pid_t mypid = getpid();
  int found = -1;

  if(!buf) {
    return -1;
  }
  for(uint8_t *ptr = buf; ptr + KERN_MIN_RECORD <= buf + size;) {
    int record = *(int *)ptr;
    if(record < KERN_MIN_RECORD || ptr + record > buf + size) {
      break;
    }
    if(*(pid_t *)&ptr[KERN_OFF_PID] == mypid) {
      snprintf(out, cap, "%s", (char *)&ptr[KERN_OFF_TDNAME]);
      found = 0;
      break;
    }
    ptr += record;
  }
  free(buf);
  return found;
#else
  snprintf(out, cap, "%s", "quark-dl.elf");
  return 0;
#endif
}

int
qdl_count_named(const char *name) {
#ifdef __SCE__
  size_t size = 0;
  uint8_t *buf = process_table(&size);
  int count = 0;

  if(!buf) {
    return -1;
  }
  for(uint8_t *ptr = buf; ptr + KERN_MIN_RECORD <= buf + size;) {
    int record = *(int *)ptr;
    if(record < KERN_MIN_RECORD || ptr + record > buf + size) {
      break;
    }
    if(!strcmp(name, (char *)&ptr[KERN_OFF_TDNAME])) {
      count++;
    }
    ptr += record;
  }
  free(buf);
  return count;
#else
  (void)name;
  return 1;
#endif
}

int
qdl_self_pid(void) {
  return (int)getpid();
}

int
qdl_parent_pid(void) {
  return (int)getppid();
}
