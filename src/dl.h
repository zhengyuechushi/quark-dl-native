#pragma once

#include <stddef.h>

/* The download queue.
 *
 * One task is one URL to one file. Tasks run in their own detached threads, at
 * most DL_LIMIT at a time, because a console downloading six files at once is
 * slower than three and much harder to explain when it fails.
 *
 * Resuming is the whole point of the file layout: bytes go to "<name>.part"
 * and the file is renamed only after fsync succeeds, so an interrupted download
 * leaves something that can be continued rather than a truncated file that
 * looks complete. A re-added URL whose final file already exists is reported as
 * done instead of downloaded again. */

#define DL_QUEUED 0
#define DL_RUNNING 1
#define DL_DONE 2
#define DL_FAILED 3
#define DL_CANCELED 4
/* Finished once, but the file is no longer on disk. Its own state, because the
 * task did not fail at anything - calling it "失败" makes a user look for a
 * network problem that does not exist. Re-adding the URL queues it again. */
#define DL_MISSING 5
/* Stopped on purpose, keeping the .part file so it can be resumed later. Its
 * own state: a paused task is not "canceled" (the bytes stay) and not "queued"
 * (it must not start on its own until the user says so). */
#define DL_PAUSED 6

#define DL_MAX 64
/* Default number of simultaneous downloads. Tunable at runtime via
 * dl_set_limit / the settings endpoint; 3 at once was more than a console
 * wants, so the default is 2 and the user can pick. */
#define DL_DEFAULT_LIMIT 2

/* Multi-connection segmented download. A Quark CDN throttles a single
 * connection, so one big file crawled at ~100 KB/s while the working exFAT
 * build, which fetches 32 MiB ranges over 8 parallel connections, saturates
 * the link. A file this size or larger is split into SEG_BYTES ranges fetched
 * SEG_CONNS at a time; anything smaller stays on the single-connection path. */
#define SEG_BYTES (32LL * 1024 * 1024)  /* bytes per range */
#define SEG_CONNS 8                     /* parallel connections per file */
#define SEG_MIN_BYTES (2 * SEG_BYTES)   /* split only above this */

/* Loads the persisted queue and settings, creating the data directory. */
int dl_init(const char *data_dir);

/* Where downloads land when the user has not chosen a directory. Persisted, and
 * a real subdirectory rather than /data/homebrew itself: that directory holds
 * the installed payloads, and mixing downloaded films into it makes both lists
 * unreadable. */
#define DL_DEFAULT_DIR "/data/homebrew/Quarkdownload"

/* What 0.12.1 and earlier used. A settings file holding exactly this value is
 * migrated to DL_DEFAULT_DIR once, on load; anything else was picked by the
 * user and is left alone. */
#define DL_LEGACY_DIR "/data/homebrew"

/* Default destination. Persisted; defaults to DL_DEFAULT_DIR. */
const char *dl_default_dir(void);
int dl_set_default_dir(const char *dir, char *err, size_t cap);

/* Queues one URL. `name` and `dir` may be NULL to use the URL's basename and
 * the default directory. Returns the task id, or -1 with err filled in.
 *
 * `fid` is optional: when set (a quark file id rather than a pasted link), the
 * queue stores the fid and mints a fresh direct link when the download actually
 * starts, so a file that waits behind several large downloads is never handed
 * an expired signature. `url` may then be empty.
 *
 * Re-adding a URL that is still queued or running returns its id and changes
 * nothing; re-adding one that has finished, failed or been canceled runs it
 * again, which resumes from the .part file when one is there. */
int dl_add(const char *url, const char *fid, const char *name, const char *dir,
           char *err, size_t cap);

/* The session a quark download has to present, and the last word on it.
 *
 * A CDN-hosted quark link answers HTTP 412 to any request that does not look
 * like the browser download it was minted for, so every quark download carries
 * the client UA, a pan.quark.cn Referer and this cookie. The caller that owns
 * the session (the quark layer) sets it after each batch of links, because
 * quark renews the cookie on exactly that call and the CDN wants the newest
 * value; NULL clears it on logout.
 *
 * The copy is taken here rather than borrowed so the queue never reads a
 * session the API thread is rewriting. */
void dl_set_cookie(const char *cookie);

/* Puts a finished task back in the queue. id < 0 retries every finished task.
 * Returns the number of tasks queued. */
int dl_retry(int id);

/* id < 0 pauses everything running or queued. A paused task keeps its .part
 * file; dl_resume puts it back in the queue. Returns the number touched. */
int dl_pause(int id);
int dl_resume(int id);

/* id < 0 cancels everything. Unlike pause, cancel also removes the task's
 * .part file, so an unfinished download is actually gone, not silently kept.
 * Returns the number of tasks touched. */
int dl_cancel(int id);

/* Concurrent download limit. dl_set_limit clamps to [1, 8]. */
int dl_limit(void);
int dl_set_limit(int n);

/* Removes finished tasks from the list. Returns how many were dropped. */
int dl_clear_finished(void);

int dl_running(void);
int dl_count(void);

/* JSON snapshot. Returns 0, or -1 when the buffer is too small. */
int dl_json(char *out, size_t cap);

/* Basename of a URL, sanitised for use as a file name. Always non-empty. */
void dl_guess_name(const char *url, char *out, size_t cap);

void dl_shutdown(void);
