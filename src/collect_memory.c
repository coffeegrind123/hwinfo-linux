#include "collectors.h"
#include "sysfs_utils.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static long long meminfo_kb(const char *meminfo, const char *key)
{
    char search[64];
    snprintf(search, sizeof(search), "%s:", key);
    const char *p = strstr(meminfo, search);
    if (!p) return -1;
    p += strlen(search);
    while (*p == ' ') p++;
    return atoll(p);
}

void collect_system_memory(json_buf_t *j)
{
    char buf[4096];
    if (read_file_str("/proc/meminfo", buf, sizeof(buf)) < 0) return;

    json_obj_open(j, "system_memory");

    long long mem_total = meminfo_kb(buf, "MemTotal");
    (void)meminfo_kb(buf, "MemFree");
    long long mem_avail = meminfo_kb(buf, "MemAvailable");
    long long buffers = meminfo_kb(buf, "Buffers");
    long long cached = meminfo_kb(buf, "Cached");
    long long swap_total = meminfo_kb(buf, "SwapTotal");
    long long swap_free = meminfo_kb(buf, "SwapFree");
    long long vmalloc_total = meminfo_kb(buf, "VmallocTotal");
    long long vmalloc_used = meminfo_kb(buf, "VmallocUsed");
    long long committed = meminfo_kb(buf, "Committed_AS");

    long long mem_used = mem_total - mem_avail;

    if (committed >= 0)
        json_metric_i(j, "virtual_memory_committed", committed / 1024, "MB");

    if (vmalloc_total >= 0 && vmalloc_used >= 0)
        json_metric_i(j, "virtual_memory_available", (vmalloc_total - vmalloc_used) / 1024, "MB");

    if (committed >= 0 && vmalloc_total > 0) {
        double vload = (double)committed / (double)(vmalloc_total) * 100.0;
        if (vload > 100.0) vload = 100.0;
        json_metric_f(j, "virtual_memory_load", vload, 1, "%");
    }

    if (mem_used >= 0)
        json_metric_i(j, "physical_memory_used", mem_used / 1024, "MB");

    if (mem_avail >= 0)
        json_metric_i(j, "physical_memory_available", mem_avail / 1024, "MB");

    if (mem_total > 0 && mem_used >= 0) {
        double pload = (double)mem_used / (double)mem_total * 100.0;
        json_metric_f(j, "physical_memory_load", pload, 1, "%");
    }

    if (swap_total > 0) {
        long long swap_used = swap_total - swap_free;
        double swap_pct = (double)swap_used / (double)swap_total * 100.0;
        json_metric_f(j, "swap_usage", swap_pct, 1, "%");
    } else {
        json_metric_f(j, "swap_usage", 0.0, 1, "%");
    }

    if (mem_total >= 0)
        json_metric_i(j, "total_physical_memory", mem_total / 1024, "MB");
    if (swap_total >= 0)
        json_metric_i(j, "total_swap", swap_total / 1024, "MB");
    if (buffers >= 0)
        json_metric_i(j, "buffers", buffers / 1024, "MB");
    if (cached >= 0)
        json_metric_i(j, "cached", cached / 1024, "MB");

    json_obj_close(j);
}

void collect_memory_timings(json_buf_t *j)
{
    json_obj_open(j, "memory_timings");

    char buf[16384];
    int found = 0;

    if (run_command("dmidecode -t memory 2>/dev/null", buf, sizeof(buf)) >= 0 && buf[0]) {
        found = 1;

        long long speed = parse_ll_field(buf, "Configured Memory Speed");
        if (speed < 0) speed = parse_ll_field(buf, "Speed");
        if (speed > 0)
            json_metric_i(j, "memory_clock", speed / 2, "MHz");

        char type_str[64];
        if (parse_str_field(buf, "Type:", type_str, sizeof(type_str)))
            json_metric_s(j, "memory_type", type_str, "");

        long long size = parse_ll_field(buf, "Total Width");
        if (size > 0)
            json_metric_i(j, "total_width", size, "bits");
    }

    if (run_command("decode-dimms 2>/dev/null", buf, sizeof(buf)) >= 0 && buf[0]) {
        found = 1;
        long long tcas = parse_ll_field(buf, "tCAS");
        long long trcd = parse_ll_field(buf, "tRCD");
        long long trp = parse_ll_field(buf, "tRP");
        long long tras = parse_ll_field(buf, "tRAS");

        if (tcas > 0) json_metric_i(j, "tcas_latency", tcas, "T");
        if (trcd > 0) json_metric_i(j, "trcd", trcd, "T");
        if (trp > 0) json_metric_i(j, "trp", trp, "T");
        if (tras > 0) json_metric_i(j, "tras", tras, "T");
    }

    if (!found) {
        json_str(j, "status", "unavailable (requires root and dmidecode/decode-dimms)");
    }

    json_obj_close(j);
}
