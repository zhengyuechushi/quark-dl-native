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
#include <time.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/vfs.h>
#else
#include <sys/mount.h>
#endif

#include "boot.h"
#include "httpc.h"
#include "quark.h"

typedef struct {
  int id; /* 0 means the slot is free */
  int state;
  int attempts;
  volatile int cancel;
  volatile int pause;
  long long done;
  long long total;
  /* Speed sampling: the worker records how many bytes it has written and when,
   * and dl_json turns the delta into bytes/second. A task that is not running
   * reports 0. */
  long long sample_done;
  long long sample_ms;
  char url[QUARK_URL_MAX];
  char dir[300];
  /* A name can now be a relative path ("剧集/第01话.mkv") so the download
   * recreates the folder tree it came from, not a bare file name. */
  char name[512];
  char err[140];
} dl_task;

static dl_task g_task[DL_MAX];
static int g_count;
static int g_running;
static int g_limit = DL_DEFAULT_LIMIT;
static int g_next_id = 1;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_data[300] = "/data/quark-dl";
static char g_dir[300] = "/data/homebrew";

/* "Referer: ...\r\nCookie: ...\r\n" for quark downloads, empty when nobody has
 * logged in. Written by the API thread, read by the download threads, hence the
 * queue's lock; a whole session is ~300 bytes in practice, so the copy that a
 * task takes is cheap next to the file it is about to fetch. */
static char g_hdr[QUARK_COOKIE_MAX + 96];

static void pump_locked(void);
static void save_locked(void);

/* ---------------------------------------------------------------- helpers */

/* Monotonic milliseconds, used to turn a byte delta into a speed. */
static long long
now_ms(void) {
  struct timespec ts;

  if(clock_gettime(CLOCK_MONOTONIC, &ts)) {
    return 0;
  }
  return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

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
  /* Sized for a download directory (300) plus a nested relative path (512). */
  char tmp[1024];
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

/* A link is not a public file: the CDN behind it repeats the anti-hotlink
 * checks of the browser download it was minted for, and answers HTTP 412 to a
 * request that carries none of them. Hence the client UA, a pan.quark.cn
 * Referer and the session cookie on every quark download.
 *
 * The cookie is a login, so it only travels to quark's own hosts and to this
 * machine. /api/dl/add accepts a URL from anyone who can reach the port, and an
 * unconditional header would turn that into a way to read the session off your
 * own server. Loopback is trusted because that is where the download tests
 * point their mock server. */
static int
quark_host(const char *url) {
  int tls = 0;
  unsigned short port = 0;
  char host[200];
  /* Only the host is read here, but the parse still has to walk the whole URL,
   * so the buffer must be able to take the whole query: a link too long for it
   * would otherwise be mistaken for "not a quark host" and the download would
   * go out with no cookie at all. */
  char path[QUARK_URL_MAX];

  if(!url ||
     http_url_parse(url, &tls, host, sizeof(host), &port, path, sizeof(path))) {
    return 0;
  }
  if(strstr(host, "quark")) {
    return 1;
  }
  return !strcmp(host, "127.0.0.1") || !strcmp(host, "localhost") ||
         !strcmp(host, "::1");
}

/* The header block the API thread last published, copied under the lock so a
 * download that takes a minute never holds it. */
static void
hdr_snapshot(char *out, size_t cap) {
  pthread_mutex_lock(&g_lock);
  snprintf(out, cap, "%s", g_hdr);
  pthread_mutex_unlock(&g_lock);
}

/* Cleans a file name, and now a relative path too. A quark folder can hand the
 * queue a name like "剧集/第01话.mkv": the '/' is a real separator that must
 * survive so the download recreates the folder tree on disk, not a character to
 * mangle. So each segment is cleaned on its own, slashes are kept, and the two
 * things that would let a name escape the download directory are stripped:
 * a leading '/' (absolute path) and any ".." segment (parent traversal). */
static void
sanitize_name(const char *in, char *out, size_t cap) {
  size_t o = 0;
  int seg_start = 1; /* beginning of a segment: nothing written for it yet */

  for(; in && *in && o + 1 < cap; in++) {
    unsigned char c = (unsigned char)*in;

    if(c == '/') {
      /* Collapse runs and drop a trailing slash; a lone '/' becomes a segment
       * boundary, never a path that starts at the filesystem root. */
      if(o && out[o - 1] != '/') {
        out[o++] = '/';
      }
      seg_start = 1;
      continue;
    }
    if((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
       (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_' ||
       c == '(' || c == ')' || c == ' ' || c >= 0x80) {
      if(seg_start && (c == ' ' || c == '.')) {
        /* A segment must not begin with a space or a dot: a dot-prefixed name
         * is how "." and ".." hide, and a leading space is what trips up a
         * rename later. Skip it. */
        continue;
      }
      out[o++] = (char)c;
      seg_start = 0;
    } else {
      if(seg_start) {
        /* Drop a character that would only pad the front of a segment. */
        continue;
      }
      out[o++] = '_';
    }
  }
  /* Trim spaces/dots off the very end, then collapse any trailing slash so the
   * name never ends with one. */
  while(o && (out[o - 1] == ' ' || out[o - 1] == '.')) {
    o--;
  }
  while(o > 1 && out[o - 1] == '/') {
    o--;
  }
  out[o] = 0;

  /* A ".." segment anywhere in the path would climb out of the download
   * directory, and "." is a no-op segment. Replace each with "_" in place. */
  {
    char *seg = out;
    for(;;) {
      char *slash = strchr(seg, '/');
      size_t seglen = slash ? (size_t)(slash - seg) : strlen(seg);

      if((seglen == 2 && seg[0] == '.' && seg[1] == '.') ||
         (seglen == 1 && seg[0] == '.')) {
        memset(seg, '_', seglen);
      }
      if(!slash) {
        break;
      }
      seg = slash + 1;
    }
  }

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
  /* id|state|done|total|dir|name|url. The url is the long field, and the line
   * has to hold it whole: a link read back short is a broken signature that
   * only shows up on the first attempt after a restart. */
  char line[QUARK_URL_MAX + 700];
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
      char full[1024];
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
  /* The first bytes of a non-success body. Discarded either way - an error page
   * is not file content - but kept long enough to be written to boot.log,
   * because a CDN's refusal explains itself in those bytes ("hotlink denied",
   * a JSON code) and "HTTP 412" alone is a dead end. */
  char errbody[220];
  size_t errlen;
} sink_ctx;

static int
sink_write(void *ctx, const void *data, size_t len) {
  sink_ctx *s = (sink_ctx *)ctx;
  const char *p = (const char *)data;

  /* An error page is not file content. Writing it would leave "gone" or an
   * XML error document at the head of a .part file, and a later resume would
   * happily append the real bytes behind it. */
  if(s->res->status >= 300) {
    size_t room = sizeof(s->errbody) - 1 - s->errlen;

    if(room > 0) {
      size_t n = len < room ? len : room;

      memcpy(s->errbody + s->errlen, data, n);
      s->errlen += n;
      s->errbody[s->errlen] = 0;
    }
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
    __atomic_fetch_add(&s->t->done, (long long)n, __ATOMIC_RELAXED);
  }
  return 0;
}

/* --------------------------------------------------- segmented (multi-conn)
 *
 * A single connection is throttled by the CDN, so a big file is split into
 * fixed-size ranges and each range is fetched over its own connection, several
 * at a time, written to its offset with pwrite. This is the shape the working
 * exFAT build's native downloader uses (32 MiB ranges, 8 connections) and the
 * only thing that makes a multi-gigabyte file hit full speed on the console.
 *
 * A range is retried on its own: one slow or refused range must not tear down
 * the seven that are healthy. The whole file is pre-sized with ftruncate so the
 * holes that a yet-unwritten range leaves are bounded and the final size is
 * right; on a filesystem without sparse support that allocation is done once,
 * not per range. */

typedef struct seg_pool seg_pool;

typedef struct {
  dl_task *t;          /* owning task, for done/cancel and the shared .part fd */
  int fd;              /* shared, open .part descriptor (pwrite is atomic-ish) */
  long long begin;     /* inclusive first byte of this range */
  long long end;       /* inclusive last byte of this range */
  long long offset;    /* next write offset within the range (== bytes written) */
  http_result res;     /* this range's response */
  int errbody_len;
  char errbody[220];
  char url[QUARK_URL_MAX];
  char hdrs[QUARK_COOKIE_MAX + 96];
  int isq;
  seg_pool *pool;      /* shared coordinator, for the in-flight counter */
} seg_range;

/* Sink for one range: pwrite at begin+offset. A non-success body is not file
 * content, so it is discarded (and a sliver kept for the error message), the
 * same rule the single-connection sink follows. */
static int
seg_write(void *ctx, const void *data, size_t len) {
  seg_range *r = (seg_range *)ctx;
  const char *p = (const char *)data;

  if(r->res.status >= 300) {
    size_t room = sizeof(r->errbody) - 1 - (size_t)r->errbody_len;

    if(room > 0) {
      size_t n = len < room ? len : room;

      memcpy(r->errbody + r->errbody_len, data, n);
      r->errbody_len += (int)n;
      r->errbody[r->errbody_len] = 0;
    }
    return 0;
  }
  /* 200 means the server ignored Range and sent the whole file: writing it at
   * this range's offset would corrupt every other range's bytes. Refuse it and
   * let seg_fetch turn the status into a permanent failure. */
  if(r->res.status == 200) {
    return -1;
  }
  while(len) {
    ssize_t n = pwrite(r->fd, p, len, (off_t)(r->begin + r->offset));

    if(n < 0) {
      if(errno == EINTR) {
        continue;
      }
      return -1;
    }
    p += n;
    len -= (size_t)n;
    r->offset += (long long)n;
    /* Several ranges write at once, so the shared byte counter is updated
     * atomically; a plain += would race the other range threads and the API
     * thread that samples it for the speed readout. */
    __atomic_fetch_add(&r->t->done, (long long)n, __ATOMIC_RELAXED);
  }
  return 0;
}

/* Downloads one range to completion. Returns 0 on success, -1 to retry, -2
 * cancelled, -3 permanent. The retry loop lives in the caller so a retry can
 * re-open a fresh connection and a fresh signed URL. */
static int
seg_fetch(seg_range *r) {
  http_req req;
  http_result *res = &r->res;

  memset(res, 0, sizeof(*res));
  memset(&req, 0, sizeof(req));
  req.url = r->url;
  req.range_from = r->begin + r->offset;
  req.range_end = r->end;
  req.write = seg_write;
  req.write_ctx = r;
  req.abort = &r->t->cancel;
  req.timeout_ms = 15000;
  if(r->isq) {
    req.user_agent = QUARK_DL_UA;
    if(r->hdrs[0]) {
      req.extra_headers = r->hdrs;
    }
  }
  if(http_perform(&req, res) < 0) {
    /* A 200 slipped through the sink's refusal (server ignored Range and sent
     * the whole file): that is permanent for a segmented range, not a retry. */
    if(res->status == 200) {
      return -3;
    }
    snprintf(r->t->err, sizeof(r->t->err), "%s",
             res->err[0] ? res->err : "传输失败");
    return r->t->cancel ? -2 : -1;
  }
  if(res->status == 416) {
    /* This range is already complete on the server's view; nothing to fetch. */
    return 0;
  }
  if(res->status == 400 || res->status == 401 || res->status == 403 ||
     res->status == 404 || res->status == 410 || res->status == 412) {
    if(res->status == 412) {
      snprintf(r->t->err, sizeof(r->t->err),
               "HTTP 412：CDN 拒绝，请回网盘页重新加入（会换新直链）");
    } else if(res->status == 403 && strstr(r->errbody, "SignatureDoesNotMatch")) {
      snprintf(r->t->err, sizeof(r->t->err),
               "HTTP 403：直链签名不匹配，请回网盘页重新加入（会换新直链）");
    } else {
      snprintf(r->t->err, sizeof(r->t->err), "HTTP %d：链接无效或已过期",
               res->status);
    }
    return -3;
  }
  if(res->status != 200 && res->status != 206) {
    snprintf(r->t->err, sizeof(r->t->err), "HTTP %d", res->status);
    return res->status == 412 ? -3 : -1;
  }
  /* 200 means the server ignored Range: the whole file came back, so this
   * range is a lost cause only if the server really ignored it (a single range
   * request answered 200 is the whole file, not this slice). */
  if(res->status == 200) {
    return -3;
  }
  return 0;
}

/* A range thread. Runs seg_fetch, retries up to 4 times, then marks itself done
 * and signals the coordinator. */
typedef struct {
  seg_range r;
  int rc;
  pthread_t th;
  volatile int done;    /* 1 once the thread has recorded its result */
} seg_thread;

/* Shared coordinator state, one per segmented download. */
typedef struct seg_pool {
  pthread_mutex_t lock;
  pthread_cond_t cond;
  int in_flight;        /* threads running, guarded by lock */
  int cancelled;        /* set when the whole file is cancelled */
} seg_pool;

static void *
seg_worker(void *arg) {
  seg_thread *st = (seg_thread *)arg;
  seg_range *r = &st->r;
  seg_pool *pool = (seg_pool *)r->pool;
  int attempt;

  st->rc = -1;
  for(attempt = 1; attempt <= 4; attempt++) {
    st->rc = seg_fetch(r);
    if(st->rc == 0 || st->rc == -2 || st->rc == -3) {
      break;
    }
    if(r->t->cancel) {
      st->rc = -2;
      break;
    }
    if(attempt < 4) {
      sleep((unsigned)(1u << (unsigned)(attempt - 1)));
    }
  }
  pthread_mutex_lock(&pool->lock);
  pool->in_flight--;
  st->done = 1;
  pthread_cond_signal(&pool->cond);
  pthread_mutex_unlock(&pool->lock);
  return NULL;
}

/* Segmented download of one file. Returns the same codes as dl_attempt.
 * `have` is the number of contiguous bytes already on disk; ranges wholly below
 * it are skipped, and the first partial range resumes from its offset. */
static int
dl_segmented(dl_task *t, int fd, long long have, long long total) {
  long long span = total - have;
  int n = (int)((span + SEG_BYTES - 1) / SEG_BYTES);
  int next = 0;           /* next range index to launch */
  int launched = 0;       /* ranges whose thread was started */
  int result = 0;         /* aggregate: 0 ok, -2 cancelled, -3 permanent */
  int i;
  seg_pool pool;
  seg_thread *st;
  char hdrs[QUARK_COOKIE_MAX + 96];
  int isq;

  if(n <= 0) {
    return 0;
  }
  if(n > 4096) {
    snprintf(t->err, sizeof(t->err), "文件分片过多");
    return -3;
  }

  /* Pre-size the file so pwrite to a high offset cannot run off the end and the
   * final size is right even while the tail ranges are still unwritten. */
  if(ftruncate(fd, (off_t)total) != 0) {
    snprintf(t->err, sizeof(t->err), "预分配失败: %s", strerror(errno));
    return -3;
  }

  hdr_snapshot(hdrs, sizeof(hdrs));
  isq = quark_host(t->url);

  st = (seg_thread *)calloc((size_t)n, sizeof(*st));
  if(!st) {
    snprintf(t->err, sizeof(t->err), "内存不足");
    return -3;
  }
  pthread_mutex_init(&pool.lock, NULL);
  pthread_cond_init(&pool.cond, NULL);
  pool.in_flight = 0;
  pool.cancelled = 0;

  boot_trace("下载 %s：分片 %d 片（每片 %lld 字节），续传 %lld 字节，登录头=%s",
             t->name, n, SEG_BYTES, have,
             isq ? (hdrs[0] ? "是" : "否（没有会话 cookie）") : "不需要");

  /* Launch ranges, at most SEG_CONNS in flight, until all are started. When the
   * pool is full, wait for one to finish before launching the next. */
  while(next < n) {
    long long begin;
    long long end;
    seg_range *r;
    seg_thread *s;

    pthread_mutex_lock(&pool.lock);
    while(pool.in_flight >= SEG_CONNS && !t->cancel) {
      pthread_cond_wait(&pool.cond, &pool.lock);
    }
    if(t->cancel) {
      pool.cancelled = 1;
      pthread_mutex_unlock(&pool.lock);
      break;
    }
    pthread_mutex_unlock(&pool.lock);

    begin = have + (long long)next * SEG_BYTES;
    end = begin + SEG_BYTES - 1;
    if(end >= total) {
      end = total - 1;
    }
    if(begin > end) {
      next++;
      continue;
    }
    s = &st[next];
    memset(s, 0, sizeof(*s));
    r = &s->r;
    r->t = t;
    r->fd = fd;
    r->begin = begin;
    r->end = end;
    r->offset = 0;
    r->pool = &pool;
    snprintf(r->url, sizeof(r->url), "%s", t->url);
    snprintf(r->hdrs, sizeof(r->hdrs), "%s", hdrs);
    r->isq = isq;

    pthread_mutex_lock(&pool.lock);
    if(pool.cancelled) {
      pthread_mutex_unlock(&pool.lock);
      next++;
      continue;
    }
    pool.in_flight++;
    pthread_mutex_unlock(&pool.lock);

    if(pthread_create(&s->th, NULL, seg_worker, s) != 0) {
      pthread_mutex_lock(&pool.lock);
      pool.in_flight--;
      pthread_mutex_unlock(&pool.lock);
      snprintf(t->err, sizeof(t->err), "无法创建分片线程");
      result = -3;
      break;
    }
    launched++;
    next++;
  }

  /* Join every launched thread and fold its result. */
  for(i = 0; i < launched; i++) {
    seg_thread *s = &st[i];

    pthread_join(s->th, NULL);
    if(s->rc == -2) {
      result = -2;
    } else if(s->rc == -3 && result == 0) {
      result = -3;
    } else if(s->rc == -1 && result == 0) {
      /* A range that exhausted its retries but was not cancelled or refused is
       * a transport failure; retry the whole file once at the dl_attempt level. */
      result = -1;
    }
  }

  free(st);
  pthread_cond_destroy(&pool.cond);
  pthread_mutex_destroy(&pool.lock);
  return result;
}

/* 0 done, -1 retry, -2 cancelled, -3 permanent. */

/* Asks the server for the file's total size without downloading the file. A
 * one-byte Range probe learns the size from Content-Range and proves the server
 * honours ranges; the sink aborts after the first byte so a server that ignores
 * Range (200 + whole file) is cut off instead of streamed. Returns 0 when the
 * server ranges (206), 1 when it does not (200), or a negative code on failure.
 * On 0/1, *total holds the full size. */
static int
probe_abort(void *ctx, const void *data, size_t len) {
  (void)ctx;
  (void)data;
  (void)len;
  /* One byte is enough to have learned the size from the headers. */
  return 1;
}

static int
dl_probe_total(dl_task *t, const char *hdrs, int isq, long long *total) {
  http_req req;
  http_result res;

  *total = -1;
  memset(&res, 0, sizeof(res));
  memset(&req, 0, sizeof(req));
  req.url = t->url;
  /* bytes=1-2: a non-zero start so a server that only ranges on a non-zero
   * offset (and the test mock) still answers 206, which is what proves ranges
   * work. The two bytes are discarded; the size comes from Content-Range. */
  req.range_from = 1;
  req.range_end = 2;
  req.write = probe_abort; /* stop after the first byte */
  req.write_ctx = NULL;
  req.abort = &t->cancel;
  req.timeout_ms = 15000;
  if(isq) {
    req.user_agent = QUARK_DL_UA;
    if(hdrs[0]) {
      req.extra_headers = hdrs;
    }
  }
  /* The probe's write callback aborts the body on purpose, so http_perform
   * returns < 0; the status and size are still in `res`. */
  http_perform(&req, &res);
  if(res.total > 0) {
    *total = res.total;
  } else if(res.length > 0) {
    *total = res.length;
  }
  if(res.status == 400 || res.status == 401 || res.status == 403 ||
     res.status == 404 || res.status == 410 || res.status == 412) {
    if(res.status == 412) {
      snprintf(t->err, sizeof(t->err),
               "HTTP 412：CDN 拒绝，请回网盘页重新加入（会换新直链）");
    } else {
      snprintf(t->err, sizeof(t->err), "HTTP %d：链接无效或已过期", res.status);
    }
    return -3;
  }
  if(res.status != 200 && res.status != 206) {
    if(res.status == 0 && res.err[0]) {
      /* Transport failed before any status line (connection refused, TLS). */
      snprintf(t->err, sizeof(t->err), "%s", res.err);
      return t->cancel ? -2 : -1;
    }
    snprintf(t->err, sizeof(t->err), "HTTP %d", res.status);
    return res.status == 412 ? -3 : -1;
  }
  return res.status == 206 ? 0 : 1;
}

/* 0 done, -1 retry, -2 cancelled, -3 permanent. */
static int
dl_attempt(dl_task *t) {
  /* dir (300) + name (512) + a slash and ".part" suffix. The name can now be a
   * relative path, so this has to be able to hold the whole thing. */
  char part[1024];
  char final[1024];
  struct stat st;
  sink_ctx sink;
  http_result res;
  http_req req;
  long long have = 0;
  long long free_bytes;
  int fd;
  /* Off the heap and off the queue's lock: a task reads its header block once,
   * then spends minutes streaming. Threads here are pthread_create'd, so a few
   * more kilobytes of frame is not the hazard it is inside the web server. */
  char hdrs[QUARK_COOKIE_MAX + 96];
  int isq;

  if(path_join(part, sizeof(part), t->dir, t->name, ".part") ||
     path_join(final, sizeof(final), t->dir, t->name, "")) {
    snprintf(t->err, sizeof(t->err), "目标路径过长");
    return -3;
  }
  /* A name can now carry a relative path ("剧集/第01话/第03集.mkv"). The base
   * directory was made at add time, but the folders under it are made here,
   * right before the first write, so the tree the file came from is recreated
   * on disk. */
  {
    const char *slash = strrchr(t->name, '/');

    if(slash && slash != t->name) {
      char parent[1024];
      char rel[512];
      size_t n = (size_t)(slash - t->name);

      if(n >= sizeof(rel)) {
        n = sizeof(rel) - 1;
      }
      memcpy(rel, t->name, n);
      rel[n] = 0;
      if(path_join(parent, sizeof(parent), t->dir, rel, "") == 0 &&
         mkdirs(parent) != 0) {
        snprintf(t->err, sizeof(t->err), "无法创建目录 %s: %s", parent,
                 strerror(errno));
        return -3;
      }
    }
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

  fd = open(part, O_RDWR | O_CREAT, 0644);
  if(fd < 0) {
    snprintf(t->err, sizeof(t->err), "无法写入 %s: %s", part, strerror(errno));
    return -3;
  }
  hdr_snapshot(hdrs, sizeof(hdrs));
  isq = quark_host(t->url);

  /* Learn the size before choosing a path: a big file is split into ranges and
   * fetched over several connections (a single connection is CDN-throttled),
   * a small one is read in one go. The probe also proves the server honours
   * Range, which the segmented path depends on. */
  {
    long long total = -1;
    int prc = dl_probe_total(t, hdrs, isq, &total);

    if(prc < 0) {
      close(fd);
      return prc;
    }
    if(total > 0) {
      t->total = total;
    }
    if(total >= SEG_MIN_BYTES && prc == 0) {
      /* Big file and the server ranges: segmented download. It writes straight
       * into this fd, so nothing here touches the single-connection sink. */
      int seg;

      t->done = have;
      seg = dl_segmented(t, fd, have, total);

      if(seg == 0) {
        if(fsync(fd) != 0) {
          int saved = errno;

          close(fd);
          snprintf(t->err, sizeof(t->err), "落盘失败: %s", strerror(saved));
          return -1;
        }
        close(fd);
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
      /* A segmented failure: -2/-3 fall through to the worker's state machine;
       * -1 means "retry the whole file", which also falls through. */
      close(fd);
      return seg;
    }
  }

  /* Single connection: seek to the resume offset and stream the rest. */
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
  if(isq) {
    req.user_agent = QUARK_DL_UA;
    if(hdrs[0]) {
      req.extra_headers = hdrs;
    }
  }
  /* One line per attempt, because the interesting failure here is invisible
   * from the queue: a link that resolves fine and then downloads nothing. With
   * the header state, the resume offset and the length of the link on the
   * record, boot.log says whether the CDN was given the three things it asks
   * for - and whether the link arrived whole. A signed URL that lost its tail
   * is answered 412 or "SignatureDoesNotMatch", which look like credential
   * problems and are not. */
  boot_trace("下载 %s：URL %u 字节，续传 %lld 字节，登录头=%s", t->name,
             (unsigned)strlen(t->url), have,
             isq ? (hdrs[0] ? "是" : "否（没有会话 cookie）") : "不需要");

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
    if(res.status == 403 && strstr(sink.errbody, "SignatureDoesNotMatch")) {
      /* The storage node recomputed the signature over what arrived and got a
       * different answer, so the string we hold is not the string it signed -
       * a link that lost bytes on the way, or one that has expired. Retrying
       * the same bytes cannot help; asking for a new link can, which is what
       * the retry button does. */
      snprintf(t->err, sizeof(t->err),
               "HTTP 403：直链签名不匹配，请回网盘页重新加入（会换新直链）");
    } else {
      snprintf(t->err, sizeof(t->err), "HTTP %d：链接无效或已过期", res.status);
    }
    boot_trace("下载失败 %s：HTTP %d 重定向 %d 次，实到 URL %u 字节 %.180s",
               t->name, res.status, res.redirects,
               (unsigned)strlen(res.final_url), sink.errbody);
    return -3;
  }
  if(res.status != 200 && res.status != 206) {
    close(fd);
    /* 412 is the CDN's anti-hotlink answer: the link is fine, the request did
     * not look like the browser download it was minted for. Named apart from a
     * generic status because the fix is not "retry", it is the headers - and
     * made permanent so the queue does not spend four attempts proving the
     * same point in eight seconds. */
    if(res.status == 412) {
      snprintf(t->err, sizeof(t->err),
               "HTTP 412：CDN 拒绝，请回网盘页重新加入（会换新直链）");
    } else {
      snprintf(t->err, sizeof(t->err), "HTTP %d", res.status);
    }
    boot_trace("下载失败 %s：HTTP %d 重定向 %d 次，实到 URL %u 字节 %.180s",
               t->name, res.status, res.redirects,
               (unsigned)strlen(res.final_url), sink.errbody);
    return res.status == 412 ? -3 : -1;
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
  if(rc == 0) {
    /* Finished before the pause landed (the worker holds the lock here, so a
     * pause that raced it lost). Report done, not paused: the file is complete. */
    t->state = DL_DONE;
  } else if(t->pause) {
    /* Paused: the .part stays so the user can continue later. */
    t->state = DL_PAUSED;
    t->pause = 0;
    t->cancel = 0;
    if(!t->err[0]) {
      snprintf(t->err, sizeof(t->err), "已暂停");
    }
  } else if(t->cancel || rc == -2) {
    t->state = DL_CANCELED;
    if(!t->err[0]) {
      snprintf(t->err, sizeof(t->err), "已取消");
    }
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

  for(i = 0; i < g_count && g_running < g_limit; i++) {
    dl_task *t = &g_task[i];
    pthread_attr_t attr;
    const pthread_attr_t *use = NULL;
    pthread_t th;
    int attr_ok;
    int started;

    if(!t->id || t->state != DL_QUEUED) {
      continue;
    }
    t->cancel = 0;
    t->attempts = 0;
    t->err[0] = 0;
    t->state = DL_RUNNING;
    /* One attempt's frame is around 30 KB - a 7 KB http_result (it carries the
     * cookie jar), a 7 KB copy of the header block, and the HTTP client's
     * scratch - and the platform's default thread stack is not something to
     * gamble a download on. Naming it is what keeps "works until a link needs
     * a big header block" from being a mystery on the console. */
    attr_ok = (pthread_attr_init(&attr) == 0);
    if(attr_ok && pthread_attr_setstacksize(&attr, 512u * 1024u) == 0) {
      use = &attr;
    }
    started = pthread_create(&th, use, dl_worker, t);
    if(attr_ok) {
      pthread_attr_destroy(&attr);
    }
    if(started != 0) {
      t->state = DL_FAILED;
      snprintf(t->err, sizeof(t->err), "无法创建下载线程");
      continue;
    }
    pthread_detach(th);
    g_running++;
  }
}

/* ------------------------------------------------------------ public API */

void
dl_set_cookie(const char *cookie) {
  pthread_mutex_lock(&g_lock);
  if(cookie && *cookie) {
    snprintf(g_hdr, sizeof(g_hdr), "Referer: " QUARK_DL_REFERER "\r\n"
                                   "Cookie: %s\r\n",
             cookie);
  } else {
    g_hdr[0] = 0;
  }
  pthread_mutex_unlock(&g_lock);
}

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
  char clean[512];
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
  /* Refused, not shortened. Everything after the '?' of a quark direct link is
   * part of its signature, so a link that does not fit the queue is a link that
   * would be rejected by the CDN with a message about credentials. Saying so
   * here costs one build and saves a round of 412. */
  if(strlen(url) >= QUARK_URL_MAX) {
    snprintf(err, cap, "URL 太长：%u 字节，上限 %d", (unsigned)strlen(url),
             QUARK_URL_MAX - 1);
    boot_trace("入队被拒：URL %u 字节超过上限 %d", (unsigned)strlen(url),
               QUARK_URL_MAX - 1);
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
dl_pause(int id) {
  int i;
  int n = 0;

  pthread_mutex_lock(&g_lock);
  for(i = 0; i < g_count; i++) {
    dl_task *t = &g_task[i];

    if(!t->id || (id >= 0 && t->id != id)) {
      continue;
    }
    if(t->state == DL_QUEUED) {
      t->state = DL_PAUSED;
      if(!t->err[0]) {
        snprintf(t->err, sizeof(t->err), "已暂停");
      }
      n++;
    } else if(t->state == DL_RUNNING) {
      /* Ask the worker to stop, but keep the bytes: pause, not cancel. The
       * pause flag is what makes the worker land on DL_PAUSED rather than
       * DL_CANCELED, and cancel is the abort hook the HTTP layer watches. */
      t->pause = 1;
      t->cancel = 1;
      n++;
    }
  }
  save_locked();
  pthread_mutex_unlock(&g_lock);
  return n;
}

int
dl_resume(int id) {
  int i;
  int n = 0;

  pthread_mutex_lock(&g_lock);
  for(i = 0; i < g_count; i++) {
    dl_task *t = &g_task[i];

    if(!t->id || (id >= 0 && t->id != id)) {
      continue;
    }
    if(t->state == DL_PAUSED) {
      t->state = DL_QUEUED;
      t->pause = 0;
      t->cancel = 0;
      t->err[0] = 0;
      n++;
    }
  }
  if(n) {
    pump_locked();
  }
  save_locked();
  pthread_mutex_unlock(&g_lock);
  return n;
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
    if(t->state == DL_QUEUED || t->state == DL_PAUSED) {
      t->state = DL_CANCELED;
      snprintf(t->err, sizeof(t->err), "已取消");
    }
    n++;
  }
  /* Remove the half-downloaded file so "取消" really means the bytes are gone.
   * Only the .part is touched - a finished file (no .part) is left alone, and
   * a paused task that is canceled loses its resume point on purpose. */
  for(i = 0; i < g_count; i++) {
    dl_task *t = &g_task[i];
    char part[1024];

    if(!t->id) {
      continue;
    }
    if(id >= 0 && t->id != id) {
      continue;
    }
    if(path_join(part, sizeof(part), t->dir, t->name, ".part") == 0) {
      unlink(part);
    }
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
dl_limit(void) {
  int n;

  pthread_mutex_lock(&g_lock);
  n = g_limit;
  pthread_mutex_unlock(&g_lock);
  return n;
}

int
dl_set_limit(int n) {
  int changed;

  if(n < 1) {
    n = 1;
  }
  if(n > 8) {
    n = 8;
  }
  pthread_mutex_lock(&g_lock);
  changed = (n != g_limit);
  g_limit = n;
  if(changed && g_limit > g_running) {
    /* More slots just opened; start whatever is waiting. */
    pump_locked();
  }
  pthread_mutex_unlock(&g_lock);
  return g_limit;
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
    case DL_PAUSED: return "paused";
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
     app(out, cap, &used, ",\"limit\":%d,\"running\":%d,\"tasks\":[", g_limit,
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
      char full[1024];
      struct stat st;

      exists = (path_join(full, sizeof(full), t->dir, t->name, "") == 0 &&
                stat(full, &st) == 0 && st.st_size > 0)
                 ? 1
                 : 0;
    }
    /* Speed is the byte delta since the last snapshot, over the elapsed
     * milliseconds - a running average over the poll interval (2s). Only a
     * running task has one; anything else reports 0. The first sample has no
     * baseline, so it seeds the counters and reports 0 rather than a huge
     * number from a clock that just started. */
    {
      long long speed = 0;
      long long now = now_ms();

      if(t->state == DL_RUNNING) {
        if(t->sample_ms && now > t->sample_ms) {
          long long delta = t->done - t->sample_done;

          if(delta > 0) {
            speed = delta * 1000LL / (now - t->sample_ms);
          }
        }
        t->sample_done = t->done;
        t->sample_ms = now;
      }
      if(app(out, cap, &used,
             "{\"id\":%d,\"state\":\"%s\",\"attempts\":%d,\"done\":%lld,"
             "\"total\":%lld,\"speed\":%lld,\"exists\":%s,\"name\":",
             t->id, state_name(t->state), t->attempts, t->done, t->total,
             speed, exists < 0 ? "null" : (exists ? "true" : "false")) < 0 ||
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
