#include "sysfs_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glob.h>
#include <sys/stat.h>
#include <unistd.h>
#include <ctype.h>
#include <dirent.h>

int read_file_str(const char *path, char *buf, size_t bufsz)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    size_t n = fread(buf, 1, bufsz - 1, f);
    buf[n] = '\0';
    while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r'))
        buf[--n] = '\0';
    fclose(f);
    return (int)n;
}

long long read_file_ll(const char *path)
{
    char buf[64];
    if (read_file_str(path, buf, sizeof(buf)) < 0) return -1;
    return atoll(buf);
}

double read_file_double(const char *path)
{
    char buf[64];
    if (read_file_str(path, buf, sizeof(buf)) < 0) return -1.0;
    return atof(buf);
}

int run_command(const char *cmd, char *buf, size_t bufsz)
{
    FILE *p = popen(cmd, "r");
    if (!p) return -1;
    size_t total = 0;
    while (total < bufsz - 1) {
        size_t n = fread(buf + total, 1, bufsz - 1 - total, p);
        if (n == 0) break;
        total += n;
    }
    buf[total] = '\0';
    int ret = pclose(p);
    return ret == 0 ? (int)total : -1;
}

int file_exists(const char *path)
{
    return access(path, R_OK) == 0;
}

int dir_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

int count_online_cpus(void)
{
    int count = 0;
    char buf[8192];
    if (read_file_str("/proc/stat", buf, sizeof(buf)) < 0) return 1;
    char *line = buf;
    while ((line = strstr(line, "cpu")) != NULL) {
        if (line[3] >= '0' && line[3] <= '9')
            count++;
        line += 3;
    }
    return count > 0 ? count : 1;
}

int count_physical_cores(void)
{
    char buf[65536];
    if (read_file_str("/proc/cpuinfo", buf, sizeof(buf)) < 0)
        return count_online_cpus();

    int max_core_id = -1;
    char *p = buf;
    while ((p = strstr(p, "core id")) != NULL) {
        char *colon = strchr(p, ':');
        if (colon) {
            int id = atoi(colon + 1);
            if (id > max_core_id) max_core_id = id;
        }
        p++;
    }
    return max_core_id >= 0 ? max_core_id + 1 : count_online_cpus();
}

char *find_hwmon_by_name(const char *name, char *pathbuf, size_t bufsz)
{
    glob_t g;
    if (glob("/sys/class/hwmon/hwmon*/name", 0, NULL, &g) != 0)
        return NULL;

    for (size_t i = 0; i < g.gl_pathc; i++) {
        char nbuf[128];
        if (read_file_str(g.gl_pathv[i], nbuf, sizeof(nbuf)) > 0) {
            if (strcmp(nbuf, name) == 0) {
                char *last_slash = strrchr(g.gl_pathv[i], '/');
                if (last_slash) {
                    size_t dirlen = last_slash - g.gl_pathv[i];
                    if (dirlen < bufsz) {
                        memcpy(pathbuf, g.gl_pathv[i], dirlen);
                        pathbuf[dirlen] = '\0';
                        globfree(&g);
                        return pathbuf;
                    }
                }
            }
        }
    }
    globfree(&g);
    return NULL;
}

double parse_double_field(const char *haystack, const char *field)
{
    const char *p = strstr(haystack, field);
    if (!p) return -1.0;
    p += strlen(field);
    while (*p && (*p == ':' || *p == ' ' || *p == '\t')) p++;
    return atof(p);
}

long long parse_ll_field(const char *haystack, const char *field)
{
    const char *p = strstr(haystack, field);
    if (!p) return -1;
    p += strlen(field);
    while (*p && (*p == ':' || *p == ' ' || *p == '\t')) p++;
    return atoll(p);
}

char *parse_str_field(const char *haystack, const char *field, char *out, size_t outsz)
{
    const char *p = strstr(haystack, field);
    if (!p) return NULL;
    p += strlen(field);
    while (*p && (*p == ':' || *p == ' ' || *p == '\t')) p++;
    size_t i = 0;
    while (*p && *p != '\n' && i < outsz - 1)
        out[i++] = *p++;
    while (i > 0 && (out[i-1] == ' ' || out[i-1] == '\t'))
        i--;
    out[i] = '\0';
    return out;
}

int glob_count(const char *pattern)
{
    glob_t g;
    if (glob(pattern, GLOB_NOSORT, NULL, &g) != 0) return 0;
    int count = (int)g.gl_pathc;
    globfree(&g);
    return count;
}

int glob_first(const char *pattern, char *out, size_t outsz)
{
    glob_t g;
    if (glob(pattern, GLOB_NOSORT, NULL, &g) != 0) return -1;
    if (g.gl_pathc == 0) { globfree(&g); return -1; }
    strncpy(out, g.gl_pathv[0], outsz - 1);
    out[outsz - 1] = '\0';
    globfree(&g);
    return 0;
}
