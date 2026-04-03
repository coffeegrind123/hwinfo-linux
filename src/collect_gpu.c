#include "collectors.h"
#include "sysfs_utils.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static void query_nvidia_field(json_buf_t *j, const char *field, const char *json_key,
                                const char *unit, int is_float, int precision)
{
    char cmd[512], buf[256];
    snprintf(cmd, sizeof(cmd), "nvidia-smi --query-gpu=%s --format=csv,noheader,nounits 2>/dev/null", field);
    if (run_command(cmd, buf, sizeof(buf)) >= 0 && buf[0] && strcmp(buf, "[N/A]") != 0 &&
        strcmp(buf, "N/A") != 0 && strcmp(buf, "[Not Supported]") != 0) {
        if (is_float)
            json_metric_f(j, json_key, atof(buf), precision, unit);
        else
            json_metric_i(j, json_key, atoll(buf), unit);
    }
}

void collect_gpu_nvidia(json_buf_t *j)
{
    json_obj_open(j, "gpu_nvidia");

    /* Check if nvidia-smi exists */
    char test[128];
    if (run_command("nvidia-smi --query-gpu=name --format=csv,noheader 2>/dev/null", test, sizeof(test)) < 0 || !test[0]) {
        /* Try AMD GPU via amdgpu hwmon */
        char hwmon_path[256];
        if (find_hwmon_by_name("amdgpu", hwmon_path, sizeof(hwmon_path))) {
            json_str(j, "vendor", "AMD");
            json_str(j, "driver", "amdgpu");

            char path[512];
            snprintf(path, sizeof(path), "%s/temp1_input", hwmon_path);
            if (file_exists(path))
                json_metric_f(j, "gpu_temperature", read_file_double(path) / 1000.0, 1, "°C");

            snprintf(path, sizeof(path), "%s/power1_average", hwmon_path);
            if (file_exists(path))
                json_metric_f(j, "gpu_power", read_file_double(path) / 1e6, 3, "W");

            snprintf(path, sizeof(path), "%s/fan1_input", hwmon_path);
            if (file_exists(path))
                json_metric_i(j, "fan_speed", read_file_ll(path), "RPM");
        } else {
            json_str(j, "status", "no NVIDIA or AMD GPU detected (nvidia-smi not found, no amdgpu hwmon)");
        }
        json_obj_close(j);
        return;
    }

    json_str(j, "vendor", "NVIDIA");
    json_str(j, "name", test);

    /* Temperatures */
    json_obj_open(j, "temperatures");
    query_nvidia_field(j, "temperature.gpu", "gpu_temperature", "°C", 1, 1);
    query_nvidia_field(j, "temperature.memory", "gpu_memory_junction_temperature", "°C", 1, 1);

    char buf[256];
    if (run_command("nvidia-smi --query-gpu=temperature.gpu.tlimit --format=csv,noheader,nounits 2>/dev/null", buf, sizeof(buf)) >= 0 && buf[0] && strcmp(buf, "[N/A]") != 0)
        json_metric_f(j, "gpu_thermal_limit", atof(buf), 1, "°C");

    /* Hotspot via dmon */
    char dmon_buf[4096];
    if (run_command("nvidia-smi dmon -c 1 -s t 2>/dev/null | tail -1", dmon_buf, sizeof(dmon_buf)) >= 0 && dmon_buf[0]) {
        /* dmon output: gpu gtemp mtemp */
        int gpu_id;
        float gtemp, mtemp;
        if (sscanf(dmon_buf, "%d %f %f", &gpu_id, &gtemp, &mtemp) >= 2) {
            /* gtemp is the core temp, we already have it */
        }
    }
    json_obj_close(j);

    /* Voltage */
    json_obj_open(j, "voltage");
    if (run_command("nvidia-smi -q -d VOLTAGE 2>/dev/null", buf, sizeof(buf)) >= 0 && buf[0]) {
        double gv = parse_double_field(buf, "Graphics");
        if (gv > 0)
            json_metric_f(j, "gpu_core_voltage", gv / 1000.0, 3, "V");
    }
    json_obj_close(j);

    /* Fans */
    json_obj_open(j, "fans");
    query_nvidia_field(j, "fan.speed", "fan_duty", "%", 1, 0);

    if (run_command("nvidia-smi -q -d FAN 2>/dev/null", buf, sizeof(buf)) >= 0 && buf[0]) {
        /* Try to parse individual fan speeds */
        char *p = buf;
        int fan_idx = 0;
        while ((p = strstr(p, "Fan Speed")) != NULL) {
            char *colon = strchr(p, ':');
            if (colon) {
                int speed = atoi(colon + 1);
                char key[32];
                snprintf(key, sizeof(key), "fan%d_duty", fan_idx);
                json_metric_i(j, key, speed, "%");
                fan_idx++;
            }
            p++;
        }
    }
    json_obj_close(j);

    /* Power */
    json_obj_open(j, "power");
    query_nvidia_field(j, "power.draw", "gpu_power", "W", 1, 3);
    query_nvidia_field(j, "power.limit", "gpu_power_limit", "W", 1, 1);
    query_nvidia_field(j, "power.default_limit", "gpu_default_power_limit", "W", 1, 1);
    query_nvidia_field(j, "power.max_limit", "gpu_max_power_limit", "W", 1, 1);
    query_nvidia_field(j, "power.min_limit", "gpu_min_power_limit", "W", 1, 1);

    /* TDP percentage */
    char draw_buf[64], limit_buf[64];
    if (run_command("nvidia-smi --query-gpu=power.draw --format=csv,noheader,nounits 2>/dev/null", draw_buf, sizeof(draw_buf)) >= 0 &&
        run_command("nvidia-smi --query-gpu=power.limit --format=csv,noheader,nounits 2>/dev/null", limit_buf, sizeof(limit_buf)) >= 0) {
        double draw = atof(draw_buf);
        double limit = atof(limit_buf);
        if (limit > 0)
            json_metric_f(j, "gpu_power_pct_of_tdp", draw / limit * 100.0, 1, "%");
    }
    json_obj_close(j);

    /* Clocks */
    json_obj_open(j, "clocks");
    query_nvidia_field(j, "clocks.current.graphics", "gpu_clock", "MHz", 1, 1);
    query_nvidia_field(j, "clocks.current.memory", "gpu_memory_clock", "MHz", 1, 1);
    query_nvidia_field(j, "clocks.current.video", "gpu_video_clock", "MHz", 1, 1);
    query_nvidia_field(j, "clocks.current.sm", "gpu_sm_clock", "MHz", 1, 1);
    query_nvidia_field(j, "clocks.max.graphics", "gpu_max_graphics_clock", "MHz", 1, 1);
    query_nvidia_field(j, "clocks.max.memory", "gpu_max_memory_clock", "MHz", 1, 1);

    /* Effective clock from dmon */
    if (run_command("nvidia-smi dmon -c 1 -s c 2>/dev/null | tail -1", dmon_buf, sizeof(dmon_buf)) >= 0 && dmon_buf[0]) {
        int gpu_id;
        float sm_clk, mem_clk;
        if (sscanf(dmon_buf, "%d %f %f", &gpu_id, &sm_clk, &mem_clk) >= 3) {
            json_metric_f(j, "effective_sm_clock", sm_clk, 1, "MHz");
            json_metric_f(j, "effective_mem_clock", mem_clk, 1, "MHz");
        }
    }
    json_obj_close(j);

    /* Utilization */
    json_obj_open(j, "utilization");
    query_nvidia_field(j, "utilization.gpu", "gpu_core_load", "%", 1, 0);
    query_nvidia_field(j, "utilization.memory", "gpu_memory_controller_load", "%", 1, 0);
    query_nvidia_field(j, "utilization.encoder", "gpu_encoder_load", "%", 1, 0);
    query_nvidia_field(j, "utilization.decoder", "gpu_decoder_load", "%", 1, 0);
    json_obj_close(j);

    /* Memory */
    json_obj_open(j, "memory");
    query_nvidia_field(j, "memory.total", "gpu_memory_total", "MB", 0, 0);
    query_nvidia_field(j, "memory.used", "gpu_memory_used", "MB", 0, 0);
    query_nvidia_field(j, "memory.free", "gpu_memory_available", "MB", 0, 0);

    /* Memory usage percentage */
    char used_buf[64], total_buf[64];
    if (run_command("nvidia-smi --query-gpu=memory.used --format=csv,noheader,nounits 2>/dev/null", used_buf, sizeof(used_buf)) >= 0 &&
        run_command("nvidia-smi --query-gpu=memory.total --format=csv,noheader,nounits 2>/dev/null", total_buf, sizeof(total_buf)) >= 0) {
        double used = atof(used_buf);
        double total = atof(total_buf);
        if (total > 0)
            json_metric_f(j, "gpu_memory_usage", used / total * 100.0, 1, "%");
    }
    json_obj_close(j);

    /* PCIe */
    json_obj_open(j, "pcie");
    query_nvidia_field(j, "pcie.link.gen.current", "pcie_gen_current", "", 0, 0);
    query_nvidia_field(j, "pcie.link.gen.max", "pcie_gen_max", "", 0, 0);
    query_nvidia_field(j, "pcie.link.width.current", "pcie_width_current", "x", 0, 0);
    query_nvidia_field(j, "pcie.link.width.max", "pcie_width_max", "x", 0, 0);

    char speed_buf[64];
    if (run_command("nvidia-smi --query-gpu=pcie.link.gen.current --format=csv,noheader,nounits 2>/dev/null", speed_buf, sizeof(speed_buf)) >= 0) {
        int gen = atoi(speed_buf);
        double gt_s = 0;
        switch (gen) {
        case 1: gt_s = 2.5; break;
        case 2: gt_s = 5.0; break;
        case 3: gt_s = 8.0; break;
        case 4: gt_s = 16.0; break;
        case 5: gt_s = 32.0; break;
        }
        if (gt_s > 0)
            json_metric_f(j, "pcie_link_speed", gt_s, 1, "GT/s");
    }
    json_obj_close(j);

    /* Performance state & limiters */
    json_obj_open(j, "other");
    query_nvidia_field(j, "pstate", "performance_state", "", 0, 0);

    char perf_buf[1024];
    if (run_command("nvidia-smi -q -d PERFORMANCE 2>/dev/null", perf_buf, sizeof(perf_buf)) >= 0 && perf_buf[0]) {
        int has_limiter = (strstr(perf_buf, "Active") != NULL);
        json_metric_b(j, "performance_limiters_active", has_limiter);
    }

    /* Driver version */
    query_nvidia_field(j, "driver_version", "driver_version", "", 0, 0);
    json_obj_close(j);

    json_obj_close(j);
}
