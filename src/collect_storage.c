#include "collectors.h"
#include "sysfs_utils.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <glob.h>
#include <dirent.h>
#include <unistd.h>

static void parse_smartctl_common(json_buf_t *j, const char *output)
{
    char model[128], serial[128], fw[128];
    if (parse_str_field(output, "Device Model", model, sizeof(model)) ||
        parse_str_field(output, "Model Number", model, sizeof(model)))
        json_str(j, "model", model);
    if (parse_str_field(output, "Serial Number", serial, sizeof(serial)))
        json_str(j, "serial", serial);
    if (parse_str_field(output, "Firmware Version", fw, sizeof(fw)))
        json_str(j, "firmware", fw);

    if (strstr(output, "PASSED") || strstr(output, "SMART overall-health self-assessment test result: PASSED"))
        json_metric_b(j, "drive_failure", 0);
    else if (strstr(output, "FAILED"))
        json_metric_b(j, "drive_failure", 1);

    int warning = (strstr(output, "Pre-fail") != NULL && strstr(output, "FAILING_NOW") != NULL);
    json_metric_b(j, "drive_warning", warning);
}

void collect_smart_hdd(json_buf_t *j)
{
    json_obj_open(j, "smart_hdd");

    glob_t g;
    int found = 0;

    /* Find all sd* block devices */
    if (glob("/sys/block/sd*", 0, NULL, &g) == 0) {
        for (size_t i = 0; i < g.gl_pathc; i++) {
            char *devname = strrchr(g.gl_pathv[i], '/');
            if (!devname) continue;
            devname++;

            /* Check if rotational (HDD) */
            char rot_path[256];
            snprintf(rot_path, sizeof(rot_path), "/sys/block/%s/queue/rotational", devname);
            long long rotational = read_file_ll(rot_path);
            if (rotational != 1) continue; /* skip SSDs */

            char cmd[256], buf[16384];
            snprintf(cmd, sizeof(cmd), "smartctl -a /dev/%s 2>/dev/null", devname);

            json_obj_open(j, devname);
            json_str(j, "device", devname);
            json_str(j, "type", "HDD");

            if (run_command(cmd, buf, sizeof(buf)) >= 0 && buf[0]) {
                parse_smartctl_common(j, buf);

                /* Temperature */
                double temp = parse_double_field(buf, "Temperature_Celsius");
                if (temp < 0) temp = parse_double_field(buf, "Current Temperature");
                if (temp >= 0)
                    json_metric_f(j, "drive_temperature", temp, 0, "°C");

                /* Host writes/reads (in sectors, convert to GB) */
                long long writes = parse_ll_field(buf, "Total_LBAs_Written");
                long long reads = parse_ll_field(buf, "Total_LBAs_Read");
                if (writes > 0)
                    json_metric_i(j, "total_host_writes", writes * 512 / (1024*1024*1024LL), "GB");
                if (reads > 0)
                    json_metric_i(j, "total_host_reads", reads * 512 / (1024*1024*1024LL), "GB");

                /* Power on hours */
                long long poh = parse_ll_field(buf, "Power_On_Hours");
                if (poh > 0)
                    json_metric_i(j, "power_on_hours", poh, "hours");

                /* Reallocated sectors */
                long long realloc = parse_ll_field(buf, "Reallocated_Sector_Ct");
                if (realloc >= 0)
                    json_metric_i(j, "reallocated_sectors", realloc, "count");
            } else {
                json_str(j, "status", "smartctl failed (requires root)");
            }

            json_obj_close(j);
            found++;
        }
        globfree(&g);
    }

    if (!found)
        json_str(j, "status", "no HDDs detected");

    json_obj_close(j);
}

void collect_smart_nvme(json_buf_t *j)
{
    json_obj_open(j, "smart_nvme");

    glob_t g;
    int found = 0;

    if (glob("/sys/block/nvme*", 0, NULL, &g) == 0) {
        for (size_t i = 0; i < g.gl_pathc; i++) {
            char *devname = strrchr(g.gl_pathv[i], '/');
            if (!devname) continue;
            devname++;

            /* Only look at nvmeXnY, not nvmeXnYpZ partitions */
            if (strchr(devname + 4, 'p')) continue;

            char cmd[256], buf[16384];

            json_obj_open(j, devname);
            json_str(j, "device", devname);
            json_str(j, "type", "NVMe");

            /* Try nvme smart-log first */
            /* Extract nvmeX from nvmeXnY */
            char ctrl_name[32];
            strncpy(ctrl_name, devname, sizeof(ctrl_name) - 1);
            ctrl_name[sizeof(ctrl_name) - 1] = '\0';
            char *n_pos = strchr(ctrl_name + 4, 'n');
            if (n_pos) *n_pos = '\0';

            snprintf(cmd, sizeof(cmd), "nvme smart-log /dev/%s 2>/dev/null", devname);
            if (run_command(cmd, buf, sizeof(buf)) >= 0 && buf[0]) {
                double temp = parse_double_field(buf, "temperature");
                if (temp >= 0)
                    json_metric_f(j, "drive_temperature", temp, 0, "°C");

                double avail_spare = parse_double_field(buf, "available_spare");
                if (avail_spare >= 0)
                    json_metric_f(j, "available_spare", avail_spare, 0, "%");

                double pct_used = parse_double_field(buf, "percentage_used");
                if (pct_used >= 0)
                    json_metric_f(j, "drive_remaining_life", 100.0 - pct_used, 1, "%");

                long long data_written = parse_ll_field(buf, "data_units_written");
                if (data_written > 0)
                    json_metric_i(j, "total_host_writes", data_written * 512 / 1024 / 1024, "GB");

                long long data_read = parse_ll_field(buf, "data_units_read");
                if (data_read > 0)
                    json_metric_i(j, "total_host_reads", data_read * 512 / 1024 / 1024, "GB");

                long long power_hours = parse_ll_field(buf, "power_on_hours");
                if (power_hours > 0)
                    json_metric_i(j, "power_on_hours", power_hours, "hours");

                int crit_warn = (int)parse_ll_field(buf, "critical_warning");
                json_metric_b(j, "drive_warning", crit_warn != 0);
                json_metric_b(j, "drive_failure", 0);
            } else {
                /* Fallback to smartctl */
                snprintf(cmd, sizeof(cmd), "smartctl -a /dev/%s 2>/dev/null", devname);
                if (run_command(cmd, buf, sizeof(buf)) >= 0 && buf[0]) {
                    parse_smartctl_common(j, buf);
                    double temp = parse_double_field(buf, "Temperature:");
                    if (temp >= 0)
                        json_metric_f(j, "drive_temperature", temp, 0, "°C");
                } else {
                    json_str(j, "status", "requires root for nvme/smartctl access");
                }
            }

            /* Read model from sysfs */
            char model_path[256], model[128];
            snprintf(model_path, sizeof(model_path), "/sys/block/%s/device/model", devname);
            if (read_file_str(model_path, model, sizeof(model)) > 0)
                json_str(j, "model", model);

            json_obj_close(j);
            found++;
        }
        globfree(&g);
    }

    if (!found)
        json_str(j, "status", "no NVMe devices detected");

    json_obj_close(j);
}

void collect_disk_io(json_buf_t *j)
{
    json_obj_open(j, "disk_io");

    char buf1[4096], buf2[4096];
    if (read_file_str("/proc/diskstats", buf1, sizeof(buf1)) <= 0) {
        json_str(j, "status", "cannot read /proc/diskstats");
        json_obj_close(j);
        return;
    }

    usleep(100000); /* 100ms sample */
    read_file_str("/proc/diskstats", buf2, sizeof(buf2));

    /* Parse each block device */
    glob_t g;
    if (glob("/sys/block/*", 0, NULL, &g) == 0) {
        for (size_t i = 0; i < g.gl_pathc; i++) {
            char *devname = strrchr(g.gl_pathv[i], '/');
            if (!devname) continue;
            devname++;

            /* Skip loop, ram, dm devices */
            if (strncmp(devname, "loop", 4) == 0 || strncmp(devname, "ram", 3) == 0)
                continue;

            /* Find device in diskstats */
            char search[64];
            snprintf(search, sizeof(search), " %s ", devname);
            char *line1 = strstr(buf1, search);
            char *line2 = strstr(buf2, search);
            if (!line1 || !line2) continue;

            /* diskstats format: major minor name rd_ios rd_merge rd_sect rd_ticks
               wr_ios wr_merge wr_sect wr_ticks in_flight io_ticks time_in_queue */
            unsigned long rd_ios1, rd_sect1, rd_ticks1, wr_ios1, wr_sect1, wr_ticks1, io_ticks1;
            unsigned long rd_ios2, rd_sect2, rd_ticks2, wr_ios2, wr_sect2, wr_ticks2, io_ticks2;
            unsigned long dummy;
            char dname[64];
            unsigned int major, minor;

            /* Rewind to beginning of line */
            while (line1 > buf1 && *(line1-1) != '\n') line1--;
            while (line2 > buf2 && *(line2-1) != '\n') line2--;

            if (sscanf(line1, "%u %u %s %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu",
                       &major, &minor, dname,
                       &rd_ios1, &dummy, &rd_sect1, &rd_ticks1,
                       &wr_ios1, &dummy, &wr_sect1, &wr_ticks1,
                       &dummy, &io_ticks1) < 13) continue;

            if (sscanf(line2, "%u %u %s %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu",
                       &major, &minor, dname,
                       &rd_ios2, &dummy, &rd_sect2, &rd_ticks2,
                       &wr_ios2, &dummy, &wr_sect2, &wr_ticks2,
                       &dummy, &io_ticks2) < 13) continue;

            double dt = 0.1; /* 100ms */
            double rd_rate = (double)(rd_sect2 - rd_sect1) * 512.0 / dt / (1024*1024);
            double wr_rate = (double)(wr_sect2 - wr_sect1) * 512.0 / dt / (1024*1024);
            double rd_activity = (double)(rd_ticks2 - rd_ticks1) / (dt * 1000) * 100.0;
            double wr_activity = (double)(wr_ticks2 - wr_ticks1) / (dt * 1000) * 100.0;
            double total_activity = (double)(io_ticks2 - io_ticks1) / (dt * 1000) * 100.0;
            if (rd_activity > 100.0) rd_activity = 100.0;
            if (wr_activity > 100.0) wr_activity = 100.0;
            if (total_activity > 100.0) total_activity = 100.0;

            /* Total bytes from sysfs stat */
            char stat_path[256];
            snprintf(stat_path, sizeof(stat_path), "/sys/block/%s/stat", devname);
            char stat_buf[512];
            unsigned long total_rd_sect = 0, total_wr_sect = 0;
            if (read_file_str(stat_path, stat_buf, sizeof(stat_buf)) > 0) {
                unsigned long vals[15];
                int nvals = sscanf(stat_buf, "%lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu",
                                   &vals[0], &vals[1], &vals[2], &vals[3],
                                   &vals[4], &vals[5], &vals[6], &vals[7],
                                   &vals[8], &vals[9], &vals[10]);
                if (nvals >= 7) {
                    total_rd_sect = vals[2];
                    total_wr_sect = vals[6];
                }
            }

            /* Determine drive type */
            char rot_path[256];
            snprintf(rot_path, sizeof(rot_path), "/sys/block/%s/queue/rotational", devname);
            long long rotational = read_file_ll(rot_path);
            const char *dtype = (rotational == 1) ? "HDD" :
                                (strncmp(devname, "nvme", 4) == 0) ? "NVMe" : "SSD";

            json_obj_open(j, devname);
            json_str(j, "type", dtype);
            json_metric_f(j, "read_activity", rd_activity, 1, "%");
            json_metric_f(j, "write_activity", wr_activity, 1, "%");
            json_metric_f(j, "total_activity", total_activity, 1, "%");
            json_metric_f(j, "read_rate", rd_rate, 3, "MB/s");
            json_metric_f(j, "write_rate", wr_rate, 3, "MB/s");
            json_metric_i(j, "read_total", (long long)(total_rd_sect * 512 / (1024*1024)), "MB");
            json_metric_i(j, "write_total", (long long)(total_wr_sect * 512 / (1024*1024)), "MB");
            json_obj_close(j);
        }
        globfree(&g);
    }

    json_obj_close(j);
}
