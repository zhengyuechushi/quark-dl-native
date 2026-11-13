/* Unit tests for the cookie paste parser.
 *
 * This is the step that cannot be checked on the console without an account, and
 * it is also the step people get wrong: copying a cookie out of the developer
 * tools has four legitimate shapes and several illegitimate ones. A parser that
 * quietly returns half a cookie produces a 401, which looks like a login problem
 * rather than a paste problem, so each shape is pinned down here. */

#include "cookie.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond, what)                                                    \
  do {                                                                       \
    if(!(cond)) {                                                            \
      printf("FAIL %s (line %d): %s\n", what, __LINE__, #cond);              \
      failures++;                                                            \
    }                                                                        \
  } while(0)

/* The thing every case is really asserting: what came out equals what a correct
 * paste of the same cookie would have produced. */
static void
expect(const char *in, const char *want, const char *what) {
  char out[8192];
  int n = cookie_extract(in, out, sizeof(out));

  if(strcmp(out, want)) {
    printf("FAIL %s\n  in:   %s\n  want: %s\n  got:  %s\n", what, in, want, out);
    failures++;
    return;
  }
  if(n != (int)strlen(want)) {
    printf("FAIL %s: length %d, want %d\n", what, n, (int)strlen(want));
    failures++;
  }
}

#define VAL "__pus=aaa111; __puus=bbb222; __kp=ccc333; __uid=dddd"

static void
test_shapes(void) {
  /* 1. the value alone */
  expect(VAL, VAL, "bare value");

  /* 2. the header line, both cases */
  expect("Cookie: " VAL, VAL, "header line");
  expect("cookie:" VAL, VAL, "header line lower case");

  /* 3. a whole header block, the way the Headers panel copies it */
  expect("accept: */*\ncookie: " VAL "\nreferer: https://pan.quark.cn/\n", VAL,
         "header block");

  /* 4. a cURL command: bash quoting, cmd quoting, and -b instead of -H */
  expect("curl 'https://drive-pc.quark.cn/1/clouddrive/file/sort?pr=ucpro' \\\n"
         "  -H 'accept: */*' \\\n  -H 'cookie: " VAL "' \\\n"
         "  -H 'referer: https://pan.quark.cn/'",
         VAL, "copy as curl bash");
  expect("curl ^\"https://drive-pc.quark.cn/1/clouddrive/file/sort^\" ^\n"
         "  -H ^\"cookie: " VAL "^\",",
         VAL, "copy as curl cmd");
  expect("curl -b '" VAL "' https://drive-pc.quark.cn/", VAL, "curl -b");
  /* The four spellings cURL accepts, and the one the test caught: "--cookie"
   * was skipped entirely because its second character is not 'b'. */
  expect("curl --cookie '" VAL "' https://drive-pc.quark.cn/", VAL,
         "curl --cookie");
  expect("curl --cookie='" VAL "' https://drive-pc.quark.cn/", VAL,
         "curl --cookie=");
  expect("curl -b'" VAL "' https://drive-pc.quark.cn/", VAL, "curl -b attached");

  /* A cURL command where the cookie is not the first header: the scan must not
   * stop at the first thing that resembles a header name. */
  expect("curl 'https://x/' -H 'referer: https://pan.quark.cn/' "
         "-H 'cookie: " VAL "'",
         VAL, "cookie after other headers");
}

static void
test_repairs(void) {
  /* Trailing separator, the most common stray character from a selection that
   * ran one character too far. */
  expect("Cookie: " VAL "; ", VAL, "trailing semicolon");
  expect("  " VAL "  ", VAL, "surrounding spaces");

  /* A long cookie must arrive whole. Truncation would look like an expired
   * session, because the service stops recognising the tail. */
  {
    char big[4096];
    char out[8192];
    size_t o = 0;
    int i;

    for(i = 0; i < 40; i++) {
      o += (size_t)snprintf(big + o, sizeof(big) - o, "__k%02d=%032d; ", i, i);
    }
    CHECK(o > 1000, "test cookie is big enough to be interesting");
    CHECK(cookie_extract(big, out, sizeof(out)) == (int)(o - 2),
          "long cookie arrives whole");
    CHECK(out[o - 2] == 0, "long cookie is terminated");
  }
}

static void
test_rejects(void) {
  char out[8192];

  /* The exact paste from the console report: a Headers panel with the
   * "Provisional headers are shown" warning, where the request never carried a
   * cookie at all. It must come back empty, not as a plausible-looking header
   * fragment that fails later with a 401. */
  CHECK(cookie_extract("Provisional headers are shown\n"
                       "dnt: 1\n"
                       "referer: https://pan.quark.cn/\n"
                       "user-agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64)\n",
                       out, sizeof(out)) == 0,
        "provisional headers paste yields nothing");

  CHECK(cookie_extract("", out, sizeof(out)) == 0, "empty");
  CHECK(cookie_extract(NULL, out, sizeof(out)) == 0, "null");
  CHECK(cookie_extract("\n\n\n", out, sizeof(out)) == 0, "whitespace only");
  CHECK(cookie_extract("this is not a cookie", out, sizeof(out)) == 0,
        "prose");
  CHECK(cookie_extract("cookie:", out, sizeof(out)) == 0, "header with no value");
  CHECK(cookie_extract("cookie: no-equals-sign-here", out, sizeof(out)) == 0,
        "header value without a pair");
}

/* The scan login ends with the session arriving in Set-Cookie, one header per
 * cookie, each carrying attributes that must not end up in the replay string.
 * Getting this wrong produces a cookie that looks plausible and is rejected. */
static void
jar_case(const char *what, const char *const *lines, int n, const char *want) {
  char jar[2048];
  int i;

  jar[0] = 0;
  for(i = 0; i < n; i++) cookie_jar_put(jar, sizeof(jar), lines[i]);
  if(strcmp(jar, want)) {
    printf("FAIL jars: %s\n  want: %s\n  got:  %s\n", what, want, jar);
    failures++;
  }
}

static void
test_jar(void) {
  {
    const char *l[] = {"__pus=aaa; Domain=.quark.cn; Path=/; Secure; HttpOnly"};
    jar_case("attributes dropped", l, 1, "__pus=aaa");
  }
  {
    const char *l[] = {"__pus=aaa; Path=/", "__puus=bbb; Path=/",
                       "__kp=ccc; Max-Age=0"};
    jar_case("several headers", l, 3, "__pus=aaa; __puus=bbb; __kp=ccc");
  }
  {
    /* A refreshed session sends the same names again; the last value wins, and
     * the jar must not grow a duplicate that the server would then see twice. */
    const char *l[] = {"__pus=old; Path=/", "__puus=b", "__pus=new; Path=/"};
    jar_case("later value replaces", l, 3, "__puus=b; __pus=new");
  }
  {
    /* "__pus" must not match inside "__puus": a naive strstr would overwrite the
     * wrong entry and silently drop the token the service actually checks. */
    const char *l[] = {"__puus=bbb; Path=/", "__pus=aaa; Path=/"};
    jar_case("prefix is not a match", l, 2, "__puus=bbb; __pus=aaa");
  }
  {
    const char *l[] = {"garbage", "=nameless", "__uid="};
    jar_case("junk ignored", l, 3, "__uid=");
  }
  {
    /* Overflow must not leave a truncated value behind, which would be invalid
     * on the wire rather than merely useless. */
    char jar[64];

    jar[0] = 0;
    cookie_jar_put(jar, sizeof(jar), "__pus=aaaa; Path=/");
    cookie_jar_put(jar, sizeof(jar),
                   "__puus=012345678901234567890123456789012345678901234567890");
    CHECK(strlen(jar) < sizeof(jar), "jar overflow leaves a valid string");
    CHECK(strstr(jar, "__pus=aaaa") != NULL, "earlier entry survives overflow");
  }
}

int
main(void) {
  test_shapes();
  test_repairs();
  test_rejects();
  test_jar();

  if(failures) {
    printf("%d check(s) failed\n", failures);
    return 1;
  }
  printf("cookie: all checks passed\n");
  return 0;
}
