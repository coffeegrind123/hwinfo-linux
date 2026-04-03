#ifndef SYSFS_UTILS_H
#define SYSFS_UTILS_H

#include <stddef.h>

int read_file_str(const char *path, char *buf, size_t bufsz);
long long read_file_ll(const char *path);
double read_file_double(const char *path);
int run_command(const char *cmd, char *buf, size_t bufsz);
int file_exists(const char *path);
int dir_exists(const char *path);
int count_online_cpus(void);
int count_physical_cores(void);

char *find_hwmon_by_name(const char *name, char *pathbuf, size_t bufsz);

double parse_double_field(const char *haystack, const char *field);
long long parse_ll_field(const char *haystack, const char *field);
char *parse_str_field(const char *haystack, const char *field, char *out, size_t outsz);

int glob_count(const char *pattern);
int glob_first(const char *pattern, char *out, size_t outsz);

#endif
