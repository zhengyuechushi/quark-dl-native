/* A start-up trail on disk.
 *
 * When the console shows no process there is normally nothing to go on: the
 * payload's stdout goes to the loader, and a crash before the first notification
 * produces no notification. This writes each step to a file in the data
 * directory, which the FTP server can read, so "it did not start" can be split
 * into "it never loaded", "it died before serving" and "it is serving on another
 * port".
 *
 * Writes are open/write/close rather than stdio on purpose: a buffered stream
 * would lose exactly the last line before a crash, which is the only line worth
 * having.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 3, or (at your option) any
 * later version.
 */

#ifndef BOOT_H
#define BOOT_H

#include <stddef.h>

/* One line, with a timestamp and the pid. Safe to call before anything else is
 * initialised: it creates the directory if it has to. Never aborts on failure —
 * a payload that cannot write its own log still has work to do. */
void boot_trace(const char *fmt, ...);

/* Called once from main: if the previous run left a large file behind, keep it
 * as boot.log.old so the run before this one is still readable. */
void boot_reset(void);

/* Crash handlers. A segfault otherwise looks the same as "the loader refused
 * the file". Installed from main; not async-perfect, but the only code it runs
 * is a couple of writes. */
void boot_watch_signals(void);

#endif /* BOOT_H */
