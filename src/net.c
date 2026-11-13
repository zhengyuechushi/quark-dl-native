/* Sockets, DNS and TLS.
 *
 * Deliberately small: the download engine above this needs connect / read /
 * write and a way to know whether the certificate checked out, and nothing
 * else. Keeping it in one file means the console-only part of stage 2 is a
 * single unit that either works or reports why.
 *
 * The TLS contexts are per connection (own entropy and DRBG), so several
 * download workers can be inside net_read at the same time. The only shared
 * object is the parsed root store, which is read-only after net_init. */

#include "net.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "ca.h"

#ifdef QDL_TLS
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#endif

#define READ_TIMEOUT_S 30
#define DNS_TIMEOUT_MS 2500

static char g_status[192] = "not initialised";
static char g_resolver[48] = "-";

#ifdef QDL_TLS
static mbedtls_x509_crt g_ca;
static int g_ca_ready;
static char g_tls_version[48] = "mbedtls";

typedef struct {
  mbedtls_ssl_context ssl;
  mbedtls_ssl_config cfg;
  mbedtls_ctr_drbg_context drbg;
  mbedtls_entropy_context ent;
  int started;
} tls_state;
#endif

struct net_conn {
  int fd;
  int use_tls;
#ifdef QDL_TLS
  tls_state tls;
#endif
  char info[128];
  char subject[128];
  char issuer[128];
  char expires[40];
};

/* ---------------------------------------------------------------- helpers */

#ifdef QDL_TLS
static unsigned
count_certs(const char *pem) {
  static const char tag[] = "BEGIN CERTIFICATE";
  const char *p = pem;
  unsigned n = 0;

  while((p = strstr(p, tag)) != NULL) {
    n++;
    p += sizeof(tag) - 1;
  }
  return n;
}

/* ---------------------------------------------------------------- TLS side */

static void
tls_error(char *err, size_t cap, const char *what, int rc) {
  char detail[96];

  detail[0] = 0;
  mbedtls_strerror(rc, detail, sizeof(detail));
  snprintf(err, cap, "%s: %s (-0x%04x)", what, detail[0] ? detail : "unknown",
           (unsigned)-rc);
}

static int
tls_seed(tls_state *t, char *err, size_t cap) {
  static const char pers[] = "quark-dl";
  int rc;

  mbedtls_entropy_init(&t->ent);
  mbedtls_ctr_drbg_init(&t->drbg);
  rc = mbedtls_ctr_drbg_seed(&t->drbg, mbedtls_entropy_func, &t->ent,
                             (const unsigned char *)pers, sizeof(pers) - 1);
  if(rc) {
    tls_error(err, cap, "随机数初始化失败", rc);
    return -1;
  }
  return 0;
}

static int
ssl_send(void *ctx, const unsigned char *buf, size_t len) {
  int fd = *(int *)ctx;
  ssize_t n = send(fd, buf, len, 0);

  if(n < 0) {
    if(errno == EINTR) {
      return MBEDTLS_ERR_SSL_WANT_WRITE;
    }
    return MBEDTLS_ERR_NET_SEND_FAILED;
  }
  return (int)n;
}

/* A blocked read that hits SO_RCVTIMEO is a real failure here: returning
 * WANT_READ would make mbedtls call straight back into a socket that will
 * never produce bytes, which is an infinite loop rather than a timeout. */
static int
ssl_recv(void *ctx, unsigned char *buf, size_t len) {
  int fd = *(int *)ctx;
  ssize_t n = recv(fd, buf, len, 0);

  if(n < 0) {
    if(errno == EINTR) {
      return MBEDTLS_ERR_SSL_WANT_READ;
    }
    return MBEDTLS_ERR_NET_RECV_FAILED;
  }
  return (int)n;
}

static void
tls_describe(net_conn *c) {
  const mbedtls_x509_crt *peer;
  const char *ver;
  const char *suite;
  char dn[128];

  /* Either of these can be NULL once a handshake has failed early, so they are
   * never handed to printf unguarded. */
  ver = mbedtls_ssl_get_version(&c->tls.ssl);
  suite = mbedtls_ssl_get_ciphersuite(&c->tls.ssl);
  snprintf(c->info, sizeof(c->info), "%s / %s", ver ? ver : "TLS",
           suite ? suite : "未协商");
  peer = mbedtls_ssl_get_peer_cert(&c->tls.ssl);
  if(!peer) {
    return;
  }
  if(mbedtls_x509_dn_gets(dn, sizeof(dn), &peer->subject) > 0) {
    snprintf(c->subject, sizeof(c->subject), "%s", dn);
  }
  if(mbedtls_x509_dn_gets(dn, sizeof(dn), &peer->issuer) > 0) {
    snprintf(c->issuer, sizeof(c->issuer), "%s", dn);
  }
  snprintf(c->expires, sizeof(c->expires), "%04d-%02d-%02d", peer->valid_to.year,
           peer->valid_to.mon, peer->valid_to.day);
}

/* Spells out why a certificate was rejected. The bit values are the verified
 * ones from mbedtls x509_crt.h (BADCERT_*); they are written as literals rather
 * than macros so a header that renames one cannot break the console build. */
static void
verify_reasons(uint32_t flags, char *out, size_t cap) {
  static const struct {
    uint32_t bit;
    const char *label;
  } table[] = {
    {0x00000001, "证书已过期"},
    {0x00000002, "证书已被吊销"},
    {0x00000004, "证书名字与访问的域名不一致"},
    {0x00000008, "签发它的 CA 不在信任库里"},
    {0x00000200, "证书的生效时间还没到（主机时间可能不对）"},
    {0x00000800, "证书用途不允许用于 TLS"},
    {0x00001000, "证书扩展用途不允许用于 TLS"},
    {0x00004000, "证书的签名算法不被接受"},
    {0x00008000, "证书的公钥算法不被接受"},
    {0x00010000, "证书的公钥不可接受"},
  };
  size_t used = 0;
  unsigned i;

  out[0] = 0;
  for(i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
    size_t n;

    if(!(flags & table[i].bit)) {
      continue;
    }
    n = strlen(table[i].label);
    if(used + n + 4 >= cap) {
      break;
    }
    if(used) {
      memcpy(out + used, "、", 3); /* U+3001, three bytes in UTF-8 */
      used += 3;
    }
    memcpy(out + used, table[i].label, n);
    used += n;
  }
  out[used] = 0;
  if(!used) {
    snprintf(out, cap, "原因未归类");
  }
}

static int
tls_start(net_conn *c, const char *host, char *err, size_t cap) {
  int rc;

  if(!g_ca_ready) {
    snprintf(err, cap, "根证书未加载: %s", net_status());
    return -1;
  }
  mbedtls_ssl_init(&c->tls.ssl);
  mbedtls_ssl_config_init(&c->tls.cfg);
  if(tls_seed(&c->tls, err, cap)) {
    return -1;
  }
  rc = mbedtls_ssl_config_defaults(&c->tls.cfg, MBEDTLS_SSL_IS_CLIENT,
                                   MBEDTLS_SSL_TRANSPORT_STREAM,
                                   MBEDTLS_SSL_PRESET_DEFAULT);
  if(rc) {
    tls_error(err, cap, "TLS 配置失败", rc);
    return -1;
  }
  mbedtls_ssl_conf_authmode(&c->tls.cfg, MBEDTLS_SSL_VERIFY_REQUIRED);
  mbedtls_ssl_conf_ca_chain(&c->tls.cfg, &g_ca, NULL);
  mbedtls_ssl_conf_rng(&c->tls.cfg, mbedtls_ctr_drbg_random, &c->tls.drbg);
  if((rc = mbedtls_ssl_setup(&c->tls.ssl, &c->tls.cfg)) != 0) {
    tls_error(err, cap, "TLS 初始化失败", rc);
    return -1;
  }
  if((rc = mbedtls_ssl_set_hostname(&c->tls.ssl, host)) != 0) {
    tls_error(err, cap, "设置 SNI 失败", rc);
    return -1;
  }
  mbedtls_ssl_set_bio(&c->tls.ssl, &c->fd, ssl_send, ssl_recv, NULL);
  while((rc = mbedtls_ssl_handshake(&c->tls.ssl)) != 0) {
    if(rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
      continue;
    }
    /* A verification failure is the one error worth spelling out: it is what a
     * console with a wrong clock or a missing root looks like. */
    if(rc == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED) {
      uint32_t flags = mbedtls_ssl_get_verify_result(&c->tls.ssl);
      char why[200];

      /* Capture what the server actually presented before unwinding: after this
       * return the caller only has a sentence, and "which certificate" is the
       * one thing that separates a wrong clock from an intercepting proxy. */
      tls_describe(c);
      verify_reasons(flags, why, sizeof(why));
      snprintf(err, cap, "证书校验失败 [0x%08lx]: %s", (unsigned long)flags, why);
      return -1;
    }
    tls_error(err, cap, "TLS 握手失败", rc);
    return -1;
  }
  c->tls.started = 1;
  tls_describe(c);
  return 0;
}

static void
tls_free(net_conn *c) {
  if(c->tls.started) {
    mbedtls_ssl_close_notify(&c->tls.ssl);
  }
  mbedtls_ssl_free(&c->tls.ssl);
  mbedtls_ssl_config_free(&c->tls.cfg);
  mbedtls_ctr_drbg_free(&c->tls.drbg);
  mbedtls_entropy_free(&c->tls.ent);
  c->tls.started = 0;
}

#endif /* QDL_TLS */

#ifdef QDL_TLS
/* Root count and whether anything was added on top of the embedded bundle; the
 * status line is rebuilt from these so it never claims 118 roots after three
 * more arrived from disk. */
static unsigned g_roots;
static int g_extra;

static void
status_refresh(void) {
  snprintf(g_status, sizeof(g_status), "%s, %u 个根证书%s", g_tls_version, g_roots,
           g_extra ? "（含附加证书）" : "");
}
#endif

int
net_init(char *err, size_t cap) {
#ifdef QDL_TLS
  const char *pem = qdl_ca_bundle();
  const char *extra;
  unsigned roots;
  int rc;

  err[0] = 0;
  if(g_ca_ready) {
    return 0;
  }
  mbedtls_x509_crt_init(&g_ca);
  rc = mbedtls_x509_crt_parse(&g_ca, (const unsigned char *)pem, strlen(pem) + 1);
  if(rc < 0) {
    tls_error(err, cap, "根证书解析失败", rc);
    snprintf(g_status, sizeof(g_status), "%s", err);
    return -1;
  }
  roots = count_certs(pem);

  /* A trust anchor may be added from disk. The CI smoke test needs this to talk
   * to a server whose certificate it generated a second earlier, and it is also
   * how a console behind a filtering proxy can be given the interception root
   * without a rebuild. It can only add trust, never switch checking off. */
  extra = getenv("QDL_CA_FILE");
  if(extra && *extra) {
    int added = mbedtls_x509_crt_parse_file(&g_ca, extra);

    if(added < 0) {
      snprintf(err, cap, "QDL_CA_FILE=%s 解析失败 (-0x%04x)", extra, (unsigned)-added);
      snprintf(g_status, sizeof(g_status), "%s", err);
      return -1;
    }
    roots += (unsigned)added;
  }
  g_ca_ready = 1;
  g_roots = roots;
  g_extra = (extra && *extra) ? 1 : 0;
  status_refresh();
  return 0;
#else
  (void)err;
  (void)cap;
  snprintf(g_status, sizeof(g_status), "未编译 TLS（只能访问 http://）");
  return 0;
#endif
}

/* The console has no environment to put QDL_CA_FILE in, so the same escape
 * hatch also reads a PEM from the data directory: copy the interception root
 * there over FTP and reload, instead of rebuilding the ELF. Adding trust is the
 * only thing this can do. */
int
net_add_ca(const char *path, char *err, size_t cap) {
#ifdef QDL_TLS
  int added;

  err[0] = 0;
  if(!g_ca_ready) {
    snprintf(err, cap, "TLS 尚未初始化");
    return -1;
  }
  added = mbedtls_x509_crt_parse_file(&g_ca, path);
  if(added < 0) {
    snprintf(err, cap, "%s 解析失败 (-0x%04x)", path, (unsigned)-added);
    return -1;
  }
  g_roots += (unsigned)added;
  g_extra = 1;
  status_refresh();
  return added;
#else
  (void)path;
  snprintf(err, cap, "此构建不含 TLS");
  return -1;
#endif
}

const char *
net_status(void) {
  return g_status;
}

const char *
net_resolver(void) {
  return g_resolver;
}

/* ------------------------------------------------------------------- DNS */

static const unsigned char *
skip_name(const unsigned char *p, const unsigned char *end) {
  while(p && p < end) {
    unsigned char c = *p;

    if(c == 0) {
      return p + 1;
    }
    if((c & 0xC0) == 0xC0) {
      return (p + 2 <= end) ? p + 2 : NULL;
    }
    p += (size_t)c + 1;
  }
  return NULL;
}

/* A single A query, used only when getaddrinfo has nothing to say. On the
 * console that is a real possibility: the payload inherits whatever resolv.conf
 * its process happens to see, and dropping to UDP 53 costs eighty lines. */
static int
dns_lookup(const char *host, const char *server, char *out, size_t cap) {
  unsigned char q[512];
  unsigned char resp[1200];
  struct sockaddr_in dst;
  struct timeval tv;
  fd_set set;
  size_t qn;
  const char *s;
  const unsigned char *p;
  const unsigned char *end;
  unsigned short id;
  int fd;
  int n;
  int i;
  int qd;
  int an;

  memset(&dst, 0, sizeof(dst));
  dst.sin_family = AF_INET;
  dst.sin_port = htons(53);
  if(inet_pton(AF_INET, server, &dst.sin_addr) != 1) {
    return -1;
  }

  id = (unsigned short)(((unsigned)getpid() * 2654435761u) ^ (unsigned)time(NULL));
  memset(q, 0, sizeof(q));
  q[0] = (unsigned char)(id >> 8);
  q[1] = (unsigned char)(id & 0xFF);
  q[2] = 0x01; /* recursion desired */
  q[4] = 0x00;
  q[5] = 0x01; /* one question */
  qn = 12;
  for(s = host; *s;) {
    const char *dot = strchr(s, '.');
    size_t len = dot ? (size_t)(dot - s) : strlen(s);

    if(len == 0 || len > 63 || qn + len + 6 > sizeof(q)) {
      return -1;
    }
    q[qn++] = (unsigned char)len;
    memcpy(q + qn, s, len);
    qn += len;
    s = dot ? dot + 1 : s + len;
  }
  q[qn++] = 0;
  q[qn++] = 0;
  q[qn++] = 1; /* A */
  q[qn++] = 0;
  q[qn++] = 1; /* IN */

  fd = socket(AF_INET, SOCK_DGRAM, 0);
  if(fd < 0) {
    return -1;
  }
  tv.tv_sec = DNS_TIMEOUT_MS / 1000;
  tv.tv_usec = (DNS_TIMEOUT_MS % 1000) * 1000;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  n = (int)sendto(fd, q, qn, 0, (struct sockaddr *)&dst, sizeof(dst));
  if(n != (int)qn) {
    close(fd);
    return -1;
  }
  FD_ZERO(&set);
  FD_SET(fd, &set);
  n = select(fd + 1, &set, NULL, NULL, &tv);
  if(n <= 0) {
    close(fd);
    return -1;
  }
  n = (int)recv(fd, resp, sizeof(resp), 0);
  close(fd);
  if(n < 12) {
    return -1;
  }
  if(resp[0] != q[0] || resp[1] != q[1] || !(resp[2] & 0x80) ||
     (resp[3] & 0x0F) != 0) {
    return -1;
  }
  qd = (resp[4] << 8) | resp[5];
  an = (resp[6] << 8) | resp[7];
  p = resp + 12;
  end = resp + n;
  for(i = 0; i < qd; i++) {
    p = skip_name(p, end);
    if(!p || p + 4 > end) {
      return -1;
    }
    p += 4;
  }
  for(i = 0; i < an; i++) {
    unsigned short type;
    unsigned short rdlen;

    p = skip_name(p, end);
    if(!p || p + 10 > end) {
      return -1;
    }
    type = (unsigned short)((p[0] << 8) | p[1]);
    rdlen = (unsigned short)((p[8] << 8) | p[9]);
    p += 10;
    if(p + rdlen > end) {
      return -1;
    }
    if(type == 1 && rdlen == 4) {
      snprintf(out, cap, "%u.%u.%u.%u", p[0], p[1], p[2], p[3]);
      return 0;
    }
    p += rdlen;
  }
  return -1;
}

/* Name servers from /etc/resolv.conf, then two well-known public ones. The
 * console may have no resolv.conf at all, which is exactly why this list has
 * hard-coded entries. */
static int
dns_fallback(const char *host, char *out, size_t cap) {
  static const char *defaults[] = {"223.5.5.5", "119.29.29.29", "8.8.8.8"};
  FILE *f;
  char line[160];
  unsigned i;

  f = fopen("/etc/resolv.conf", "r");
  if(f) {
    while(fgets(line, sizeof(line), f)) {
      char server[64];

      if(sscanf(line, "nameserver %63s", server) == 1) {
        if(dns_lookup(host, server, out, cap) == 0) {
          fclose(f);
          snprintf(g_resolver, sizeof(g_resolver), "dns %s", server);
          return 0;
        }
      }
    }
    fclose(f);
  }
  for(i = 0; i < sizeof(defaults) / sizeof(defaults[0]); i++) {
    if(dns_lookup(host, defaults[i], out, cap) == 0) {
      snprintf(g_resolver, sizeof(g_resolver), "dns %s", defaults[i]);
      return 0;
    }
  }
  return -1;
}

int
net_resolve(const char *host, char *out, size_t cap, int *family) {
  struct addrinfo hints;
  struct addrinfo *res = NULL;
  struct in_addr v4;
  struct in6_addr v6;

  if(inet_pton(AF_INET, host, &v4) == 1) {
    snprintf(out, cap, "%s", host);
    if(family) {
      *family = AF_INET;
    }
    snprintf(g_resolver, sizeof(g_resolver), "literal");
    return 0;
  }
  if(inet_pton(AF_INET6, host, &v6) == 1) {
    snprintf(out, cap, "%s", host);
    if(family) {
      *family = AF_INET6;
    }
    snprintf(g_resolver, sizeof(g_resolver), "literal");
    return 0;
  }
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  if(getaddrinfo(host, NULL, &hints, &res) == 0 && res) {
    struct sockaddr_in *sin = (struct sockaddr_in *)res->ai_addr;
    char buf[64];

    if(inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf))) {
      snprintf(out, cap, "%s", buf);
      freeaddrinfo(res);
      if(family) {
        *family = AF_INET;
      }
      snprintf(g_resolver, sizeof(g_resolver), "getaddrinfo");
      return 0;
    }
    freeaddrinfo(res);
  }
  if(dns_fallback(host, out, cap) == 0) {
    if(family) {
      *family = AF_INET;
    }
    return 0;
  }
  return -1;
}

/* --------------------------------------------------------------- sockets */

static int
connect_to(const char *ip, unsigned short port, int timeout_ms, char *err,
           size_t cap) {
  struct sockaddr_in addr;
  struct timeval tv;
  fd_set set;
  socklen_t len;
  int fd;
  int rc;
  int flags;
  int so_error = 0;

  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  if(inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
    snprintf(err, cap, "地址无效: %s", ip);
    return -1;
  }
  fd = socket(AF_INET, SOCK_STREAM, 0);
  if(fd < 0) {
    snprintf(err, cap, "socket() 失败: %s", strerror(errno));
    return -1;
  }
  flags = fcntl(fd, F_GETFL, 0);
  fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  rc = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
  if(rc != 0 && errno != EINPROGRESS) {
    snprintf(err, cap, "连接 %s:%u 失败: %s", ip, (unsigned)port, strerror(errno));
    close(fd);
    return -1;
  }
  if(rc != 0) {
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    FD_ZERO(&set);
    FD_SET(fd, &set);
    rc = select(fd + 1, NULL, &set, NULL, &tv);
    if(rc <= 0) {
      snprintf(err, cap, "连接 %s:%u 超时（%d ms）", ip, (unsigned)port, timeout_ms);
      close(fd);
      return -1;
    }
    len = sizeof(so_error);
    if(getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &len) != 0 || so_error) {
      snprintf(err, cap, "连接 %s:%u 被拒绝: %s", ip, (unsigned)port,
               strerror(so_error ? so_error : errno));
      close(fd);
      return -1;
    }
  }
  fcntl(fd, F_SETFL, flags);
  tv.tv_sec = READ_TIMEOUT_S;
  tv.tv_usec = 0;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  {
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
  }
  return fd;
}

static void
peer_copy(net_peer *p, const net_conn *c) {
  if(!p) {
    return;
  }
  snprintf(p->info, sizeof(p->info), "%s", c->info);
  snprintf(p->subject, sizeof(p->subject), "%s", c->subject);
  snprintf(p->issuer, sizeof(p->issuer), "%s", c->issuer);
  snprintf(p->expires, sizeof(p->expires), "%s", c->expires);
}

net_conn *
net_dial(const char *host, unsigned short port, int use_tls, int timeout_ms,
         char *err, size_t cap, net_peer *peer) {
  char ip[64];
  net_conn *c;
  int fd;

  err[0] = 0;
  if(peer) {
    memset(peer, 0, sizeof(*peer));
  }
#ifdef QDL_TLS
  if(use_tls && !g_ca_ready) {
    snprintf(err, cap, "TLS 未就绪: %s", g_status);
    return NULL;
  }
#else
  if(use_tls) {
    snprintf(err, cap, "此构建不含 TLS");
    return NULL;
  }
#endif
  if(net_resolve(host, ip, sizeof(ip), NULL)) {
    snprintf(err, cap, "无法解析域名 %s（%s）", host, g_resolver);
    return NULL;
  }
  fd = connect_to(ip, port, timeout_ms, err, cap);
  if(fd < 0) {
    return NULL;
  }
  c = calloc(1, sizeof(*c));
  if(!c) {
    close(fd);
    snprintf(err, cap, "内存不足");
    return NULL;
  }
  c->fd = fd;
  c->use_tls = use_tls;
  snprintf(c->info, sizeof(c->info), "plain");
#ifdef QDL_TLS
  if(use_tls && tls_start(c, host, err, cap)) {
    /* The connection is dead but the certificate is not: hand it to the caller
     * so the error can say whose certificate was refused. */
    peer_copy(peer, c);
    tls_free(c);
    close(fd);
    free(c);
    return NULL;
  }
#else
  (void)use_tls;
#endif
  peer_copy(peer, c);
  return c;
}

long
net_read(net_conn *c, void *buf, size_t len, char *err, size_t cap) {
  if(len == 0) {
    return 0;
  }
#ifdef QDL_TLS
  if(c->use_tls) {
    int rc = mbedtls_ssl_read(&c->tls.ssl, (unsigned char *)buf, len);

    if(rc == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
      return 0;
    }
    if(rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
      snprintf(err, cap, "TLS 读取超时");
      return -1;
    }
    if(rc < 0) {
      tls_error(err, cap, "TLS 读取失败", rc);
      return -1;
    }
    return rc;
  }
#endif
  for(;;) {
    ssize_t n = recv(c->fd, buf, len, 0);

    if(n < 0) {
      if(errno == EINTR) {
        continue;
      }
      if(errno == EAGAIN || errno == EWOULDBLOCK) {
        snprintf(err, cap, "读取超时（%d s 无数据）", READ_TIMEOUT_S);
      } else {
        snprintf(err, cap, "读取失败: %s", strerror(errno));
      }
      return -1;
    }
    return (long)n;
  }
}

int
net_write_all(net_conn *c, const void *buf, size_t len, char *err, size_t cap) {
  const char *p = (const char *)buf;

  while(len) {
#ifdef QDL_TLS
    if(c->use_tls) {
      int rc = mbedtls_ssl_write(&c->tls.ssl, (const unsigned char *)p, len);

      if(rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
        continue;
      }
      if(rc < 0) {
        tls_error(err, cap, "TLS 发送失败", rc);
        return -1;
      }
      p += rc;
      len -= (size_t)rc;
      continue;
    }
#endif
    {
      ssize_t n = send(c->fd, p, len, 0);

      if(n < 0) {
        if(errno == EINTR) {
          continue;
        }
        snprintf(err, cap, "发送失败: %s", strerror(errno));
        return -1;
      }
      p += n;
      len -= (size_t)n;
    }
  }
  return 0;
}

void
net_close(net_conn *c) {
  if(!c) {
    return;
  }
#ifdef QDL_TLS
  if(c->use_tls) {
    tls_free(c);
  }
#endif
  if(c->fd >= 0) {
    close(c->fd);
  }
  free(c);
}

const char *
net_info(net_conn *c) {
  return c->info;
}

const char *
net_cert_subject(net_conn *c) {
  return c->subject;
}

const char *
net_cert_issuer(net_conn *c) {
  return c->issuer;
}

const char *
net_cert_expires(net_conn *c) {
  return c->expires;
}
