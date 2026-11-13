#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A response from a server that is having a bad day can nest arbitrarily deep.
 * 48 is far past anything a real payload uses and keeps the recursion inside a
 * thread stack that also has to hold 8 KB of request headers. */
#define JSON_MAX_DEPTH 48

typedef struct {
  const char *p;
  char *err;
  size_t cap;
  int depth;
  int failed;
} parser;

/* The offset is the only thing that makes a parse error actionable, so every
 * failure reports one. */
static void
fail_at(parser *s, const char *why, const char *start) {
  if(!s->failed) {
    s->failed = 1;
    snprintf(s->err, s->cap, "%s（第 %ld 字节）", why, (long)(s->p - start));
  }
}

static void
skip_ws(parser *s) {
  while(*s->p == ' ' || *s->p == '\t' || *s->p == '\n' || *s->p == '\r') {
    s->p++;
  }
}

static json_value *
new_value(json_type t) {
  json_value *v = (json_value *)calloc(1, sizeof(*v));

  if(v) {
    v->type = t;
    v->number = 0;
  }
  return v;
}

static int
hex4(const char *p, unsigned *out) {
  unsigned v = 0;
  int i;

  for(i = 0; i < 4; i++) {
    char c = p[i];

    v <<= 4;
    if(c >= '0' && c <= '9') {
      v |= (unsigned)(c - '0');
    } else if(c >= 'a' && c <= 'f') {
      v |= (unsigned)(c - 'a' + 10);
    } else if(c >= 'A' && c <= 'F') {
      v |= (unsigned)(c - 'A' + 10);
    } else {
      return -1;
    }
  }
  *out = v;
  return 0;
}

static size_t
utf8_put(unsigned cp, char *out) {
  if(cp < 0x80) {
    out[0] = (char)cp;
    return 1;
  }
  if(cp < 0x800) {
    out[0] = (char)(0xc0 | (cp >> 6));
    out[1] = (char)(0x80 | (cp & 0x3f));
    return 2;
  }
  if(cp < 0x10000) {
    out[0] = (char)(0xe0 | (cp >> 12));
    out[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
    out[2] = (char)(0x80 | (cp & 0x3f));
    return 3;
  }
  out[0] = (char)(0xf0 | (cp >> 18));
  out[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
  out[2] = (char)(0x80 | ((cp >> 6) & 0x3f));
  out[3] = (char)(0x80 | (cp & 0x3f));
  return 4;
}

/* Reads a quoted string into a freshly allocated buffer. The buffer is sized
 * from the remaining input, which is always at least as long as the decoded
 * result: every escape sequence is two or more bytes in and one to four out,
 * and \uXXXX is six in for at most four out. */
static char *
read_string(parser *s, const char *start) {
  const char *q = s->p + 1;
  size_t room = strlen(q);
  const char *end = q + room;
  char *out = (char *)malloc(room + 1);
  size_t o = 0;

  if(!out) {
    fail_at(s, "内存不足", start);
    return NULL;
  }
  for(;;) {
    char c = *q;

    if(!c) {
      free(out);
      fail_at(s, "字符串没有结束引号", start);
      return NULL;
    }
    if(c == '"') {
      q++;
      break;
    }
    if(c != '\\') {
      out[o++] = c;
      q++;
      continue;
    }
    q++;
    switch(*q) {
      case '"': out[o++] = '"'; q++; break;
      case '\\': out[o++] = '\\'; q++; break;
      case '/': out[o++] = '/'; q++; break;
      case 'b': out[o++] = '\b'; q++; break;
      case 'f': out[o++] = '\f'; q++; break;
      case 'n': out[o++] = '\n'; q++; break;
      case 'r': out[o++] = '\r'; q++; break;
      case 't': out[o++] = '\t'; q++; break;
      case 'u': {
        unsigned cp;
        size_t n;

        /* Four hex digits have to be present before hex4 reads them. A body
         * that ends mid-escape is a truncated response, not a code point. */
        if(q + 5 > end) {
          free(out);
          fail_at(s, "\\u 转义被截断", start);
          return NULL;
        }
        if(hex4(q + 1, &cp)) {
          free(out);
          fail_at(s, "\\u 后面不是四位十六进制", start);
          return NULL;
        }
        q += 5;
        /* A surrogate pair is two \u escapes that mean one code point. A lone
         * half is replaced rather than rejected: it cannot be rendered anyway,
         * and failing the whole document over it would be worse. */
        if(cp >= 0xd800 && cp <= 0xdbff && q + 6 <= end && q[0] == '\\' &&
           q[1] == 'u') {
          unsigned lo;

          if(!hex4(q + 2, &lo) && lo >= 0xdc00 && lo <= 0xdfff) {
            cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
            q += 6;
          } else {
            cp = 0xfffd;
          }
        } else if(cp >= 0xd800 && cp <= 0xdfff) {
          cp = 0xfffd;
        }
        n = utf8_put(cp, out + o);
        o += n;
        break;
      }
      default:
        free(out);
        fail_at(s, "无法识别的转义", start);
        return NULL;
    }
  }
  out[o] = 0;
  s->p = q;
  return out;
}

static json_value *parse_value(parser *s, const char *start);

/* Grows the container by one slot. Both arrays and objects store their values
 * the same way; objects additionally keep a parallel key array. */
static int
push_slot(json_value *v, json_value *item, char *key) {
  json_value *ni;
  char **nk;

  ni = (json_value *)realloc(v->items, (size_t)(v->count + 1) * sizeof(*ni));
  if(!ni) {
    return -1;
  }
  v->items = ni;
  if(v->type == JSON_OBJECT) {
    nk = (char **)realloc(v->keys, (size_t)(v->count + 1) * sizeof(*nk));
    if(!nk) {
      return -1;
    }
    v->keys = nk;
    v->keys[v->count] = key;
  }
  v->items[v->count] = *item;
  v->count++;
  return 0;
}

static json_value *
parse_array(parser *s, const char *start) {
  json_value *v = new_value(JSON_ARRAY);

  if(!v) {
    fail_at(s, "内存不足", start);
    return NULL;
  }
  s->p++; /* [ */
  skip_ws(s);
  if(*s->p == ']') {
    s->p++;
    return v;
  }
  for(;;) {
    json_value *item = parse_value(s, start);

    if(!item) {
      json_free(v);
      return NULL;
    }
    if(push_slot(v, item, NULL)) {
      json_free(item);
      json_free(v);
      fail_at(s, "内存不足", start);
      return NULL;
    }
    skip_ws(s);
    if(*s->p == ',') {
      s->p++;
      skip_ws(s);
      continue;
    }
    if(*s->p == ']') {
      s->p++;
      return v;
    }
    json_free(v);
    fail_at(s, "数组里缺少逗号或右方括号", start);
    return NULL;
  }
}

static json_value *
parse_object(parser *s, const char *start) {
  json_value *v = new_value(JSON_OBJECT);

  if(!v) {
    fail_at(s, "内存不足", start);
    return NULL;
  }
  s->p++; /* { */
  skip_ws(s);
  if(*s->p == '}') {
    s->p++;
    return v;
  }
  for(;;) {
    char *key;
    json_value *item;

    skip_ws(s);
    if(*s->p != '"') {
      json_free(v);
      fail_at(s, "对象的键必须是字符串", start);
      return NULL;
    }
    key = read_string(s, start);
    if(!key) {
      json_free(v);
      return NULL;
    }
    skip_ws(s);
    if(*s->p != ':') {
      free(key);
      json_free(v);
      fail_at(s, "键后面缺少冒号", start);
      return NULL;
    }
    s->p++;
    item = parse_value(s, start);
    if(!item) {
      free(key);
      json_free(v);
      return NULL;
    }
    if(push_slot(v, item, key)) {
      free(key);
      json_free(item);
      json_free(v);
      fail_at(s, "内存不足", start);
      return NULL;
    }
    skip_ws(s);
    if(*s->p == ',') {
      s->p++;
      continue;
    }
    if(*s->p == '}') {
      s->p++;
      return v;
    }
    json_free(v);
    fail_at(s, "对象里缺少逗号或右花括号", start);
    return NULL;
  }
}

static json_value *
parse_value(parser *s, const char *start) {
  json_value *v;

  skip_ws(s);
  if(s->depth >= JSON_MAX_DEPTH) {
    fail_at(s, "嵌套太深", start);
    return NULL;
  }
  switch(*s->p) {
    case '"': {
      char *str = read_string(s, start);

      if(!str) {
        return NULL;
      }
      v = new_value(JSON_STRING);
      if(!v) {
        free(str);
        fail_at(s, "内存不足", start);
        return NULL;
      }
      v->string = str;
      return v;
    }
    case '{':
      s->depth++;
      v = parse_object(s, start);
      s->depth--;
      return v;
    case '[':
      s->depth++;
      v = parse_array(s, start);
      s->depth--;
      return v;
    case 't':
      if(!strncmp(s->p, "true", 4)) {
        s->p += 4;
        v = new_value(JSON_BOOL);
        if(v) {
          v->boolean = 1;
        }
        return v;
      }
      break;
    case 'f':
      if(!strncmp(s->p, "false", 5)) {
        s->p += 5;
        v = new_value(JSON_BOOL);
        if(v) {
          v->boolean = 0;
        }
        return v;
      }
      break;
    case 'n':
      if(!strncmp(s->p, "null", 4)) {
        s->p += 4;
        return new_value(JSON_NULL);
      }
      break;
    default:
      if(*s->p == '-' || (*s->p >= '0' && *s->p <= '9')) {
        char *end = NULL;
        double d = strtod(s->p, &end);

        if(end == s->p) {
          break;
        }
        s->p = end;
        v = new_value(JSON_NUMBER);
        if(v) {
          v->number = d;
        }
        return v;
      }
      break;
  }
  fail_at(s, "无法识别的内容", start);
  return NULL;
}

json_value *
json_parse(const char *text, char *err, size_t cap) {
  parser s;
  json_value *v;

  if(err && cap) {
    err[0] = 0;
  }
  if(!text) {
    if(err && cap) {
      snprintf(err, cap, "内容为空");
    }
    return NULL;
  }
  memset(&s, 0, sizeof(s));
  s.p = text;
  s.err = err;
  s.cap = cap;
  v = parse_value(&s, text);
  if(!v) {
    return NULL;
  }
  skip_ws(&s);
  /* Trailing garbage means the document is not what the caller thinks it is,
   * and silently ignoring it is how a truncated or concatenated response turns
   * into a plausible-looking wrong answer. */
  if(*s.p) {
    json_free(v);
    fail_at(&s, "JSON 结束后还有多余内容", text);
    return NULL;
  }
  return v;
}

/* Releases everything a value owns, but not the value itself. Array and object
 * members live inside their parent's allocation, so handing &items[i] to free()
 * would be freeing a pointer into the middle of a block - which aborts, and
 * would have aborted on the first successful parse of any document. Only the
 * root of a tree is separately allocated and may be freed. */
static void
free_contents(json_value *v) {
  int i;

  for(i = 0; i < v->count; i++) {
    free_contents(&v->items[i]);
    if(v->keys) {
      free(v->keys[i]);
    }
  }
  free(v->items);
  free(v->keys);
  free(v->string);
  v->items = NULL;
  v->keys = NULL;
  v->string = NULL;
  v->count = 0;
}

void
json_free(json_value *v) {
  if(!v) {
    return;
  }
  free_contents(v);
  free(v);
}

const json_value *
json_get(const json_value *obj, const char *key) {
  int i;

  if(!obj || obj->type != JSON_OBJECT || !key) {
    return NULL;
  }
  for(i = 0; i < obj->count; i++) {
    if(obj->keys[i] && !strcmp(obj->keys[i], key)) {
      return &obj->items[i];
    }
  }
  return NULL;
}

const json_value *
json_at(const json_value *arr, int i) {
  if(!arr || arr->type != JSON_ARRAY || i < 0 || i >= arr->count) {
    return NULL;
  }
  return &arr->items[i];
}

const char *
json_str(const json_value *v, const char *dflt) {
  if(!v || v->type != JSON_STRING || !v->string) {
    return dflt;
  }
  return v->string;
}

long long
json_int(const json_value *v, long long dflt) {
  if(!v || v->type != JSON_NUMBER) {
    return dflt;
  }
  return (long long)v->number;
}

double
json_num(const json_value *v, double dflt) {
  if(!v || v->type != JSON_NUMBER) {
    return dflt;
  }
  return v->number;
}

int
json_bool(const json_value *v, int dflt) {
  if(!v || v->type != JSON_BOOL) {
    return dflt;
  }
  return v->boolean ? 1 : 0;
}

int
json_count(const json_value *v) {
  if(!v || (v->type != JSON_ARRAY && v->type != JSON_OBJECT)) {
    return 0;
  }
  return v->count;
}

const json_value *
json_path(const json_value *v, const char *path) {
  const char *p = path;

  if(!v || !path) {
    return NULL;
  }
  while(*p) {
    if(*p == '.') {
      p++;
      continue;
    }
    if(*p == '[') {
      int idx = 0;

      p++;
      while(*p >= '0' && *p <= '9') {
        idx = idx * 10 + (*p - '0');
        p++;
      }
      if(*p != ']') {
        return NULL;
      }
      p++;
      v = json_at(v, idx);
      if(!v) {
        return NULL;
      }
      continue;
    }
    {
      const char *dot = strchr(p, '.');
      const char *br = strchr(p, '[');
      const char *end = dot;
      char name[64];
      size_t n;

      if(!end || (br && br < end)) {
        end = br;
      }
      if(!end) {
        end = p + strlen(p);
      }
      n = (size_t)(end - p);
      if(n == 0 || n >= sizeof(name)) {
        return NULL;
      }
      memcpy(name, p, n);
      name[n] = 0;
      v = json_get(v, name);
      if(!v) {
        return NULL;
      }
      p = end;
    }
  }
  return v;
}
