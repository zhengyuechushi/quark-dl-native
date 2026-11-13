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

#define DL_MAX 64
#define DL_LIMIT 3

/* Loads the persisted queue and settings, creating the data directory. */
int dl_init(const char *data_dir);

/* Default destination. Persisted; defaults to /data/homebrew, the location
 * ProsperoStore installs to, so a user finds downloads where they expect. */
const char *dl_default_dir(void);
int dl_set_default_dir(const char *dir, char *err, size_t cap);

/* Queues one URL. `name` and `dir` may be NULL to use the URL's basename and
 * the default directory. Returns the task id, or -1 with err filled in.
 *
 * Re-adding a URL that is still queued or running returns its id and changes
 * nothing; re-adding one that has finished, failed or been canceled runs it
 * again, which resumes from the .part file when one is there. */
int dl_add(const char *url, const char *name, const char *dir, char *err,
           size_t cap);

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

/* id < 0 cancels everything. Returns the number of tasks touched. */
int dl_cancel(int id);

/* Removes finished tasks from the list. Returns how many were dropped. */
int dl_clear_finished(void);

int dl_running(void);
int dl_count(void);

/* JSON snapshot. Returns 0, or -1 when the buffer is too small. */
int dl_json(char *out, size_t cap);

/* Basename of a URL, sanitised for use as a file name. Always non-empty. */
void dl_guess_name(const char *url, char *out, size_t cap);

void dl_shutdown(void);
