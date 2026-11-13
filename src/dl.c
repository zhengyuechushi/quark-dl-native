/* The download queue.
 *
 * Shape of a task on disk: "<dir>/<name>.part" while bytes are still arriving,
 * then a rename to "<dir>/<name>". The rename is the only moment a file becomes
 * "done", so a half-written download can never be mistaken for a complete one,
 * and .part is exactly what a retry resumes from.
 *
 * Retries are per task and capped, because the interesting failures on a
 * console are (a) the link expired, which no amount of retrying fixes, and
 * (b) the connection died mid-file, which one retry fixes. Both are reported
 * with the HTTP status so the answer is visible in the UI instead of inferred
 * from a truncated file. */

#include "dl.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/vfs.h>
#else
#include <sys/mount.h>
#endif

#include "httpc.h"

typedef struct {
  int id; /* 0 means the slot is free */
  int state;
  int attempts;
  volatile int cancel;
  long long done;
  long long total;
  char url[900];
  char dir[300];
  char name[160];
  char err[140];
} dl_task;

static dl_task g_task[DL_MAX];
static int g_count;
static int g_running;
static int g_next_id = 1;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_data[300] = "/data/quark-dl";
static char g_dir[300] = "/data/homebrew";

static void pump_locked(void);
static void save_locked(void);

/* ---------------------------------------------------------------- helpers */

static int
app(char *buf, size_t cap, size_t *used, const char *fmt, ...) {
  va_list ap;
  int n;

  if(*used >= cap) {
    return -1;
  }
  va_start(ap, fmt);
  n = vsnprintf(buf + *used, cap - *used, fmt, ap);
  va_end(ap);
  if(n < 0 || (size_t)n >= cap - *used) {
    return -1;
  }
  *used += (size_t)n;
  return 0;
}

static int
jstr(char *buf, size_t cap, size_t *used, const char *s) {
  if(app(buf, cap, used, "\"") < 0) {
    return -1;
  }
  for(; s && *s; s++) {
    unsigned char c = (unsigned char)*s;

    if(c == '"' || c == '\\') {
      if(app(buf, cap, used, "\\%c", c) < 0) {
        return -1;
      }
    } else if(c < 0x20) {
      if(app(buf, cap, used, " ") < 0) {
        return -1;
      }
    } else if(app(buf, cap, used, "%c", c) < 0) {
      return -1;
    }
  }
  return app(buf, cap, used, "\"");
}

static int
mkdirs(const char *path) {
  char tmp[400];
  size_t i;
  size_t n;

  n = strlen(path);
  if(n == 0 || n >= sizeof(tmp)) {
    return -1;
  }
  memcpy(tmp, path, n + 1);
  for(i = 1; i < n; i++) {
    if(tmp[i] != '/') {
      continue;
    }
    tmp[i] = 0;
    if(mkdir(tmp, 0755) != 0 && errno != EEXIST) {
      return -1;
    }
    tmp[i] = '/';
  }
  if(mkdir(tmp, 0755) != 0 && errno != EEXIST) {
    return -1;
  }
  return 0;
}

static long long
disk_free(const char *dir) {
  struct statfs sfs;

  if(statfs(dir, &sfs) != 0) {
    return -1;
  }
  return (long long)sfs.f_bavail * (long long)sfs.f_bsize;
}

static int
path_join(char *out, size_t cap, const char *dir, const char *name,
          const char *suffix) {
  int n = snprintf(out, cap, "%s/%s%s", dir, name, suffix ? suffix : "");

  return (n < 0 || (size_t)n >= cap) ? -1 : 0;
}

static void
sanitize_name(const char *in, char *out, size_t cap) {
  size_t o = 0;

  for(; in && *in && o + 1 < cap; in++) {
    unsigned char c = (unsigned char)*in;

    if((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
       (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_' ||
       c == '(' || c == ')' || c == ' ' || c >= 0x80) {
      out[o++] = (char)c;
    } else {
      out[o++] = '_';
    }
  }
  while(o && (out[o - 1] == ' ' || out[o - 1] == '.')) {
    o--;
  }
  out[o] = 0;
  if(!o || !strcmp(out, ".") || !strcmp(out, "..")) {
    snprintf(out, cap, "download.bin");
  }
}

void
dl_guess_name(const char *url, char *out, size_t cap) {
  const char *p;
  const char *base;
  const char *end;
  char raw[220];
  size_t n;

  out[0] = 0;
  if(!url) {
    snprintf(out, cap, "download.bin");
    return;
  }
  /* Everything after the last '/' of the path, before '?' or '#'. */
  p = strstr(url, "://");
  p = p ? p + 3 : url;
  base = strchr(p, '/');
  if(!base) {
    snprintf(out, cap, "download.bin");
    return;
  }
  base++;
  end = base + strcspn(base, "?#");
  if(end == base) {
    snprintf(out, cap, "download.bin");
    return;
  }
  n = (size_t)(end - base);
  if(n >= sizeof(raw)) {
    n = sizeof(raw) - 1;
  }
  memcpy(raw, base, n);
  raw[n] = 0;
  sanitize_name(raw, out, cap);
}

/* ------------------------------------------------------------ persistence */

static void
save_locked(void) {
  char path[400];
  char tmp[410];
  FILE *f;
  int i;

  snprintf(tmp, sizeof(tmp), "%s/downloads.txt.tmp", g_data);
  snprintf(path, sizeof(path), "%s/downloads.txt", g_data);
  f = fopen(tmp, "w");
  if(!f) {
    return;
  }
  for(i = 0; i < g_count; i++) {
    dl_task *t = &g_task[i];
    int state;

    if(!t->id) {
      continue;
    }
    /* A task interrupted by a crash comes back as queued, never as running:
     * nothing is downloading it once the process is gone. */
    state = (t->state == DL_RUNNING) ? DL_QUEUED : t->state;
    fprintf(f, "%d|%d|%lld|%lld|%s|%s|%s\n", t->id, state, t->done, t->total,
            t->dir, t->name, t->url);
  }
  if(fclose(f) == 0) {
    rename(tmp, path);
  }
}

static void
load_settings(void) {
  char path[400];
  char line[400];
  FILE *f;

  snprintf(path, sizeof(path), "%s/settings.txt", g_data);
  f = fopen(path, "r");
  if(!f) {
    return;
  }
  while(fgets(line, sizeof(line), f)) {
    char *v;

    line[strcspn(line, "\r\n")] = 0;
    if(!strncmp(line, "dir=", 4) && line[4] == '/') {
      v = line + 4;
      snprintf(g_dir, sizeof(g_dir), "%s", v);
    }
  }
  fclose(f);
}

static void
save_settings(void) {
  char path[400];
  FILE *f;

  snprintf(path, sizeof(path), "%s/settings.txt", g_data);
  f = fopen(path, "w");
  if(!f) {
    return;
  }
  fprintf(f, "dir=%s\n", g_dir);
  fclose(f);
}

static void
load_queue(void) {
  char path[400];
  char line[1600];
  FILE *f;

  snprintf(path, sizeof(path), "%s/downloads.txt", g_data);
  f = fopen(path, "r");
  if(!f) {
    return;
  }
  while(fgets(line, sizeof(line), f) && g_count < DL_MAX) {
    char *field[7];
    char *p = line;
    int i;
    int ok = 1;
    dl_task *t;

    line[strcspn(line, "\r\n")] = 0;
    for(i = 0; i < 6; i++) {
      char *bar = strchr(p, '|');

      if(!bar) {
        ok = 0;
        break;
      }
      *bar = 0;
      field[i] = p;
      p = bar + 1;
    }
    if(!ok) {
      continue;
    }
    field[6] = p;
    if(!field[6][0] || strncmp(field[6], "http", 4)) {
      continue;
    }
    t = &g_task[g_count++];
    memset(t, 0, sizeof(*t));
    t->id = atoi(field[0]);
    t->state = atoi(field[1]);
    t->done = strtoll(field[2], NULL, 10);
    t->total = strtoll(field[3], NULL, 10);
    snprintf(t->dir, sizeof(t->dir), "%s", field[4]);
    snprintf(t->name, sizeof(t->name), "%s", field[5]);
    snprintf(t->url, sizeof(t->url), "%s", field[6]);
    if(t->state == DL_RUNNING || t->state == DL_CANCELED) {
      t->state = DL_QUEUED;
    }
    /* A task whose file is gone is not "done", whatever the queue file says.
     * Saying so at load time keeps the list honest, and it is what gives the
     * "重新下载" button something to mean. */
    if(t->state == DL_DONE) {
      char full[700];
      struct stat st;

      if(path_join(full, sizeof(full), t->dir, t->name, "") != 0 ||
         stat(full, &st) != 0 || st.st_size <= 0) {
        t->state = DL_MISSING;
        t->done = 0;
      }
    }
    if(t->id >= g_next_id) {
      g_next_id = t->id + 1;
    }
  }
  fclose(f);
}

/* ------------------------------------------------------------- the worker */

typedef struct {
  int fd;
  dl_task *t;
  http_result *res;
  long long have;
  int first;
} sink_ctx;

static int
sink_write(void *ctx, const void *data, size_t len) {
  sink_ctx *s = (sink_ctx *)ctx;
  const char *p = (const char *)data;

  /* An error page is not file content. Writing it would leave "gone" or an
   * XML error document at the head of a .part file, and a later resume would
   * happily append the real bytes behind it. */
  if(s->res->status >= 300) {
    return 0;
  }
  if(s->first) {
    s->first = 0;
    /* 200 to a ranged request means the server ignored Range. Truncating now
     * beats appending a second copy of the file onto the .part. */
    if(s->res->status == 200 && s->have > 0) {
      if(ftruncate(s->fd, 0) != 0 || lseek(s->fd, 0, SEEK_SET) < 0) {
        return -1;
      }
      s->t->done = 0;
    }
  }
  while(len) {
    ssize_t n = write(s->fd, p, len);

    if(n < 0) {
      if(errno == EINTR) {
        continue;
      }
      return -1;
    }
    p += n;
    len -= (size_t)n;
    s->t->done += (long long)n;
  }
  return 0;
}

/* 0 done, -1 retry, -2 cancelled, -3 permanent. */
static int
dl_attempt(dl_task *t) {
  char part[520];
  char final[520];
  struct stat st;
  sink_ctx sink;
  http_result res;
  http_req req;
  long long have = 0;
  long long free_bytes;
  int fd;

  if(path_join(part, sizeof(part), t->dir, t->name, ".part") ||
     path_join(final, sizeof(final), t->dir, t->name, "")) {
    snprintf(t->err, sizeof(t->err), "目标路径过长");
    return -3;
  }
  if(stat(final, &st) == 0 && st.st_size > 0) {
    t->done = (long long)st.st_size;
    t->total = t->done;
    snprintf(t->err, sizeof(t->err), "文件已存在，跳过");
    return 0;
  }
  if(stat(part, &st) == 0 && st.st_size > 0) {
    have = (long long)st.st_size;
  }
  free_bytes = disk_free(t->dir);
  if(free_bytes >= 0 && free_bytes < (64LL << 20)) {
    snprintf(t->err, sizeof(t->err), "剩余空间不足 64 MB（%lld MB）",
             free_bytes >> 20);
    return -3;
  }

  fd = open(part, O_WRONLY | O_CREAT, 0644);
  if(fd < 0) {
    snprintf(t->err, sizeof(t->err), "无法写入 %s: %s", part, strerror(errno));
    return -3;
  }
  if(lseek(fd, have, SEEK_SET) < 0) {
    snprintf(t->err, sizeof(t->err), "定位失败: %s", strerror(errno));
    close(fd);
    return -3;
  }

  memset(&sink, 0, sizeof(sink));
  sink.fd = fd;
  sink.t = t;
  sink.res = &res;
  sink.have = have;
  sink.first = 1;

  memset(&req, 0, sizeof(req));
  req.url = t->url;
  req.range_from = have;
  req.write = sink_write;
  req.write_ctx = &sink;
  req.abort = &t->cancel;
  req.timeout_ms = 15000;

  t->done = have;
  if(http_perform(&req, &res) < 0) {
    close(fd);
    snprintf(t->err, sizeof(t->err), "%s", res.err[0] ? res.err : "传输失败");
    return t->cancel ? -2 : -1;
  }
  if(res.total > 0) {
    t->total = res.total;
  } else if(res.length > 0) {
    t->total = res.length;
  }
  if(res.status == 416) {
    /* The .part already holds everything the server has. */
    close(fd);
    if(rename(part, final) == 0) {
      t->done = t->total > 0 ? t->total : have;
      t->err[0] = 0;
      return 0;
    }
    snprintf(t->err, sizeof(t->err), "重命名失败: %s", strerror(errno));
    return -1;
  }
  if(res.status == 400 || res.status == 401 || res.status == 403 ||
     res.status == 404 || res.status == 410) {
    close(fd);
    snprintf(t->err, sizeof(t->err), "HTTP %d：链接无效或已过期", res.status);
    return -3;
  }
  if(res.status != 200 && res.status != 206) {
    close(fd);
    snprintf(t->err, sizeof(t->err), "HTTP %d", res.status);
    return -1;
  }
  if(fsync(fd) != 0) {
    int saved = errno;

    close(fd);
    snprintf(t->err, sizeof(t->err), "落盘失败: %s", strerror(saved));
    return -1;
  }
  if(close(fd) != 0) {
    snprintf(t->err, sizeof(t->err), "关闭文件失败: %s", strerror(errno));
    return -1;
  }
  if(rename(part, final) != 0) {
    snprintf(t->err, sizeof(t->err), "重命名失败: %s", strerror(errno));
    return -1;
  }
  if(t->total <= 0) {
    t->total = t->done;
  }
  t->err[0] = 0;
  return 0;
}

static void *
dl_worker(void *arg) {
  dl_task *t = (dl_task *)arg;
  int rc = -1;
  int attempt;

  for(attempt = 1; attempt <= 4; attempt++) {
    pthread_mutex_lock(&g_lock);
    t->attempts = attempt;
    pthread_mutex_unlock(&g_lock);

    rc = dl_attempt(t);
    if(rc == 0 || rc == -2 || rc == -3) {
      break;
    }
    if(t->cancel) {
      rc = -2;
      break;
    }
    if(attempt < 4) {
      sleep((unsigned)(1u << (unsigned)(attempt - 1)));
    }
  }

  pthread_mutex_lock(&g_lock);
  if(t->cancel || rc == -2) {
    t->state = DL_CANCELED;
    if(!t->err[0]) {
      snprintf(t->err, sizeof(t->err), "已取消");
    }
  } else if(rc == 0) {
    t->state = DL_DONE;
  } else {
    t->state = DL_FAILED;
    if(!t->err[0]) {
      snprintf(t->err, sizeof(t->err), "失败（%d 次尝试）", t->attempts);
    }
  }
  if(g_running > 0) {
    g_running--;
  }
  pump_locked();
  save_locked();
  pthread_mutex_unlock(&g_lock);
  return NULL;
}

static void
pump_locked(void) {
  int i;

  for(i = 0; i < g_count && g_running < DL_LIMIT; i++) {
    dl_task *t = &g_task[i];
    pthread_t th;

    if(!t->id || t->state != DL_QUEUED) {
      continue;
    }
    t->cancel = 0;
    t->attempts = 0;
    t->err[0] = 0;
    t->state = DL_RUNNING;
    if(pthread_create(&th, NULL, dl_worker, t) != 0) {
      t->state = DL_FAILED;
      snprintf(t->err, sizeof(t->err), "无法创建下载线程");
      continue;
    }
    pthread_detach(th);
    g_running++;
  }
}

/* ------------------------------------------------------------ public API */

int
dl_init(const char *data_dir) {
  const char *override;

  if(data_dir && *data_dir) {
    snprintf(g_data, sizeof(g_data), "%s", data_dir);
  }
  mkdirs(g_data);

  override = getenv("QDL_DEFAULT_DIR");
  if(override && override[0] == '/') {
    snprintf(g_dir, sizeof(g_dir), "%s", override);
  }
  load_settings();
  load_queue();
  mkdirs(g_dir);

  pthread_mutex_lock(&g_lock);
  pump_locked();
  pthread_mutex_unlock(&g_lock);
  return 0;
}

const char *
dl_default_dir(void) {
  return g_dir;
}

int
dl_set_default_dir(const char *dir, char *err, size_t cap) {
  if(!dir || dir[0] != '/') {
    snprintf(err, cap, "目录必须是绝对路径");
    return -1;
  }
  if(mkdirs(dir) != 0) {
    snprintf(err, cap, "无法创建目录 %s: %s", dir, strerror(errno));
    return -1;
  }
  if(access(dir, W_OK) != 0) {
    snprintf(err, cap, "目录不可写 %s: %s", dir, strerror(errno));
    return -1;
  }
  pthread_mutex_lock(&g_lock);
  snprintf(g_dir, sizeof(g_dir), "%s", dir);
  save_settings();
  pthread_mutex_unlock(&g_lock);
  return 0;
}

int
dl_add(const char *url, const char *name, const char *dir, char *err,
       size_t cap) {
  char clean[160];
  dl_task *t = NULL;
  int i;
  int id;

  if(!url || !*url) {
    snprintf(err, cap, "URL 为空");
    return -1;
  }
  if(strncmp(url, "http://", 7) && strncmp(url, "https://", 8)) {
    snprintf(err, cap, "只支持 http:// 或 https:// 链接");
    return -1;
  }
  if(name && *name) {
    sanitize_name(name, clean, sizeof(clean));
  } else {
    dl_guess_name(url, clean, sizeof(clean));
  }

  /* The worker starts inside dl_add, so the directory has to exist first or
   * the first open() fails for a reason that has nothing to do with the URL. */
  {
    const char *target = (dir && dir[0] == '/') ? dir : g_dir;

    if(mkdirs(target) != 0) {
      snprintf(err, cap, "无法创建目录 %s: %s", target, strerror(errno));
      return -1;
    }
  }

  pthread_mutex_lock(&g_lock);
  for(i = 0; i < g_count; i++) {
    dl_task *e = &g_task[i];

    if(!e->id || strcmp(e->url, url)) {
      continue;
    }
    /* Already waiting or in flight: hand back the same task. Adding it twice
     * would only download the same bytes twice. */
    if(e->state == DL_QUEUED || (e->state == DL_RUNNING && !e->cancel)) {
      id = e->id;
      pthread_mutex_unlock(&g_lock);
      return id;
    }
    if(e->state == DL_RUNNING) {
      /* Canceled a moment ago and the worker has not noticed yet. Clearing the
       * flag lets it carry on instead of bouncing the user off with "it is
       * still there" for a task that is visibly stopping. */
      e->cancel = 0;
      id = e->id;
      pthread_mutex_unlock(&g_lock);
      return id;
    }
    /* Done, failed or canceled: "add this link again" means run it again, not
     * "there it is, still finished". The .part file, if one survived, is what
     * turns that into a resume instead of a restart; if the finished file is
     * still on disk dl_attempt reports it as already there and nothing is
     * transferred. */
    e->state = DL_QUEUED;
    e->cancel = 0;
    e->attempts = 0;
    e->done = 0;
    e->err[0] = 0;
    if(name && *name) {
      snprintf(e->name, sizeof(e->name), "%s", clean);
    }
    if(dir && dir[0] == '/') {
      snprintf(e->dir, sizeof(e->dir), "%s", dir);
    }
    id = e->id;
    pump_locked();
    save_locked();
    pthread_mutex_unlock(&g_lock);
    return id;
  }
  for(i = 0; i < g_count; i++) {
    if(!g_task[i].id) {
      t = &g_task[i];
      break;
    }
  }
  if(!t) {
    if(g_count >= DL_MAX) {
      pthread_mutex_unlock(&g_lock);
      snprintf(err, cap, "队列已满（最多 %d 个）", DL_MAX);
      return -1;
    }
    t = &g_task[g_count++];
  }
  memset(t, 0, sizeof(*t));
  t->id = g_next_id++;
  t->state = DL_QUEUED;
  snprintf(t->url, sizeof(t->url), "%s", url);
  snprintf(t->name, sizeof(t->name), "%s", clean);
  if(dir && dir[0] == '/') {
    snprintf(t->dir, sizeof(t->dir), "%s", dir);
  } else {
    snprintf(t->dir, sizeof(t->dir), "%s", g_dir);
  }
  id = t->id;
  pump_locked();
  save_locked();
  pthread_mutex_unlock(&g_lock);
  return id;
}

int
dl_cancel(int id) {
  int i;
  int n = 0;

  pthread_mutex_lock(&g_lock);
  for(i = 0; i < g_count; i++) {
    dl_task *t = &g_task[i];

    if(!t->id || (id >= 0 && t->id != id)) {
      continue;
    }
    t->cancel = 1;
    if(t->state == DL_QUEUED) {
      t->state = DL_CANCELED;
      snprintf(t->err, sizeof(t->err), "已取消");
    }
    n++;
  }
  save_locked();
  pthread_mutex_unlock(&g_lock);
  return n;
}

int
dl_retry(int id) {
  int i;
  int n = 0;

  pthread_mutex_lock(&g_lock);
  for(i = 0; i < g_count; i++) {
    dl_task *t = &g_task[i];

    if(!t->id || (id >= 0 && t->id != id)) {
      continue;
    }
    if(t->state == DL_QUEUED || t->state == DL_RUNNING) {
      continue;
    }
    t->state = DL_QUEUED;
    t->cancel = 0;
    t->attempts = 0;
    t->done = 0;
    t->err[0] = 0;
    n++;
  }
  if(n) {
    pump_locked();
  }
  save_locked();
  pthread_mutex_unlock(&g_lock);
  return n;
}

int
dl_clear_finished(void) {
  int i;
  int n = 0;

  pthread_mutex_lock(&g_lock);
  for(i = 0; i < g_count; i++) {
    dl_task *t = &g_task[i];

    if(!t->id) {
      continue;
    }
    if(t->state == DL_DONE || t->state == DL_FAILED || t->state == DL_CANCELED ||
       t->state == DL_MISSING) {
      t->id = 0;
      n++;
    }
  }
  save_locked();
  pthread_mutex_unlock(&g_lock);
  return n;
}

int
dl_running(void) {
  int n;

  pthread_mutex_lock(&g_lock);
  n = g_running;
  pthread_mutex_unlock(&g_lock);
  return n;
}

int
dl_count(void) {
  int i;
  int n = 0;

  pthread_mutex_lock(&g_lock);
  for(i = 0; i < g_count; i++) {
    if(g_task[i].id) {
      n++;
    }
  }
  pthread_mutex_unlock(&g_lock);
  return n;
}

static const char *
state_name(int state) {
  switch(state) {
    case DL_QUEUED: return "queued";
    case DL_RUNNING: return "running";
    case DL_DONE: return "done";
    case DL_FAILED: return "failed";
    case DL_CANCELED: return "canceled";
    case DL_MISSING: return "missing";
    default: return "unknown";
  }
}

int
dl_json(char *out, size_t cap) {
  size_t used = 0;
  int i;
  int first = 1;

  if(app(out, cap, &used, "{\"dir\":") < 0 ||
     jstr(out, cap, &used, g_dir) < 0 ||
     app(out, cap, &used, ",\"limit\":%d,\"running\":%d,\"tasks\":[", DL_LIMIT,
         g_running) < 0) {
    return -1;
  }
  pthread_mutex_lock(&g_lock);
  for(i = 0; i < g_count; i++) {
    dl_task *t = &g_task[i];
    int exists = -1;

    if(!t->id) {
      continue;
    }
    if(!first && app(out, cap, &used, ",") < 0) {
      pthread_mutex_unlock(&g_lock);
      return -1;
    }
    first = 0;
    /* Only a finished task gets asked about its file; the answer is what the
     * page uses to decide between "已完成" and "重新下载". A task that already
     * knows its file is gone reports false without another stat. */
    if(t->state == DL_DONE || t->state == DL_MISSING) {
      char full[700];
      struct stat st;

      exists = (path_join(full, sizeof(full), t->dir, t->name, "") == 0 &&
                stat(full, &st) == 0 && st.st_size > 0)
                 ? 1
                 : 0;
    }
    if(app(out, cap, &used,
           "{\"id\":%d,\"state\":\"%s\",\"attempts\":%d,\"done\":%lld,"
           "\"total\":%lld,\"exists\":%s,\"name\":",
           t->id, state_name(t->state), t->attempts, t->done, t->total,
           exists < 0 ? "null" : (exists ? "true" : "false")) < 0 ||
       jstr(out, cap, &used, t->name) < 0 ||
       app(out, cap, &used, ",\"dir\":") < 0 ||
       jstr(out, cap, &used, t->dir) < 0 ||
       app(out, cap, &used, ",\"url\":") < 0 ||
       jstr(out, cap, &used, t->url) < 0 ||
       app(out, cap, &used, ",\"err\":") < 0 ||
       jstr(out, cap, &used, t->err) < 0 ||
       app(out, cap, &used, "}") < 0) {
      pthread_mutex_unlock(&g_lock);
      return -1;
    }
  }
  pthread_mutex_unlock(&g_lock);
  return app(out, cap, &used, "]}");
}

void
dl_shutdown(void) {
  int i;

  pthread_mutex_lock(&g_lock);
  for(i = 0; i < g_count; i++) {
    if(g_task[i].id && g_task[i].state == DL_RUNNING) {
      g_task[i].cancel = 1;
    }
  }
  save_locked();
  pthread_mutex_unlock(&g_lock);
  /* Workers notice the flag between reads and leave within a read timeout. */
  for(i = 0; i < 20; i++) {
    if(!dl_running()) {
      break;
    }
    usleep(100 * 1000);
  }
}
