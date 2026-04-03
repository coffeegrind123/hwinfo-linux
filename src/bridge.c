#include "json_builder.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <errno.h>

#define MAX_RESPONSE (4 * 1024 * 1024)
#define MAX_SENSORS 512
#define MAX_READINGS 2048

enum reading_type {
    RT_NONE = 0,
    RT_TEMP = 1,
    RT_VOLTAGE = 2,
    RT_FAN = 3,
    RT_CURRENT = 4,
    RT_POWER = 5,
    RT_CLOCK = 6,
    RT_USAGE = 7,
    RT_OTHER = 8
};

typedef struct {
    int entry_index;
    unsigned int sensor_id;
    int sensor_inst;
    char name_original[256];
    char name_user[256];
} sensor_t;

typedef struct {
    int entry_index;
    int reading_type;
    int sensor_index;
    unsigned int reading_id;
    char label_original[256];
    char label_user[256];
    char unit[32];
    double value;
    double value_min;
    double value_max;
    double value_avg;
} reading_t;

typedef struct {
    sensor_t sensors[MAX_SENSORS];
    int sensor_count;
    reading_t readings[MAX_READINGS];
    int reading_count;
    long long poll_time;
} hwinfo_data_t;

static char *http_get(const char *host, int port, const char *path)
{
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { perror("socket"); return NULL; }

    struct timeval tv = {.tv_sec = 5, .tv_usec = 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    struct hostent *he = gethostbyname(host);
    if (he) {
        memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);
    } else {
        addr.sin_addr.s_addr = inet_addr(host);
    }

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(sock);
        return NULL;
    }

    char request[512];
    snprintf(request, sizeof(request),
             "GET %s HTTP/1.1\r\nHost: %s:%d\r\nConnection: close\r\n\r\n",
             path, host, port);

    if (send(sock, request, strlen(request), 0) < 0) {
        perror("send");
        close(sock);
        return NULL;
    }

    char *buf = malloc(MAX_RESPONSE);
    if (!buf) { close(sock); return NULL; }

    size_t total = 0;
    while (total < MAX_RESPONSE - 1) {
        ssize_t n = recv(sock, buf + total, MAX_RESPONSE - 1 - total, 0);
        if (n <= 0) break;
        total += n;
    }
    buf[total] = '\0';
    close(sock);

    char *body = strstr(buf, "\r\n\r\n");
    if (body) {
        body += 4;
        memmove(buf, body, strlen(body) + 1);
    }

    return buf;
}

/*
 * Minimal JSON parser — just enough for the RemoteHWInfo schema.
 * Not a general-purpose parser; tailored to known structure.
 */

static const char *skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

static const char *parse_json_string(const char *p, char *out, size_t outsz)
{
    p = skip_ws(p);
    if (*p != '"') return NULL;
    p++;
    size_t i = 0;
    while (*p && *p != '"') {
        if (*p == '\\' && *(p+1)) {
            p++;
            switch (*p) {
            case '"': case '\\': case '/':
                if (i < outsz - 1) out[i++] = *p;
                break;
            case 'n':
                if (i < outsz - 1) out[i++] = '\n';
                break;
            case 't':
                if (i < outsz - 1) out[i++] = '\t';
                break;
            default:
                if (i < outsz - 1) out[i++] = *p;
                break;
            }
        } else {
            if (i < outsz - 1) out[i++] = *p;
        }
        p++;
    }
    out[i] = '\0';
    if (*p == '"') p++;
    return p;
}

static const char *parse_json_number(const char *p, double *out)
{
    p = skip_ws(p);
    if (*p == '"') {
        /* Handle quoted numbers like "Infinity", "NaN", or quoted numeric strings */
        char buf[64];
        p = parse_json_string(p, buf, sizeof(buf));
        if (strcmp(buf, "Infinity") == 0 || strcmp(buf, "-Infinity") == 0 ||
            strcmp(buf, "NaN") == 0) {
            *out = 0.0;
        } else {
            *out = atof(buf);
        }
        return p;
    }
    char *end;
    *out = strtod(p, &end);
    return end;
}

static const char *parse_json_int(const char *p, int *out)
{
    double d;
    p = parse_json_number(p, &d);
    *out = (int)d;
    return p;
}

static const char *parse_json_uint(const char *p, unsigned int *out)
{
    double d;
    p = parse_json_number(p, &d);
    *out = (unsigned int)d;
    return p;
}

static const char *parse_json_ll(const char *p, long long *out)
{
    p = skip_ws(p);
    char *end;
    *out = strtoll(p, &end, 10);
    return end;
}

static const char *skip_json_value(const char *p)
{
    p = skip_ws(p);
    if (*p == '"') {
        p++;
        while (*p && *p != '"') { if (*p == '\\') p++; p++; }
        if (*p == '"') p++;
        return p;
    }
    if (*p == '{') {
        int depth = 1; p++;
        while (*p && depth > 0) {
            if (*p == '{') depth++;
            else if (*p == '}') depth--;
            else if (*p == '"') { p++; while (*p && *p != '"') { if (*p == '\\') p++; p++; } }
            p++;
        }
        return p;
    }
    if (*p == '[') {
        int depth = 1; p++;
        while (*p && depth > 0) {
            if (*p == '[') depth++;
            else if (*p == ']') depth--;
            else if (*p == '"') { p++; while (*p && *p != '"') { if (*p == '\\') p++; p++; } }
            p++;
        }
        return p;
    }
    /* number, bool, null */
    while (*p && *p != ',' && *p != '}' && *p != ']' && *p != '\n') p++;
    return p;
}

static int parse_hwinfo_data(const char *json, hwinfo_data_t *data)
{
    memset(data, 0, sizeof(*data));

    const char *p = skip_ws(json);
    if (*p != '{') return -1;
    p++;

    /* Find "hwinfo" key */
    const char *hwinfo = strstr(p, "\"hwinfo\"");
    if (!hwinfo) return -1;

    /* Find pollTime */
    const char *pt = strstr(hwinfo, "\"pollTime\"");
    if (pt) {
        pt += 10;
        pt = skip_ws(pt);
        if (*pt == ':') pt++;
        pt = parse_json_ll(pt, &data->poll_time);
    }

    /* Parse sensors array */
    const char *sensors_start = strstr(hwinfo, "\"sensors\"");
    if (!sensors_start) return -1;
    sensors_start = strchr(sensors_start, '[');
    if (!sensors_start) return -1;
    p = sensors_start + 1;

    while (data->sensor_count < MAX_SENSORS) {
        p = skip_ws(p);
        if (*p == ']') { p++; break; }
        if (*p == ',') { p++; continue; }
        if (*p != '{') break;
        p++;

        sensor_t *s = &data->sensors[data->sensor_count];
        while (*p && *p != '}') {
            p = skip_ws(p);
            if (*p == ',') { p++; continue; }
            if (*p != '"') break;

            char key[64];
            p = parse_json_string(p, key, sizeof(key));
            p = skip_ws(p);
            if (*p == ':') p++;
            p = skip_ws(p);

            if (strcmp(key, "entryIndex") == 0)
                p = parse_json_int(p, &s->entry_index);
            else if (strcmp(key, "sensorId") == 0)
                p = parse_json_uint(p, &s->sensor_id);
            else if (strcmp(key, "sensorInst") == 0)
                p = parse_json_int(p, &s->sensor_inst);
            else if (strcmp(key, "sensorNameOriginal") == 0)
                p = parse_json_string(p, s->name_original, sizeof(s->name_original));
            else if (strcmp(key, "sensorNameUser") == 0)
                p = parse_json_string(p, s->name_user, sizeof(s->name_user));
            else
                p = skip_json_value(p);
        }
        if (*p == '}') p++;
        data->sensor_count++;
    }

    /* Parse readings array */
    const char *readings_start = strstr(p, "\"readings\"");
    if (!readings_start) readings_start = strstr(hwinfo, "\"readings\"");
    if (!readings_start) return -1;
    readings_start = strchr(readings_start, '[');
    if (!readings_start) return -1;
    p = readings_start + 1;

    while (data->reading_count < MAX_READINGS) {
        p = skip_ws(p);
        if (*p == ']') { p++; break; }
        if (*p == ',') { p++; continue; }
        if (*p != '{') break;
        p++;

        reading_t *r = &data->readings[data->reading_count];
        while (*p && *p != '}') {
            p = skip_ws(p);
            if (*p == ',') { p++; continue; }
            if (*p != '"') break;

            char key[64];
            p = parse_json_string(p, key, sizeof(key));
            p = skip_ws(p);
            if (*p == ':') p++;
            p = skip_ws(p);

            if (strcmp(key, "entryIndex") == 0)
                p = parse_json_int(p, &r->entry_index);
            else if (strcmp(key, "readingType") == 0)
                p = parse_json_int(p, &r->reading_type);
            else if (strcmp(key, "sensorIndex") == 0)
                p = parse_json_int(p, &r->sensor_index);
            else if (strcmp(key, "readingId") == 0)
                p = parse_json_uint(p, &r->reading_id);
            else if (strcmp(key, "labelOriginal") == 0)
                p = parse_json_string(p, r->label_original, sizeof(r->label_original));
            else if (strcmp(key, "labelUser") == 0)
                p = parse_json_string(p, r->label_user, sizeof(r->label_user));
            else if (strcmp(key, "unit") == 0)
                p = parse_json_string(p, r->unit, sizeof(r->unit));
            else if (strcmp(key, "value") == 0)
                p = parse_json_number(p, &r->value);
            else if (strcmp(key, "valueMin") == 0)
                p = parse_json_number(p, &r->value_min);
            else if (strcmp(key, "valueMax") == 0)
                p = parse_json_number(p, &r->value_max);
            else if (strcmp(key, "valueAvg") == 0)
                p = parse_json_number(p, &r->value_avg);
            else
                p = skip_json_value(p);
        }
        if (*p == '}') p++;
        data->reading_count++;
    }

    return 0;
}

static const char *sensor_name_for_reading(const hwinfo_data_t *data, const reading_t *r)
{
    if (r->sensor_index >= 0 && r->sensor_index < data->sensor_count) {
        const char *name = data->sensors[r->sensor_index].name_user;
        if (name[0]) return name;
        return data->sensors[r->sensor_index].name_original;
    }
    return "Unknown";
}

static const char *reading_label(const reading_t *r)
{
    if (r->label_user[0]) return r->label_user;
    return r->label_original;
}

static void sanitize_key(const char *in, char *out, size_t outsz)
{
    size_t i = 0;
    for (; *in && i < outsz - 1; in++) {
        char c = *in;
        if (c == ' ' || c == '/' || c == '\\' || c == '(' || c == ')' ||
            c == '[' || c == ']' || c == '#' || c == ':' || c == '.')
            c = '_';
        if (c == '%') c = 'p';
        if ((unsigned char)c < 0x20) continue;
        out[i++] = c;
    }
    /* Trim trailing underscores */
    while (i > 0 && out[i-1] == '_') i--;
    out[i] = '\0';
}

static void emit_reading_metric(json_buf_t *j, const reading_t *r)
{
    char key[256];
    sanitize_key(reading_label(r), key, sizeof(key));

    json_obj_open(j, key);
    json_float(j, "current", r->value, 3);
    json_float(j, "min", r->value_min, 3);
    json_float(j, "max", r->value_max, 3);
    json_float(j, "avg", r->value_avg, 3);
    json_str(j, "unit", r->unit);
    json_str(j, "label", reading_label(r));
    json_obj_close(j);
}

static const char *reading_type_name(int rt)
{
    switch (rt) {
    case RT_TEMP:    return "temperatures";
    case RT_VOLTAGE: return "voltages";
    case RT_FAN:     return "fans";
    case RT_CURRENT: return "currents";
    case RT_POWER:   return "power";
    case RT_CLOCK:   return "clocks";
    case RT_USAGE:   return "usage";
    case RT_OTHER:   return "other";
    default:         return "unknown";
    }
}

static void emit_by_sensor_and_type(json_buf_t *j, const hwinfo_data_t *data)
{
    /* Group readings by sensor, then by type within each sensor */
    for (int si = 0; si < data->sensor_count; si++) {
        const sensor_t *sensor = &data->sensors[si];
        const char *sname = sensor->name_user[0] ? sensor->name_user : sensor->name_original;

        /* Check if this sensor has any readings */
        int has_readings = 0;
        for (int ri = 0; ri < data->reading_count; ri++) {
            if (data->readings[ri].sensor_index == si) { has_readings = 1; break; }
        }
        if (!has_readings) continue;

        char sensor_key[256];
        sanitize_key(sname, sensor_key, sizeof(sensor_key));
        json_obj_open(j, sensor_key);
        json_str(j, "sensor_name", sname);
        json_int(j, "sensor_id", sensor->sensor_id);
        json_int(j, "sensor_instance", sensor->sensor_inst);

        /* Emit readings grouped by type */
        for (int rt = RT_TEMP; rt <= RT_OTHER; rt++) {
            int has_type = 0;
            for (int ri = 0; ri < data->reading_count; ri++) {
                if (data->readings[ri].sensor_index == si && data->readings[ri].reading_type == rt) {
                    has_type = 1; break;
                }
            }
            if (!has_type) continue;

            json_obj_open(j, reading_type_name(rt));
            for (int ri = 0; ri < data->reading_count; ri++) {
                const reading_t *r = &data->readings[ri];
                if (r->sensor_index == si && r->reading_type == rt)
                    emit_reading_metric(j, r);
            }
            json_obj_close(j);
        }

        json_obj_close(j);
    }
}

static void emit_flat(json_buf_t *j, const hwinfo_data_t *data)
{
    /* Flat list grouped by reading type across all sensors */
    for (int rt = RT_TEMP; rt <= RT_OTHER; rt++) {
        int has_type = 0;
        for (int ri = 0; ri < data->reading_count; ri++) {
            if (data->readings[ri].reading_type == rt) { has_type = 1; break; }
        }
        if (!has_type) continue;

        json_obj_open(j, reading_type_name(rt));
        for (int ri = 0; ri < data->reading_count; ri++) {
            const reading_t *r = &data->readings[ri];
            if (r->reading_type != rt) continue;

            const char *sname = sensor_name_for_reading(data, r);
            char key[768];
            char sensor_key[256], label_key[256];
            sanitize_key(sname, sensor_key, sizeof(sensor_key));
            sanitize_key(reading_label(r), label_key, sizeof(label_key));
            snprintf(key, sizeof(key), "%s__%s", sensor_key, label_key);

            json_obj_open(j, key);
            json_str(j, "sensor", sname);
            json_str(j, "label", reading_label(r));
            json_float(j, "current", r->value, 3);
            json_float(j, "min", r->value_min, 3);
            json_float(j, "max", r->value_max, 3);
            json_float(j, "avg", r->value_avg, 3);
            json_str(j, "unit", r->unit);
            json_obj_close(j);
        }
        json_obj_close(j);
    }
}

static void print_usage(const char *prog)
{
    fprintf(stderr, "Usage: %s [OPTIONS]\n", prog);
    fprintf(stderr, "Bridge: fetch HWiNFO sensor data from RemoteHWInfo and output JSON\n\n");
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  -H, --host HOST   RemoteHWInfo host (default: host.docker.internal)\n");
    fprintf(stderr, "  -P, --port PORT   RemoteHWInfo port (default: 60000)\n");
    fprintf(stderr, "  -f, --flat        Flat output grouped by reading type (default)\n");
    fprintf(stderr, "  -s, --by-sensor   Output grouped by sensor, then by type\n");
    fprintf(stderr, "  -p, --pretty      Pretty-print JSON\n");
    fprintf(stderr, "  -r, --repeat N    Repeat N times (0=infinite)\n");
    fprintf(stderr, "  -i, --interval S  Interval between repeats (default: 1)\n");
    fprintf(stderr, "  -h, --help        Show this help\n");
}

static void pretty_print(const char *output)
{
    int indent = 0;
    int in_string = 0;
    for (const char *p = output; *p; p++) {
        if (*p == '"' && (p == output || *(p-1) != '\\'))
            in_string = !in_string;
        if (in_string) { putchar(*p); continue; }
        switch (*p) {
        case '{': case '[':
            putchar(*p); putchar('\n'); indent++;
            for (int i = 0; i < indent; i++) printf("  ");
            break;
        case '}': case ']':
            putchar('\n'); indent--;
            for (int i = 0; i < indent; i++) printf("  ");
            putchar(*p);
            break;
        case ',':
            putchar(','); putchar('\n');
            for (int i = 0; i < indent; i++) printf("  ");
            break;
        case ':':
            putchar(':'); putchar(' ');
            break;
        default:
            putchar(*p);
        }
    }
    putchar('\n');
}

int main(int argc, char *argv[])
{
    const char *host = "host.docker.internal";
    int port = 60000;
    int pretty = 0;
    int by_sensor = 0;
    int repeat = 1;
    int interval = 1;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]); return 0;
        } else if ((strcmp(argv[i], "-H") == 0 || strcmp(argv[i], "--host") == 0) && i+1 < argc) {
            host = argv[++i];
        } else if ((strcmp(argv[i], "-P") == 0 || strcmp(argv[i], "--port") == 0) && i+1 < argc) {
            port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--pretty") == 0) {
            pretty = 1;
        } else if (strcmp(argv[i], "-f") == 0 || strcmp(argv[i], "--flat") == 0) {
            by_sensor = 0;
        } else if (strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "--by-sensor") == 0) {
            by_sensor = 1;
        } else if ((strcmp(argv[i], "-r") == 0 || strcmp(argv[i], "--repeat") == 0) && i+1 < argc) {
            repeat = atoi(argv[++i]);
        } else if ((strcmp(argv[i], "-i") == 0 || strcmp(argv[i], "--interval") == 0) && i+1 < argc) {
            interval = atoi(argv[++i]);
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage(argv[0]); return 1;
        }
    }

    for (int r = 0; repeat == 0 || r < repeat; r++) {
        char *raw = http_get(host, port, "/json.json");
        if (!raw) {
            fprintf(stderr, "Failed to fetch from %s:%d/json.json\n", host, port);
            if (repeat == 0) { sleep(interval); continue; }
            return 1;
        }

        hwinfo_data_t data;
        if (parse_hwinfo_data(raw, &data) < 0) {
            fprintf(stderr, "Failed to parse RemoteHWInfo JSON response\n");
            free(raw);
            if (repeat == 0) { sleep(interval); continue; }
            return 1;
        }
        free(raw);

        json_buf_t j;
        json_init(&j);
        json_obj_open(&j, NULL);

        /* Metadata */
        json_obj_open(&j, "system_info");
        json_str(&j, "source", "RemoteHWInfo bridge");
        char endpoint[256];
        snprintf(endpoint, sizeof(endpoint), "http://%s:%d/json.json", host, port);
        json_str(&j, "endpoint", endpoint);

        time_t now = time(NULL);
        struct tm *tm = gmtime(&now);
        char ts[64];
        strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", tm);
        json_str(&j, "timestamp", ts);

        json_int(&j, "hwinfo_poll_time", data.poll_time);
        json_int(&j, "sensor_count", data.sensor_count);
        json_int(&j, "reading_count", data.reading_count);
        json_obj_close(&j);

        if (by_sensor)
            emit_by_sensor_and_type(&j, &data);
        else
            emit_flat(&j, &data);

        json_obj_close(&j);

        char *output = json_finish(&j);
        if (pretty)
            pretty_print(output);
        else
            fputs(output, stdout);
        free(output);
        fflush(stdout);

        if ((repeat == 0 || r < repeat - 1))
            sleep(interval);
    }

    return 0;
}
