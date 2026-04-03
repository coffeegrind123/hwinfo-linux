#ifndef COLLECTORS_H
#define COLLECTORS_H

#include "json_builder.h"

void collect_system_memory(json_buf_t *j);
void collect_cpu_voltages(json_buf_t *j);
void collect_cpu_clocks_usage(json_buf_t *j);
void collect_cpu_ratios(json_buf_t *j);
void collect_cpu_cstates(json_buf_t *j);
void collect_memory_timings(json_buf_t *j);
void collect_cpu_temperatures(json_buf_t *j);
void collect_cpu_power(json_buf_t *j);
void collect_cpu_clocks_enhanced(json_buf_t *j);
void collect_cpu_limits(json_buf_t *j);
void collect_cpu_bandwidth(json_buf_t *j);
void collect_motherboard_sensors(json_buf_t *j);
void collect_chipset(json_buf_t *j);
void collect_ec_sensors(json_buf_t *j);
void collect_dimm_temps(json_buf_t *j);
void collect_smart_hdd(json_buf_t *j);
void collect_smart_nvme(json_buf_t *j);
void collect_disk_io(json_buf_t *j);
void collect_gpu_nvidia(json_buf_t *j);
void collect_network(json_buf_t *j);
void collect_hw_errors(json_buf_t *j);

#endif
