#include "collectors.h"
#include "sysfs_utils.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <glob.h>
#include <unistd.h>
#include <time.h>
#include <dirent.h>

static int ncpus_cached = -1;

static int get_ncpus(void)
{
    if (ncpus_cached < 0)
        ncpus_cached = count_online_cpus();
    return ncpus_cached;
}

void collect_cpu_voltages(json_buf_t *j)
{
    json_obj_open(j, "cpu_core_voltages");

    char hwmon_path[256];
    int found = 0;

    const char *drivers[] = {"zenpower", "k10temp", "coretemp", NULL};
    for (int i = 0; drivers[i]; i++) {
        if (find_hwmon_by_name(drivers[i], hwmon_path, sizeof(hwmon_path))) {
            found = 1;
            json_str(j, "driver", drivers[i]);

            char path[512];
            for (int idx = 1; idx <= 32; idx++) {
                snprintf(path, sizeof(path), "%s/in%d_input", hwmon_path, idx);
                if (file_exists(path)) {
                    double v = read_file_double(path) / 1000.0;
                    char label_path[512], label[64], key[64];
                    snprintf(label_path, sizeof(label_path), "%s/in%d_label", hwmon_path, idx);
                    if (read_file_str(label_path, label, sizeof(label)) > 0)
                        snprintf(key, sizeof(key), "%s", label);
                    else
                        snprintf(key, sizeof(key), "in%d", idx);
                    json_metric_f(j, key, v, 3, "V");
                }
            }
            break;
        }
    }

    if (!found) {
        char cmd_buf[8192];
        if (run_command("sensors 2>/dev/null", cmd_buf, sizeof(cmd_buf)) >= 0) {
            char *line = strtok(cmd_buf, "\n");
            while (line) {
                char *vid = strstr(line, "VID");
                if (!vid) vid = strstr(line, "Vcore");
                if (vid) {
                    char *plus = strchr(line, '+');
                    if (plus) {
                        double v = atof(plus + 1);
                        char key[128];
                        char *colon = strchr(line, ':');
                        if (colon) {
                            size_t klen = colon - line;
                            if (klen > sizeof(key) - 1) klen = sizeof(key) - 1;
                            memcpy(key, line, klen);
                            key[klen] = '\0';
                            while (klen > 0 && key[klen-1] == ' ') key[--klen] = '\0';
                            json_metric_f(j, key, v, 3, "V");
                            found = 1;
                        }
                    }
                }
                line = strtok(NULL, "\n");
            }
        }
    }

    if (!found)
        json_str(j, "status", "unavailable (requires lm-sensors with zenpower/k10temp/coretemp)");

    json_obj_close(j);
}

void collect_cpu_clocks_usage(json_buf_t *j)
{
    json_obj_open(j, "cpu_clocks_and_usage");

    int ncpus = get_ncpus();
    json_int(j, "logical_cpus", ncpus);

    double max_freq = 0, min_freq = 1e12, sum_freq = 0;
    int freq_count = 0;

    json_arr_open(j, "per_core_clocks");
    for (int i = 0; i < ncpus; i++) {
        char path[256];
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", i);
        long long freq_khz = read_file_ll(path);
        if (freq_khz > 0) {
            double mhz = freq_khz / 1000.0;
            json_obj_open(j, NULL);
            json_int(j, "core", i);
            json_float(j, "clock_mhz", mhz, 1);
            json_obj_close(j);

            if (mhz > max_freq) max_freq = mhz;
            if (mhz < min_freq) min_freq = mhz;
            sum_freq += mhz;
            freq_count++;
        }
    }
    json_arr_close(j);

    if (freq_count > 0) {
        json_metric_f(j, "average_clock", sum_freq / freq_count, 1, "MHz");
        json_metric_f(j, "max_clock", max_freq, 1, "MHz");
        json_metric_f(j, "min_clock", min_freq, 1, "MHz");
    }

    char path[256];
    snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu0/cpufreq/base_frequency");
    long long base = read_file_ll(path);
    if (base > 0)
        json_metric_f(j, "base_clock", base / 1000.0, 1, "MHz");

    snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu0/cpufreq/bios_limit");
    long long bios_limit = read_file_ll(path);
    if (bios_limit > 0)
        json_metric_f(j, "bios_limit", bios_limit / 1000.0, 1, "MHz");

    /* CPU usage from /proc/stat snapshot */
    char stat1[8192], stat2[8192];
    if (read_file_str("/proc/stat", stat1, sizeof(stat1)) > 0) {
        usleep(100000); /* 100ms sample */
        if (read_file_str("/proc/stat", stat2, sizeof(stat2)) > 0) {
            unsigned long long u1, n1, s1, i1, w1, q1, sq1, st1;
            unsigned long long u2, n2, s2, i2, w2, q2, sq2, st2;

            if (sscanf(stat1, "cpu  %llu %llu %llu %llu %llu %llu %llu %llu",
                       &u1, &n1, &s1, &i1, &w1, &q1, &sq1, &st1) >= 4 &&
                sscanf(stat2, "cpu  %llu %llu %llu %llu %llu %llu %llu %llu",
                       &u2, &n2, &s2, &i2, &w2, &q2, &sq2, &st2) >= 4) {
                unsigned long long total1 = u1 + n1 + s1 + i1 + w1 + q1 + sq1 + st1;
                unsigned long long total2 = u2 + n2 + s2 + i2 + w2 + q2 + sq2 + st2;
                unsigned long long idle_d = i2 - i1;
                unsigned long long total_d = total2 - total1;

                if (total_d > 0) {
                    double usage = (1.0 - (double)idle_d / (double)total_d) * 100.0;
                    json_metric_f(j, "total_cpu_usage", usage, 1, "%");
                }
            }

            /* Per-core usage */
            json_arr_open(j, "per_core_usage");
            for (int i = 0; i < ncpus && i < 256; i++) {
                char key[16];
                snprintf(key, sizeof(key), "cpu%d", i);

                char *line1 = strstr(stat1, key);
                char *line2 = strstr(stat2, key);
                if (!line1 || !line2) continue;

                /* Make sure we match exact "cpuN " not "cpuNN" */
                if (line1[strlen(key)] != ' ') continue;

                unsigned long long cu1, cn1, cs1, ci1, cw1, cq1, csq1, cst1;
                unsigned long long cu2, cn2, cs2, ci2, cw2, cq2, csq2, cst2;

                if (sscanf(line1 + strlen(key), " %llu %llu %llu %llu %llu %llu %llu %llu",
                           &cu1, &cn1, &cs1, &ci1, &cw1, &cq1, &csq1, &cst1) >= 4 &&
                    sscanf(line2 + strlen(key), " %llu %llu %llu %llu %llu %llu %llu %llu",
                           &cu2, &cn2, &cs2, &ci2, &cw2, &cq2, &csq2, &cst2) >= 4) {
                    unsigned long long t1 = cu1 + cn1 + cs1 + ci1 + cw1 + cq1 + csq1 + cst1;
                    unsigned long long t2 = cu2 + cn2 + cs2 + ci2 + cw2 + cq2 + csq2 + cst2;
                    unsigned long long di = ci2 - ci1;
                    unsigned long long dt = t2 - t1;
                    if (dt > 0) {
                        double u = (1.0 - (double)di / (double)dt) * 100.0;
                        json_obj_open(j, NULL);
                        json_int(j, "core", i);
                        json_float(j, "usage_pct", u, 1);
                        json_obj_close(j);
                    }
                }
            }
            json_arr_close(j);
        }
    }

    json_obj_close(j);
}

void collect_cpu_ratios(json_buf_t *j)
{
    json_obj_open(j, "cpu_core_ratios");

    int ncpus = get_ncpus();
    char path[256];

    long long base_freq = 0;
    snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu0/cpufreq/base_frequency");
    base_freq = read_file_ll(path);
    if (base_freq <= 0) {
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_min_freq");
        base_freq = read_file_ll(path);
    }

    double bus_clock = 100.0; /* standard bus clock */
    json_metric_f(j, "bus_clock", bus_clock, 1, "MHz");

    json_arr_open(j, "per_core_ratios");
    for (int i = 0; i < ncpus; i++) {
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", i);
        long long freq = read_file_ll(path);
        if (freq > 0) {
            double ratio = (freq / 1000.0) / bus_clock;
            json_obj_open(j, NULL);
            json_int(j, "core", i);
            json_float(j, "ratio", ratio, 1);
            json_float(j, "freq_mhz", freq / 1000.0, 1);
            json_obj_close(j);
        }
    }
    json_arr_close(j);

    json_obj_close(j);
}

void collect_cpu_cstates(json_buf_t *j)
{
    json_obj_open(j, "cpu_cstate_residency");

    int ncpus = get_ncpus();
    char path[512];

    /* Try turbostat first */
    char turbo_buf[16384];
    if (run_command("turbostat --show PkgWatt,Pkg%pc2,Pkg%pc6,CoreTmp,Core%c0,Core%c1,Core%c6 --num_iterations 1 --interval 0.1 2>/dev/null", turbo_buf, sizeof(turbo_buf)) >= 0 && turbo_buf[0]) {
        json_str(j, "source", "turbostat");
        json_str(j, "raw_output_sample", turbo_buf);
    }

    /* Read cpuidle stats */
    json_arr_open(j, "per_core_cstates");
    for (int cpu = 0; cpu < ncpus && cpu < 64; cpu++) {
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpuidle", cpu);
        if (!dir_exists(path)) continue;

        json_obj_open(j, NULL);
        json_int(j, "cpu", cpu);

        json_arr_open(j, "states");
        for (int s = 0; s < 16; s++) {
            char name_path[768], time_path[768], usage_path[768];
            snprintf(name_path, sizeof(name_path), "%s/state%d/name", path, s);
            snprintf(time_path, sizeof(time_path), "%s/state%d/time", path, s);
            snprintf(usage_path, sizeof(usage_path), "%s/state%d/usage", path, s);

            if (!file_exists(name_path)) break;

            char name[64];
            read_file_str(name_path, name, sizeof(name));
            long long time_us = read_file_ll(time_path);
            long long usage_count = read_file_ll(usage_path);

            json_obj_open(j, NULL);
            json_str(j, "state", name);
            json_int(j, "time_us", time_us);
            json_int(j, "usage_count", usage_count);
            json_obj_close(j);
        }
        json_arr_close(j);

        json_obj_close(j);
    }
    json_arr_close(j);

    json_obj_close(j);
}

void collect_cpu_temperatures(json_buf_t *j)
{
    json_obj_open(j, "cpu_temperatures");

    char hwmon_path[256];
    const char *drivers[] = {"k10temp", "zenpower", "coretemp", NULL};
    int found = 0;

    for (int d = 0; drivers[d]; d++) {
        if (!find_hwmon_by_name(drivers[d], hwmon_path, sizeof(hwmon_path)))
            continue;

        found = 1;
        json_str(j, "driver", drivers[d]);

        for (int idx = 1; idx <= 32; idx++) {
            char path[512], label_path[512], label[64];
            snprintf(path, sizeof(path), "%s/temp%d_input", hwmon_path, idx);
            if (!file_exists(path)) continue;

            double temp = read_file_double(path) / 1000.0;
            snprintf(label_path, sizeof(label_path), "%s/temp%d_label", hwmon_path, idx);

            char key[128];
            if (read_file_str(label_path, label, sizeof(label)) > 0)
                snprintf(key, sizeof(key), "%s", label);
            else
                snprintf(key, sizeof(key), "temp%d", idx);

            json_metric_f(j, key, temp, 1, "°C");
        }
        break;
    }

    if (!found) {
        /* Fallback: search all hwmon for any temp sensors */
        glob_t g;
        if (glob("/sys/class/hwmon/hwmon*/temp*_input", 0, NULL, &g) == 0) {
            for (size_t i = 0; i < g.gl_pathc && i < 32; i++) {
                double temp = read_file_double(g.gl_pathv[i]) / 1000.0;
                char label_path[512], label[64];
                strncpy(label_path, g.gl_pathv[i], sizeof(label_path) - 1);
                char *underscore = strstr(label_path, "_input");
                if (underscore) {
                    strcpy(underscore, "_label");
                    char key[128];
                    if (read_file_str(label_path, label, sizeof(label)) > 0)
                        snprintf(key, sizeof(key), "%s", label);
                    else
                        snprintf(key, sizeof(key), "sensor_%zu", i);
                    json_metric_f(j, key, temp, 1, "°C");
                    found = 1;
                }
            }
            globfree(&g);
        }
    }

    if (!found)
        json_str(j, "status", "unavailable (requires lm-sensors)");

    json_obj_close(j);
}

void collect_cpu_power(json_buf_t *j)
{
    json_obj_open(j, "cpu_power_and_voltage");

    /* RAPL power via powercap */
    int found_rapl = 0;

    for (int pkg = 0; pkg < 4; pkg++) {
        char name_path[512], energy_path[512], max_path[512];
        snprintf(name_path, sizeof(name_path),
                 "/sys/class/powercap/intel-rapl:%d/name", pkg);
        snprintf(energy_path, sizeof(energy_path),
                 "/sys/class/powercap/intel-rapl:%d/energy_uj", pkg);

        /* Also try AMD format */
        if (!file_exists(name_path)) {
            snprintf(name_path, sizeof(name_path),
                     "/sys/class/powercap/intel-rapl:0:%d/name", pkg);
            snprintf(energy_path, sizeof(energy_path),
                     "/sys/class/powercap/intel-rapl:0:%d/energy_uj", pkg);
        }

        if (!file_exists(energy_path)) continue;

        char name[64];
        read_file_str(name_path, name, sizeof(name));

        long long e1 = read_file_ll(energy_path);
        usleep(100000); /* 100ms */
        long long e2 = read_file_ll(energy_path);

        if (e1 >= 0 && e2 >= 0 && e2 > e1) {
            double watts = (double)(e2 - e1) / 100000.0; /* uJ over 100ms -> W */
            char key[128];
            snprintf(key, sizeof(key), "%s_power", name);
            json_metric_f(j, key, watts, 3, "W");
            found_rapl = 1;
        }

        snprintf(max_path, sizeof(max_path),
                 "/sys/class/powercap/intel-rapl:%d/constraint_0_max_power_uw", pkg);
        long long max_uw = read_file_ll(max_path);
        if (max_uw > 0) {
            char key[128];
            snprintf(key, sizeof(key), "%s_tdp", name);
            json_metric_f(j, key, max_uw / 1e6, 1, "W");
        }
    }

    /* zenpower/k10temp voltage readings */
    char hwmon_path[256];
    if (find_hwmon_by_name("zenpower", hwmon_path, sizeof(hwmon_path))) {
        for (int i = 1; i <= 8; i++) {
            char path[512], label_path[512], label[64], key[128];
            snprintf(path, sizeof(path), "%s/power%d_input", hwmon_path, i);
            if (!file_exists(path)) continue;
            double power = read_file_double(path) / 1e6;
            snprintf(label_path, sizeof(label_path), "%s/power%d_label", hwmon_path, i);
            if (read_file_str(label_path, label, sizeof(label)) > 0)
                snprintf(key, sizeof(key), "%s", label);
            else
                snprintf(key, sizeof(key), "power%d", i);
            json_metric_f(j, key, power, 3, "W");
        }

        for (int i = 0; i <= 3; i++) {
            char path[512], label_path[512], label[64], key[128];
            snprintf(path, sizeof(path), "%s/in%d_input", hwmon_path, i);
            if (!file_exists(path)) continue;
            double v = read_file_double(path) / 1000.0;
            snprintf(label_path, sizeof(label_path), "%s/in%d_label", hwmon_path, i);
            if (read_file_str(label_path, label, sizeof(label)) > 0)
                snprintf(key, sizeof(key), "%s", label);
            else
                snprintf(key, sizeof(key), "voltage_%d", i);
            json_metric_f(j, key, v, 3, "V");
        }

        for (int i = 1; i <= 4; i++) {
            char path[512], label_path[512], label[64], key[128];
            snprintf(path, sizeof(path), "%s/curr%d_input", hwmon_path, i);
            if (!file_exists(path)) continue;
            double amps = read_file_double(path) / 1000.0;
            snprintf(label_path, sizeof(label_path), "%s/curr%d_label", hwmon_path, i);
            if (read_file_str(label_path, label, sizeof(label)) > 0)
                snprintf(key, sizeof(key), "%s", label);
            else
                snprintf(key, sizeof(key), "current_%d", i);
            json_metric_f(j, key, amps, 3, "A");
        }
    }

    if (!found_rapl)
        json_str(j, "rapl_status", "unavailable (requires powercap/RAPL support)");

    json_obj_close(j);
}

void collect_cpu_clocks_enhanced(json_buf_t *j)
{
    json_obj_open(j, "cpu_clocks_enhanced");

    /* Try to read FCLK/UCLK from zenpower or turbostat */
    char buf[8192];
    if (run_command("turbostat --show Avg_MHz,Busy%,Bzy_MHz,TSC_MHz --num_iterations 1 --interval 0.1 2>/dev/null", buf, sizeof(buf)) >= 0 && buf[0]) {
        json_str(j, "source", "turbostat");
        /* Parse last data line */
        char *last_line = NULL;
        char *line = strtok(buf, "\n");
        while (line) {
            if (line[0] != '\0' && (line[0] == '-' || (line[0] >= '0' && line[0] <= '9')))
                last_line = line;
            line = strtok(NULL, "\n");
        }
        if (last_line) {
            json_str(j, "turbostat_summary", last_line);
        }
    }

    /* cpufreq boost/max freq */
    char path[256];
    snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq");
    long long max_freq = read_file_ll(path);
    if (max_freq > 0)
        json_metric_f(j, "max_boost_clock", max_freq / 1000.0, 1, "MHz");

    snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_min_freq");
    long long min_freq = read_file_ll(path);
    if (min_freq > 0)
        json_metric_f(j, "min_clock", min_freq / 1000.0, 1, "MHz");

    /* Check if boost is enabled */
    if (file_exists("/sys/devices/system/cpu/cpufreq/boost")) {
        long long boost = read_file_ll("/sys/devices/system/cpu/cpufreq/boost");
        json_metric_b(j, "boost_enabled", boost == 1);
    }

    /* Scaling governor */
    snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor");
    char gov[64];
    if (read_file_str(path, gov, sizeof(gov)) > 0)
        json_str(j, "scaling_governor", gov);

    /* Scaling driver */
    snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu0/cpufreq/scaling_driver");
    char driver[64];
    if (read_file_str(path, driver, sizeof(driver)) > 0)
        json_str(j, "scaling_driver", driver);

    json_obj_close(j);
}

void collect_cpu_limits(json_buf_t *j)
{
    json_obj_open(j, "cpu_limits_and_throttling");

    /* Check thermal throttling */
    int throttled = 0;

    glob_t g;
    if (glob("/sys/devices/system/cpu/cpu*/thermal_throttle/core_throttle_count", 0, NULL, &g) == 0) {
        long long total_throttle = 0;
        for (size_t i = 0; i < g.gl_pathc; i++) {
            long long count = read_file_ll(g.gl_pathv[i]);
            if (count > 0) total_throttle += count;
        }
        json_metric_i(j, "total_core_throttle_count", total_throttle, "count");
        throttled = total_throttle > 0;
        globfree(&g);
    }

    if (glob("/sys/devices/system/cpu/cpu*/thermal_throttle/package_throttle_count", 0, NULL, &g) == 0) {
        long long pkg_throttle = 0;
        for (size_t i = 0; i < g.gl_pathc; i++) {
            long long count = read_file_ll(g.gl_pathv[i]);
            if (count > 0) pkg_throttle += count;
        }
        json_metric_i(j, "package_throttle_count", pkg_throttle, "count");
        globfree(&g);
    }

    json_metric_b(j, "thermal_throttling_active", throttled);

    /* RAPL power limits */
    for (int domain = 0; domain < 2; domain++) {
        char name_path[256], limit_path[256], name[64];
        snprintf(name_path, sizeof(name_path),
                 "/sys/class/powercap/intel-rapl:%d/name", domain);
        if (!file_exists(name_path)) continue;
        read_file_str(name_path, name, sizeof(name));

        for (int c = 0; c < 3; c++) {
            snprintf(limit_path, sizeof(limit_path),
                     "/sys/class/powercap/intel-rapl:%d/constraint_%d_power_limit_uw", domain, c);
            long long limit_uw = read_file_ll(limit_path);
            if (limit_uw > 0) {
                char key[128];
                snprintf(key, sizeof(key), "%s_power_limit_%d", name, c);
                json_metric_f(j, key, limit_uw / 1e6, 1, "W");
            }
        }
    }

    json_obj_close(j);
}

void collect_cpu_bandwidth(json_buf_t *j)
{
    json_obj_open(j, "cpu_memory_bandwidth");

    /* This requires perf counters which typically need root */
    /* Try reading from turbostat or perf */
    char buf[4096];
    if (run_command("perf stat -e 'amd_df/event=0x07,umask=0x38/' -e 'amd_df/event=0x47,umask=0x38/' -a -- sleep 0.1 2>&1", buf, sizeof(buf)) >= 0 && buf[0]) {
        json_str(j, "source", "perf_amd_df");
        json_str(j, "raw_output", buf);
    } else {
        json_str(j, "status", "unavailable (requires root and perf with PMU support)");
    }

    /* Active core count */
    int ncpus = get_ncpus();
    int active = 0;
    for (int i = 0; i < ncpus; i++) {
        char path[256];
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/online", i);
        long long online = read_file_ll(path);
        if (online == 1 || !file_exists(path)) /* cpu0 often has no online file */
            active++;
    }
    json_metric_i(j, "active_core_count", active, "count");

    json_obj_close(j);
}
