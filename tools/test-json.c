/* Unit tests for the JSON reader.
 *
 * Written because this parser is the piece of stage three that cannot be
 * checked against the real service until a cookie exists, and a broken parser
 * would look exactly like a wrong field name: an empty directory. These cases
 * are the shapes the Quark API actually returns, plus the malformed ones that
 * must be rejected rather than silently half-read. */

#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(cond, what)                                                    \
  do {                                                                       \
    if(!(cond)) {                                                            \
      printf("FAIL %s (line %d): %s\n", what, __LINE__, #cond);              \
      failures++;                                                            \
    }                                                                        \
  } while(0)

static void
test_object(void) {
  char err[160];
  json_value *v = json_parse(
      "{\"status\":200,\"code\":0,\"data\":{\"list\":[{\"fid\":\"a1\","
      "\"file_name\":\"电影.mkv\",\"dir\":false,\"size\":1234}],"
      "\"metadata\":{\"_total\":7}}}",
      err, sizeof(err));

  CHECK(v != NULL, "object parses");
  if(!v) {
    printf("  error: %s\n", err);
    return;
  }
  CHECK(json_int(json_get(v, "status"), -1) == 200, "status");
  CHECK(json_int(json_get(v, "code"), -1) == 0, "code");
  CHECK(json_count(json_path(v, "data.list")) == 1, "list length");
  CHECK(!strcmp(json_str(json_path(v, "data.list[0].fid"), ""), "a1"), "fid");
  CHECK(!strcmp(json_str(json_path(v, "data.list[0].file_name"), ""),
                "电影.mkv"),
        "utf-8 name");
  CHECK(json_bool(json_path(v, "data.list[0].dir"), 1) == 0, "dir false");
  CHECK(json_int(json_path(v, "data.list[0].size"), 0) == 1234, "size");
  CHECK(json_int(json_path(v, "data.metadata._total"), 0) == 7, "total");
  CHECK(json_path(v, "data.list[9]") == NULL, "out of range is null");
  CHECK(json_path(v, "data.nope.deeper") == NULL, "missing path is null");
  json_free(v);
}

static void
test_escapes(void) {
  char err[160];
  json_value *v = json_parse(
      "{\"s\":\"a\\\"b\\\\c\\/d\\ne\\tf\",\"u\":\"\\u4e2d\\u6587\","
      "\"emoji\":\"\\ud83d\\ude00\",\"lone\":\"\\ud800x\"}",
      err, sizeof(err));

  CHECK(v != NULL, "escapes parse");
  if(!v) {
    printf("  error: %s\n", err);
    return;
  }
  CHECK(!strcmp(json_str(json_get(v, "s"), ""), "a\"b\\c/d\ne\tf"), "escapes");
  CHECK(!strcmp(json_str(json_get(v, "u"), ""), "中文"), "\\u decodes");
  /* U+1F600 */
  CHECK(!strcmp(json_str(json_get(v, "emoji"), ""), "\xf0\x9f\x98\x80"),
        "surrogate pair");
  CHECK(!strcmp(json_str(json_get(v, "lone"), ""), "\xef\xbf\xbdx"),
        "lone surrogate becomes U+FFFD");
  json_free(v);
}

static void
test_empty_and_whitespace(void) {
  char err[160];
  json_value *a = json_parse("  {}  ", err, sizeof(err));
  json_value *b = json_parse("\r\n[ ]\t", err, sizeof(err));
  json_value *c = json_parse("{\"n\":-12.5e2,\"t\":true,\"f\":false,\"z\":null}",
                             err, sizeof(err));

  CHECK(a && json_count(a) == 0, "empty object");
  CHECK(b && json_count(b) == 0, "empty array");
  CHECK(c && json_num(json_get(c, "n"), 0) == -1250.0, "exponent number");
  CHECK(c && json_bool(json_get(c, "t"), 0) == 1, "true");
  CHECK(c && json_bool(json_get(c, "f"), 1) == 0, "false");
  CHECK(c && json_get(c, "z")->type == JSON_NULL, "null");
  json_free(a);
  json_free(b);
  json_free(c);
}

static void
test_rejects(void) {
  char err[160];

  /* Each of these is something a broken or truncated response looks like. All
   * of them must fail loudly: a half-parsed answer is worse than no answer,
   * because it reads as "your folder is empty". */
  CHECK(json_parse("{\"a\":1", err, sizeof(err)) == NULL, "truncated object");
  CHECK(json_parse("[1,2,", err, sizeof(err)) == NULL, "truncated array");
  CHECK(json_parse("{\"a\":1}trailing", err, sizeof(err)) == NULL,
        "trailing garbage");
  CHECK(json_parse("", err, sizeof(err)) == NULL, "empty input");
  CHECK(json_parse("'a':1", err, sizeof(err)) == NULL, "single quotes");
  CHECK(json_parse("{\"a\" 1}", err, sizeof(err)) == NULL, "missing colon");
  /* Leading zeros are not legal JSON, but numbers go through strtod, which
   * accepts them. That slack is deliberate: a size written "0123" still means
   * 123, and refusing the whole listing over it would cost more than it buys. */
  {
    json_value *z = json_parse("{\"a\":0123}", err, sizeof(err));

    CHECK(z != NULL && json_int(json_get(z, "a"), 0) == 123,
          "leading zero tolerated");
    json_free(z);
  }
  CHECK(json_parse("{\"a\":\"\\u12\"}", err, sizeof(err)) == NULL,
        "short \\u escape");
  CHECK(json_parse("{\"a\":tru}", err, sizeof(err)) == NULL, "bad literal");
}

static void
test_deep_nesting(void) {
  char err[160];
  char buf[512];
  int i;
  int n = 0;
  json_value *v;

  /* 40 levels is inside the limit and must work. */
  for(i = 0; i < 40; i++) {
    buf[n++] = '[';
  }
  for(i = 0; i < 40; i++) {
    buf[n++] = ']';
  }
  buf[n] = 0;
  v = json_parse(buf, err, sizeof(err));
  CHECK(v != NULL, "40 levels parse");
  json_free(v);

  /* 200 is past it and must be refused rather than blowing the stack. */
  n = 0;
  for(i = 0; i < 200; i++) {
    buf[n++] = '[';
  }
  for(i = 0; i < 200; i++) {
    buf[n++] = ']';
  }
  buf[n] = 0;
  CHECK(json_parse(buf, err, sizeof(err)) == NULL, "200 levels refused");
}

static void
test_null_safety(void) {
  char err[160];
  json_value *v = json_parse("{\"a\":\"x\"}", err, sizeof(err));

  CHECK(json_get(NULL, "a") == NULL, "get on null");
  CHECK(json_at(NULL, 0) == NULL, "at on null");
  CHECK(json_str(NULL, "d") != NULL, "str on null returns default");
  CHECK(json_int(NULL, 5) == 5, "int default");
  CHECK(json_bool(NULL, 1) == 1, "bool default");
  CHECK(json_count(NULL) == 0, "count on null");
  CHECK(json_path(NULL, "a") == NULL, "path on null");
  CHECK(json_get(v, "nope") == NULL, "missing key");
  json_free(v);
  json_free(NULL);
}

int
main(void) {
  test_object();
  test_escapes();
  test_empty_and_whitespace();
  test_rejects();
  test_deep_nesting();
  test_null_safety();

  if(failures) {
    printf("%d check(s) failed\n", failures);
    return 1;
  }
  printf("json: all checks passed\n");
  return 0;
}
