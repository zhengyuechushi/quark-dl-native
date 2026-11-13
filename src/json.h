#pragma once

#include <stddef.h>

/* A JSON reader small enough to audit in one sitting.
 *
 * Every Quark endpoint answers in JSON, and nothing in this project could read
 * JSON before. cJSON would work, but it is a second build-time dependency in a
 * tree that already carries mbedTLS, and the API used here is narrow: parse a
 * document, walk into it, read strings and numbers. That is what this is.
 *
 * Deliberately not supported: comments, single quotes, trailing commas, NaN.
 * None of them are legal JSON, and accepting them would only hide a malformed
 * response.
 *
 * Values own their memory. json_free releases a whole tree. */

typedef enum {
  JSON_NULL = 0,
  JSON_BOOL,
  JSON_NUMBER,
  JSON_STRING,
  JSON_ARRAY,
  JSON_OBJECT
} json_type;

typedef struct json_value json_value;

struct json_value {
  json_type type;
  int boolean;          /* JSON_BOOL */
  double number;        /* JSON_NUMBER */
  char *string;         /* JSON_STRING, NUL-terminated */
  char **keys;          /* JSON_OBJECT: member names, NULL for arrays */
  json_value *items;    /* array elements or object values */
  int count;
};

/* Returns NULL on malformed input with a short reason in err. */
json_value *json_parse(const char *text, char *err, size_t cap);
void json_free(json_value *v);

/* All accessors accept NULL and return the default, so a caller can walk a
 * response without checking every step: a missing field and a missing parent
 * behave the same way, which is what a "field did not arrive" case should do. */
const json_value *json_get(const json_value *obj, const char *key);
const json_value *json_at(const json_value *arr, int i);
const char *json_str(const json_value *v, const char *dflt);
long long json_int(const json_value *v, long long dflt);
double json_num(const json_value *v, double dflt);
int json_bool(const json_value *v, int dflt);
int json_count(const json_value *v);

/* Resolves "data.list[0].file_name" style paths. Returns NULL when any step is
 * missing or has the wrong type. */
const json_value *json_path(const json_value *v, const char *path);
