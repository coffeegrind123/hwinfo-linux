#include "json_builder.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INITIAL_CAP 4096

static void ensure_cap(json_buf_t *j, size_t need)
{
    if (j->len + need >= j->cap) {
        while (j->len + need >= j->cap)
            j->cap *= 2;
        j->data = realloc(j->data, j->cap);
    }
}

static void append(json_buf_t *j, const char *s, size_t n)
{
    ensure_cap(j, n);
    memcpy(j->data + j->len, s, n);
    j->len += n;
    j->data[j->len] = '\0';
}

static void append_str(json_buf_t *j, const char *s)
{
    append(j, s, strlen(s));
}

static void maybe_comma(json_buf_t *j)
{
    if (j->needs_comma[j->depth]) {
        append(j, ",", 1);
    }
    j->needs_comma[j->depth] = 1;
}

static void append_escaped(json_buf_t *j, const char *s)
{
    append(j, "\"", 1);
    for (; *s; s++) {
        switch (*s) {
        case '"':  append(j, "\\\"", 2); break;
        case '\\': append(j, "\\\\", 2); break;
        case '\n': append(j, "\\n", 2);  break;
        case '\r': append(j, "\\r", 2);  break;
        case '\t': append(j, "\\t", 2);  break;
        default:
            if ((unsigned char)*s < 0x20) {
                char esc[8];
                snprintf(esc, sizeof(esc), "\\u%04x", (unsigned char)*s);
                append_str(j, esc);
            } else {
                append(j, s, 1);
            }
        }
    }
    append(j, "\"", 1);
}

static void write_key(json_buf_t *j, const char *key)
{
    maybe_comma(j);
    if (key) {
        append_escaped(j, key);
        append(j, ":", 1);
    }
}

void json_init(json_buf_t *j)
{
    j->data = malloc(INITIAL_CAP);
    j->data[0] = '\0';
    j->len = 0;
    j->cap = INITIAL_CAP;
    j->depth = 0;
    memset(j->needs_comma, 0, sizeof(j->needs_comma));
}

void json_free(json_buf_t *j)
{
    free(j->data);
    j->data = NULL;
    j->len = 0;
    j->cap = 0;
}

char *json_finish(json_buf_t *j)
{
    append(j, "\n", 1);
    char *result = j->data;
    j->data = NULL;
    j->len = 0;
    j->cap = 0;
    return result;
}

void json_obj_open(json_buf_t *j, const char *key)
{
    write_key(j, key);
    append(j, "{", 1);
    j->depth++;
    j->needs_comma[j->depth] = 0;
}

void json_obj_close(json_buf_t *j)
{
    j->depth--;
    append(j, "}", 1);
}

void json_arr_open(json_buf_t *j, const char *key)
{
    write_key(j, key);
    append(j, "[", 1);
    j->depth++;
    j->needs_comma[j->depth] = 0;
}

void json_arr_close(json_buf_t *j)
{
    j->depth--;
    append(j, "]", 1);
}

void json_str(json_buf_t *j, const char *key, const char *val)
{
    write_key(j, key);
    if (val)
        append_escaped(j, val);
    else
        append_str(j, "null");
}

void json_int(json_buf_t *j, const char *key, long long val)
{
    write_key(j, key);
    char buf[32];
    snprintf(buf, sizeof(buf), "%lld", val);
    append_str(j, buf);
}

void json_float(json_buf_t *j, const char *key, double val, int precision)
{
    write_key(j, key);
    char buf[64];
    snprintf(buf, sizeof(buf), "%.*f", precision, val);
    append_str(j, buf);
}

void json_bool(json_buf_t *j, const char *key, int val)
{
    write_key(j, key);
    append_str(j, val ? "true" : "false");
}

void json_null(json_buf_t *j, const char *key)
{
    write_key(j, key);
    append_str(j, "null");
}

void json_metric_f(json_buf_t *j, const char *name, double value, int precision, const char *unit)
{
    json_obj_open(j, name);
    json_float(j, "current", value, precision);
    json_float(j, "min", value, precision);
    json_float(j, "max", value, precision);
    json_float(j, "avg", value, precision);
    json_str(j, "unit", unit);
    json_obj_close(j);
}

void json_metric_i(json_buf_t *j, const char *name, long long value, const char *unit)
{
    json_obj_open(j, name);
    json_int(j, "current", value);
    json_int(j, "min", value);
    json_int(j, "max", value);
    json_int(j, "avg", value);
    json_str(j, "unit", unit);
    json_obj_close(j);
}

void json_metric_b(json_buf_t *j, const char *name, int value)
{
    json_obj_open(j, name);
    json_bool(j, "current", value);
    json_str(j, "unit", "bool");
    json_obj_close(j);
}

void json_metric_s(json_buf_t *j, const char *name, const char *value, const char *unit)
{
    json_obj_open(j, name);
    json_str(j, "current", value);
    json_str(j, "unit", unit);
    json_obj_close(j);
}
