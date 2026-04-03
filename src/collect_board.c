#include "collectors.h"
#include "sysfs_utils.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <glob.h>
#include <dirent.h>

static void collect_hwmon_sensors(json_buf_t *j, const char *hwmon_path)
{
    char path[512], label_path[512], label[64], key[128];

    /* Temperature sensors */
    for (int i = 1; i <= 32; i++) {
        snprintf(path, sizeof(path), "%s/temp%d_input", hwmon_path, i);
        if (!file_exists(path)) continue;
        double temp = read_file_double(path) / 1000.0;
        snprintf(label_path, sizeof(label_path), "%s/temp%d_label", hwmon_path, i);
        if (read_file_str(label_path, label, sizeof(label)) > 0)
            snprintf(key, sizeof(key), "%s", label);
        else
            snprintf(key, sizeof(key), "temp%d", i);
        json_metric_f(j, key, temp, 1, "°C");
    }

    /* Voltage sensors */
    for (int i = 0; i <= 32; i++) {
        snprintf(path, sizeof(path), "%s/in%d_input", hwmon_path, i);
        if (!file_exists(path)) continue;
        double v = read_file_double(path) / 1000.0;
        snprintf(label_path, sizeof(label_path), "%s/in%d_label", hwmon_path, i);
        if (read_file_str(label_path, label, sizeof(label)) > 0)
            snprintf(key, sizeof(key), "%s", label);
        else
            snprintf(key, sizeof(key), "in%d", i);
        json_metric_f(j, key, v, 3, "V");
    }

    /* Fan sensors */
    for (int i = 1; i <= 16; i++) {
        snprintf(path, sizeof(path), "%s/fan%d_input", hwmon_path, i);
        if (!file_exists(path)) continue;
        long long rpm = read_file_ll(path);
        snprintf(label_path, sizeof(label_path), "%s/fan%d_label", hwmon_path, i);
        if (read_file_str(label_path, label, sizeof(label)) > 0)
            snprintf(key, sizeof(key), "%s", label);
        else
            snprintf(key, sizeof(key), "fan%d", i);
        json_metric_i(j, key, rpm, "RPM");
    }

    /* Power sensors */
    for (int i = 1; i <= 8; i++) {
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

    /* Current sensors */
    for (int i = 1; i <= 8; i++) {
        snprintf(path, sizeof(path), "%s/curr%d_input", hwmon_path, i);
        if (!file_exists(path)) continue;
        double amps = read_file_double(path) / 1000.0;
        snprintf(label_path, sizeof(label_path), "%s/curr%d_label", hwmon_path, i);
        if (read_file_str(label_path, label, sizeof(label)) > 0)
            snprintf(key, sizeof(key), "%s", label);
        else
            snprintf(key, sizeof(key), "curr%d", i);
        json_metric_f(j, key, amps, 3, "A");
    }
}

void collect_motherboard_sensors(json_buf_t *j)
{
    json_obj_open(j, "motherboard_sensors");

    char hwmon_path[256];
    const char *board_drivers[] = {"nct6775", "nct6776", "nct6779", "nct6791",
                                    "nct6792", "nct6793", "nct6795", "nct6796",
                                    "nct6797", "nct6798",
                                    "it8728", "it8786", "it8792",
                                    "w83627", "w83795", "f71882fg",
                                    NULL};
    int found = 0;

    for (int i = 0; board_drivers[i]; i++) {
        if (find_hwmon_by_name(board_drivers[i], hwmon_path, sizeof(hwmon_path))) {
            found = 1;
            json_str(j, "driver", board_drivers[i]);
            collect_hwmon_sensors(j, hwmon_path);
            break;
        }
    }

    if (!found) {
        /* Try to find any hwmon that isn't CPU/GPU related */
        glob_t g;
        if (glob("/sys/class/hwmon/hwmon*/name", 0, NULL, &g) == 0) {
            for (size_t i = 0; i < g.gl_pathc; i++) {
                char name[64];
                if (read_file_str(g.gl_pathv[i], name, sizeof(name)) <= 0) continue;

                /* Skip known CPU/GPU drivers */
                if (strcmp(name, "k10temp") == 0 || strcmp(name, "coretemp") == 0 ||
                    strcmp(name, "zenpower") == 0 || strcmp(name, "nvidia") == 0 ||
                    strcmp(name, "nouveau") == 0 || strcmp(name, "amdgpu") == 0 ||
                    strcmp(name, "iwlwifi") == 0 || strcmp(name, "nvme") == 0 ||
                    strcmp(name, "jc42") == 0)
                    continue;

                char *last_slash = strrchr(g.gl_pathv[i], '/');
                if (last_slash) {
                    char dirpath[256];
                    size_t dirlen = last_slash - g.gl_pathv[i];
                    memcpy(dirpath, g.gl_pathv[i], dirlen);
                    dirpath[dirlen] = '\0';

                    json_obj_open(j, name);
                    collect_hwmon_sensors(j, dirpath);
                    json_obj_close(j);
                    found = 1;
                }
            }
            globfree(&g);
        }
    }

    /* DMI board info */
    char buf[1024];
    if (run_command("dmidecode -t baseboard 2>/dev/null | head -20", buf, sizeof(buf)) >= 0 && buf[0]) {
        char manufacturer[128], product[128];
        if (parse_str_field(buf, "Manufacturer", manufacturer, sizeof(manufacturer)))
            json_str(j, "board_manufacturer", manufacturer);
        if (parse_str_field(buf, "Product Name", product, sizeof(product)))
            json_str(j, "board_product", product);
    }

    if (!found)
        json_str(j, "sensor_status", "no dedicated board sensors found via hwmon");

    json_obj_close(j);
}

void collect_chipset(json_buf_t *j)
{
    json_obj_open(j, "chipset");

    /* Look for chipset temp in hwmon */
    char hwmon_path[256];
    const char *chipset_names[] = {"pch_cannonlake", "pch_cometlake", "pch_skylake",
                                     "amdgpu", NULL}; /* X570 chipset often shows up via amdgpu */
    int found = 0;

    for (int i = 0; chipset_names[i]; i++) {
        if (find_hwmon_by_name(chipset_names[i], hwmon_path, sizeof(hwmon_path))) {
            char path[512];
            snprintf(path, sizeof(path), "%s/temp1_input", hwmon_path);
            if (file_exists(path)) {
                double temp = read_file_double(path) / 1000.0;
                json_metric_f(j, "chipset_temperature", temp, 1, "°C");
                json_str(j, "driver", chipset_names[i]);
                found = 1;
                break;
            }
        }
    }

    /* Try lspci for chipset identification */
    char buf[2048];
    if (run_command("lspci 2>/dev/null | grep -i 'ISA bridge\\|SMBus\\|chipset' | head -5", buf, sizeof(buf)) >= 0 && buf[0])
        json_str(j, "chipset_id", buf);

    if (!found)
        json_str(j, "temp_status", "chipset temperature not found via hwmon");

    json_obj_close(j);
}

void collect_ec_sensors(json_buf_t *j)
{
    json_obj_open(j, "embedded_controller_sensors");

    char hwmon_path[256];
    int found = 0;

    /* ASUS EC sensors driver */
    if (find_hwmon_by_name("asus_ec_sensors", hwmon_path, sizeof(hwmon_path)) ||
        find_hwmon_by_name("asus-ec-sensors", hwmon_path, sizeof(hwmon_path))) {
        found = 1;
        json_str(j, "driver", "asus-ec-sensors");
        collect_hwmon_sensors(j, hwmon_path);
    }

    /* Also try asus_wmi_sensors */
    if (find_hwmon_by_name("asus_wmi_sensors", hwmon_path, sizeof(hwmon_path))) {
        found = 1;
        json_str(j, "driver", "asus_wmi_sensors");
        collect_hwmon_sensors(j, hwmon_path);
    }

    if (!found)
        json_str(j, "status", "unavailable (requires asus-ec-sensors or similar kernel module)");

    json_obj_close(j);
}

void collect_dimm_temps(json_buf_t *j)
{
    json_obj_open(j, "dimm_temperatures");

    int found = 0;
    glob_t g;

    if (glob("/sys/class/hwmon/hwmon*/name", 0, NULL, &g) == 0) {
        for (size_t i = 0; i < g.gl_pathc; i++) {
            char name[64];
            if (read_file_str(g.gl_pathv[i], name, sizeof(name)) <= 0) continue;
            if (strcmp(name, "jc42") != 0) continue;

            char *last_slash = strrchr(g.gl_pathv[i], '/');
            if (!last_slash) continue;

            char dirpath[256];
            size_t dirlen = last_slash - g.gl_pathv[i];
            memcpy(dirpath, g.gl_pathv[i], dirlen);
            dirpath[dirlen] = '\0';

            char path[512];
            snprintf(path, sizeof(path), "%s/temp1_input", dirpath);
            if (file_exists(path)) {
                double temp = read_file_double(path) / 1000.0;
                char key[64];
                snprintf(key, sizeof(key), "dimm_%d_temperature", found);
                json_metric_f(j, key, temp, 1, "°C");
                found++;
            }
        }
        globfree(&g);
    }

    if (!found)
        json_str(j, "status", "unavailable (requires jc42 kernel module for SPD temperature sensors)");

    json_obj_close(j);
}
