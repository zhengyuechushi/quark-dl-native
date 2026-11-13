#include "cookie.h"

#include <stdio.h>
#include <string.h>

/* A cookie for this service runs to a few kilobytes. The temporary buffer has to
 * be able to hold the longest candidate before it is copied out, otherwise the
 * length check and the stored value would disagree. */
#define COOKIE_CAND_MAX 8192

/* Case-insensitive compare of the leading word; s points at the candidate. */
static int
ci_word(const char *s, const char *word) {
  while(*word) {
    char a = *s++;
    char b = *word++;

    if(a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
    if(b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
    if(a != b) return 0;
  }
  return 1;
}

/* Copies one header value starting at p. It ends at a line break, at the quote
 * closing a shell argument, or at the caret cURL uses on Windows in place of a
 * backslash. Trailing separators go, so "a=b; " and "a=b" are one string. */
static int
take_value(const char *p, char *out, size_t cap) {
  size_t o = 0;

  while(*p == ' ' || *p == '\t' || *p == '"' || *p == '\'') {
    p++;
  }
  for(; *p && *p != '\r' && *p != '\n' && *p != '"' && *p != '\'' && *p != '^';
      p++) {
    if(o + 1 < cap) out[o++] = *p;
  }
  while(o && (out[o - 1] == ' ' || out[o - 1] == '\t' || out[o - 1] == ';')) {
    o--;
  }
  out[o] = 0;
  return (int)o;
}

/* A cookie is a list of name=value pairs. No equals sign means this is not one,
 * whatever else it may be. */
static int
looks_like_cookie(const char *s) {
  return s && s[0] && strchr(s, '=') != NULL;
}

/* "cookie:" anywhere in the text. Covers the header block, the single header
 * line, and -H 'cookie: ..' inside a cURL command. */
static int
scan_header(const char *raw, char *out, size_t cap) {
  const char *p;

  for(p = raw; *p; p++) {
    const char *v;
    int n;

    if(*p != 'c' && *p != 'C') continue;
    if(!ci_word(p, "cookie")) continue;
    /* A word boundary is not checked on the left: it would have to accept the
     * quote of a shell argument, a dash of -H, and the start of a line anyway,
     * and requiring a colon after the word is what actually excludes the cases
     * that matter. */
    v = p + 6;
    while(*v == ' ' || *v == '\t') v++;
    if(*v != ':') continue;
    n = take_value(v + 1, out, cap);
    if(looks_like_cookie(out)) return n;
    out[0] = 0;
  }
  return 0;
}

/* cURL's own cookie switch: "-b value", "-bvalue", "--cookie value" and
 * "--cookie=value" all mean the same thing. */
static int
scan_curl_b(const char *raw, char *out, size_t cap) {
  const char *p;

  for(p = raw; *p; p++) {
    const char *v = NULL;
    int n;

    if(p[0] != '-') continue;
    if(p[1] == 'b') {
      /* -b takes its value either attached or after a space. */
      v = p + 2;
    } else if(ci_word(p, "--cookie")) {
      v = p + 8;
    } else {
      continue;
    }
    while(*v == ' ' || *v == '\t' || *v == '=') v++;
    /* The long form without a value would otherwise swallow the URL that
     * follows it; take_value rejects anything without an "=". */
    n = take_value(v, out, cap);
    if(looks_like_cookie(out)) return n;
    out[0] = 0;
  }
  return 0;
}

/* No marker at all, so the paste is probably the value itself or one line out of
 * the headers panel. The longest line that parses wins; a header block whose
 * cookie line was left behind yields nothing and says so upstream. */
static int
scan_lines(const char *raw, char *out, size_t cap) {
  const char *p = raw;
  char line[COOKIE_CAND_MAX];
  char cand[COOKIE_CAND_MAX];
  int best = 0;

  while(*p) {
    const char *e = p;
    int len;
    int n;

    while(*e && *e != '\r' && *e != '\n') e++;
    len = (int)(e - p);
    if(len > 0 && len < (int)sizeof(line)) {
      memcpy(line, p, (size_t)len);
      line[len] = 0;
      /* Same trimming as a header value, so a selection that caught the spaces
       * on either side still stores a usable cookie. */
      n = take_value(line, cand, sizeof(cand));
      if(n > best && n < (int)cap && looks_like_cookie(cand)) {
        best = n;
        memcpy(out, cand, (size_t)n + 1);
      }
    }
    p = *e ? e + 1 : e;
  }
  return best;
}

int
cookie_extract(const char *raw, char *out, size_t cap) {
  int n;

  if(!out || !cap) return 0;
  out[0] = 0;
  if(!raw) return 0;

  if((n = scan_header(raw, out, cap)) > 0) return n;
  if((n = scan_curl_b(raw, out, cap)) > 0) return n;
  return scan_lines(raw, out, cap);
}

/* ------------------------------------------------------------- cookie jar */

/* Finds "name=" at the start of the jar or right after a separator. Substring
 * matches are rejected: looking for "__pus" must not find "__puus". */
static char *
jar_find(char *jar, const char *name) {
  size_t n = strlen(name);
  char *p = jar;

  while(*p) {
    if(!strncmp(p, name, n) && p[n] == '=') return p;
    p = strstr(p, "; ");
    if(!p) return NULL;
    p += 2;
  }
  return NULL;
}

void
cookie_jar_put(char *jar, size_t cap, const char *line) {
  char name[64];
  char pair[600];
  char *slot;
  char *end;
  size_t n = 0;
  size_t vstart;
  size_t used;
  size_t plen;

  if(!jar || !cap || !line) return;
  while(*line == ' ' || *line == '\t') line++;

  while(line[n] && line[n] != '=' && line[n] != ';' && n < sizeof(name) - 1) {
    name[n] = line[n];
    n++;
  }
  while(n && (name[n - 1] == ' ' || name[n - 1] == '\t')) {
    n--;
  }
  name[n] = 0;
  if(!n || line[n] != '=') return;   /* a Set-Cookie without a pair is not one */

  vstart = n + 1;
  n = 0;
  while(line[vstart + n] && line[vstart + n] != ';' &&
        n < sizeof(pair) - sizeof(name) - 2) {
    n++;
  }
  snprintf(pair, sizeof(pair), "%s=%.*s", name, (int)n, line + vstart);
  /* A value that arrived with trailing blanks would stop matching the server's
   * own copy of it. */
  for(n = strlen(pair); n && (pair[n - 1] == ' ' || pair[n - 1] == '\t' ||
                              pair[n - 1] == '\r' || pair[n - 1] == '\n');
      n--) {
    pair[n - 1] = 0;
  }

  slot = jar_find(jar, name);
  if(slot) {
    end = strstr(slot, "; ");
    if(end) {
      memmove(slot, end + 2, strlen(end + 2) + 1);
    } else {
      slot[0] = 0;
    }
  }
  used = strlen(jar);
  plen = strlen(pair);
  if(used + plen + (used ? 2 : 0) + 1 >= cap) {
    return;   /* a jar that does not fit is better short than half a value */
  }
  if(used) {
    memcpy(jar + used, "; ", 2);
    used += 2;
  }
  memcpy(jar + used, pair, plen + 1);
}
