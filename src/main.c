#include "json_builder.h"
#include "sysfs_utils.h"
#include "collectors.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/utsname.h>

static void print_usage(const char *prog)
{
    fprintf(stderr, "Usage: %s [OPTIONS]\n", prog);
    fprintf(stderr, "Linux hardware monitoring tool (HWiNFO64-equivalent)\n\n");
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  -a, --all         Collect all metrics (default)\n");
    fprintf(stderr, "  -c, --category CAT  Collect specific category\n");
    fprintf(stderr, "  -l, --list        List available categories\n");
    fprintf(stderr, "  -p, --pretty      Pretty-print JSON output\n");
    fprintf(stderr, "  -r, --repeat N    Repeat collection N times (0=infinite)\n");
    fprintf(stderr, "  -i, --interval S  Interval between repeats in seconds (default: 1)\n");
    fprintf(stderr, "  -h, --help        Show this help\n");
    fprintf(stderr, "\nCategories:\n");
    fprintf(stderr, "  memory, cpu_voltages, cpu_clocks, cpu_ratios, cpu_cstates,\n");
    fprintf(stderr, "  mem_timings, cpu_temps, cpu_power, cpu_clocks_enh, cpu_limits,\n");
    fprintf(stderr, "  cpu_bandwidth, motherboard, chipset, ec_sensors, dimm_temps,\n");
    fprintf(stderr, "  smart_hdd, smart_nvme, disk_io, gpu, network, hw_errors\n");
}

typedef struct {
    const char *name;
    void (*collect)(json_buf_t *);
} collector_entry_t;

static const collector_entry_t collectors[] = {
    {"memory",          collect_system_memory},
    {"cpu_voltages",    collect_cpu_voltages},
    {"cpu_clocks",      collect_cpu_clocks_usage},
    {"cpu_ratios",      collect_cpu_ratios},
    {"cpu_cstates",     collect_cpu_cstates},
    {"mem_timings",     collect_memory_timings},
    {"cpu_temps",       collect_cpu_temperatures},
    {"cpu_power",       collect_cpu_power},
    {"cpu_clocks_enh",  collect_cpu_clocks_enhanced},
    {"cpu_limits",      collect_cpu_limits},
    {"cpu_bandwidth",   collect_cpu_bandwidth},
    {"motherboard",     collect_motherboard_sensors},
    {"chipset",         collect_chipset},
    {"ec_sensors",      collect_ec_sensors},
    {"dimm_temps",      collect_dimm_temps},
    {"smart_hdd",       collect_smart_hdd},
    {"smart_nvme",      collect_smart_nvme},
    {"disk_io",         collect_disk_io},
    {"gpu",             collect_gpu_nvidia},
    {"network",         collect_network},
    {"hw_errors",       collect_hw_errors},
    {NULL, NULL}
};

static void collect_system_info(json_buf_t *j)
{
    json_obj_open(j, "system_info");

    struct utsname un;
    if (uname(&un) == 0) {
        json_str(j, "hostname", un.nodename);
        json_str(j, "kernel", un.release);
        json_str(j, "arch", un.machine);
        json_str(j, "os", un.sysname);
    }

    time_t now = time(NULL);
    struct tm *tm = gmtime(&now);
    char ts[64];
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", tm);
    json_str(j, "timestamp", ts);

    /* Uptime */
    double uptime = read_file_double("/proc/uptime");
    if (uptime > 0) {
        json_float(j, "uptime_seconds", uptime, 1);
        int days = (int)(uptime / 86400);
        int hours = (int)((uptime - days * 86400) / 3600);
        int mins = (int)((uptime - days * 86400 - hours * 3600) / 60);
        char upstr[64];
        snprintf(upstr, sizeof(upstr), "%dd %dh %dm", days, hours, mins);
        json_str(j, "uptime_human", upstr);
    }

    /* CPU model */
    char cpuinfo[4096];
    if (read_file_str("/proc/cpuinfo", cpuinfo, sizeof(cpuinfo)) > 0) {
        char model[256];
        if (parse_str_field(cpuinfo, "model name", model, sizeof(model)))
            json_str(j, "cpu_model", model);
    }

    json_int(j, "logical_cpus", count_online_cpus());
    json_int(j, "physical_cores", count_physical_cores());

    json_obj_close(j);
}

static void do_collection(const char *category, int pretty)
{
    json_buf_t j;
    json_init(&j);

    json_obj_open(&j, NULL);

    collect_system_info(&j);

    if (!category) {
        for (int i = 0; collectors[i].name; i++)
            collectors[i].collect(&j);
    } else {
        int found = 0;
        /* Support comma-separated categories */
        char cats[1024];
        strncpy(cats, category, sizeof(cats) - 1);
        cats[sizeof(cats) - 1] = '\0';

        char *tok = strtok(cats, ",");
        while (tok) {
            while (*tok == ' ') tok++;
            for (int i = 0; collectors[i].name; i++) {
                if (strcmp(collectors[i].name, tok) == 0) {
                    collectors[i].collect(&j);
                    found = 1;
                    break;
                }
            }
            if (!found) {
                fprintf(stderr, "Unknown category: %s\n", tok);
            }
            found = 0;
            tok = strtok(NULL, ",");
        }
    }

    json_obj_close(&j);

    char *output = json_finish(&j);

    if (pretty) {
        /* Simple pretty-printer: add newlines and indentation */
        int indent = 0;
        int in_string = 0;
        for (const char *p = output; *p; p++) {
            if (*p == '"' && (p == output || *(p-1) != '\\'))
                in_string = !in_string;

            if (in_string) {
                putchar(*p);
                continue;
            }

            switch (*p) {
            case '{':
            case '[':
                putchar(*p);
                putchar('\n');
                indent++;
                for (int i = 0; i < indent; i++) printf("  ");
                break;
            case '}':
            case ']':
                putchar('\n');
                indent--;
                for (int i = 0; i < indent; i++) printf("  ");
                putchar(*p);
                break;
            case ',':
                putchar(',');
                putchar('\n');
                for (int i = 0; i < indent; i++) printf("  ");
                break;
            case ':':
                putchar(':');
                putchar(' ');
                break;
            default:
                putchar(*p);
            }
        }
        putchar('\n');
    } else {
        fputs(output, stdout);
    }

    free(output);
}

int main(int argc, char *argv[])
{
    const char *category = NULL;
    int pretty = 0;
    int repeat = 1;
    int interval = 1;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "-l") == 0 || strcmp(argv[i], "--list") == 0) {
            printf("Available categories:\n");
            for (int c = 0; collectors[c].name; c++)
                printf("  %s\n", collectors[c].name);
            return 0;
        } else if ((strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--category") == 0) && i + 1 < argc) {
            category = argv[++i];
        } else if (strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--pretty") == 0) {
            pretty = 1;
        } else if ((strcmp(argv[i], "-r") == 0 || strcmp(argv[i], "--repeat") == 0) && i + 1 < argc) {
            repeat = atoi(argv[++i]);
        } else if ((strcmp(argv[i], "-i") == 0 || strcmp(argv[i], "--interval") == 0) && i + 1 < argc) {
            interval = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-a") == 0 || strcmp(argv[i], "--all") == 0) {
            category = NULL;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }

    if (repeat == 0) {
        /* Infinite loop */
        while (1) {
            do_collection(category, pretty);
            fflush(stdout);
            sleep(interval);
        }
    } else {
        for (int r = 0; r < repeat; r++) {
            do_collection(category, pretty);
            fflush(stdout);
            if (r < repeat - 1)
                sleep(interval);
        }
    }

    return 0;
}
