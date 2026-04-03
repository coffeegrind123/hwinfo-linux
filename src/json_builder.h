#ifndef JSON_BUILDER_H
#define JSON_BUILDER_H

#include <stddef.h>

#define JSON_MAX_DEPTH 32

typedef struct {
    char *data;
    size_t len;
    size_t cap;
    int depth;
    int needs_comma[JSON_MAX_DEPTH];
} json_buf_t;

void json_init(json_buf_t *j);
void json_free(json_buf_t *j);
char *json_finish(json_buf_t *j);

void json_obj_open(json_buf_t *j, const char *key);
void json_obj_close(json_buf_t *j);
void json_arr_open(json_buf_t *j, const char *key);
void json_arr_close(json_buf_t *j);

void json_str(json_buf_t *j, const char *key, const char *val);
void json_int(json_buf_t *j, const char *key, long long val);
void json_float(json_buf_t *j, const char *key, double val, int precision);
void json_bool(json_buf_t *j, const char *key, int val);
void json_null(json_buf_t *j, const char *key);

void json_metric_f(json_buf_t *j, const char *name, double value, int precision, const char *unit);
void json_metric_i(json_buf_t *j, const char *name, long long value, const char *unit);
void json_metric_b(json_buf_t *j, const char *name, int value);
void json_metric_s(json_buf_t *j, const char *name, const char *value, const char *unit);

#endif
