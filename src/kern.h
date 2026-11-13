#pragma once

#include <stddef.h>

/* Reads our own ki_tdname out of the kernel process table. This is the exact
 * string the console's process list renders, so it settles "is the name right?"
 * without depending on a screenshot. Returns 0 on success, -1 when the kernel
 * does not expose the table (the Linux build, for one). */
int qdl_kernel_name(char *out, size_t cap);

/* Number of processes currently carrying `name`, including this one. */
int qdl_count_named(const char *name);

/* Process id of this image, or -1. */
int qdl_self_pid(void);

/* Parent pid, or -1. */
int qdl_parent_pid(void);
