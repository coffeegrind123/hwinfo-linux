# hwinfo-linux

Linux hardware monitoring tool that replicates HWiNFO64 sensor coverage. Outputs structured JSON for LLM consumption, scripting, and automation.

Two binaries:

- **hwinfo-linux** — Native Linux collector. Reads directly from `/proc`, `/sys`, hwmon, nvidia-smi, smartctl, turbostat, dmidecode.
- **hwinfo-bridge** — Fetches sensor data from [RemoteHWInfo](https://github.com/Demion/remotehwinfo) running on a Windows host. Use this when running in WSL2 or Docker on Windows.

Both produce the same JSON schema with `current/min/max/avg/unit` per metric.

## Coverage

All 23 HWiNFO64 metric categories:

| # | Category | Source (native) | Source (bridge) |
|---|---|---|---|
| 1 | System Memory | /proc/meminfo | HWiNFO shared memory |
| 2 | CPU Core Voltages | hwmon (zenpower/k10temp) | HWiNFO SVI2 TFN |
| 3 | CPU Clocks & Usage | /proc/stat, cpufreq | HWiNFO per-core clocks |
| 4 | CPU Core Ratios | cpufreq / bus clock | HWiNFO ratios |
| 5 | CPU C-State Residency | cpuidle, turbostat | HWiNFO C0/C1/C6 |
| 6 | Memory Timings | dmidecode, decode-dimms | HWiNFO tCAS/tRCD/tRP/tRAS |
| 7 | CPU Temperatures | hwmon (k10temp/coretemp) | HWiNFO per-core/CCD/IOD |
| 8 | CPU Power & Voltage | RAPL powercap, zenpower | HWiNFO SVI2 TFN power |
| 9 | CPU Clocks (Enhanced) | turbostat, cpufreq | HWiNFO FCLK/UCLK/L3 |
| 10 | CPU Limits & Throttling | thermal_throttle, RAPL | HWiNFO PPT/TDC/EDC |
| 11 | CPU Memory Bandwidth | perf amd_df events | HWiNFO DRAM R/W bandwidth |
| 12 | Motherboard Sensors | hwmon (nct6775 etc.) | HWiNFO Super I/O |
| 13 | Chipset | hwmon, lspci | HWiNFO chipset temp |
| 14 | Embedded Controller | hwmon (asus-ec-sensors) | HWiNFO EC sensors |
| 15 | DIMM Temperatures | hwmon (jc42) | HWiNFO SPD temp |
| 16 | S.M.A.R.T. HDD | smartctl | HWiNFO SMART |
| 17 | S.M.A.R.T. NVMe | nvme smart-log, smartctl | HWiNFO SMART |
| 18-19 | Disk I/O | /proc/diskstats | HWiNFO disk activity |
| 20 | GPU (NVIDIA/AMD) | nvidia-smi, amdgpu hwmon | HWiNFO GPU sensors |
| 21-22 | Network | /proc/net/dev | HWiNFO network stats |
| 23 | Hardware Errors | EDAC, mcelog, rasdaemon | HWiNFO WHEA |

## Building

### Requirements

- GCC or Clang with C11 support
- GNU Make
- Linux (any distro, kernel 4.x+)

### Build

```bash
git clone --recurse-submodules https://github.com/coffeegrind123/hwinfo-linux.git
cd hwinfo-linux
make
```

This produces two binaries in the project root: `hwinfo-linux` and `hwinfo-bridge`.

### Install (optional)

```bash
sudo make install          # installs to /usr/local/bin/
sudo make install PREFIX=/usr  # or custom prefix
```

### Uninstall

```bash
sudo rm /usr/local/bin/hwinfo-linux /usr/local/bin/hwinfo-bridge
```

## Usage

### hwinfo-linux (native)

```bash
# All metrics, compact JSON
./hwinfo-linux

# All metrics, pretty-printed
./hwinfo-linux -p

# Specific categories (comma-separated)
./hwinfo-linux -c memory,cpu_temps,gpu -p

# Continuous monitoring every 5 seconds
./hwinfo-linux -r 0 -i 5

# List available categories
./hwinfo-linux -l
```

Available categories: `memory`, `cpu_voltages`, `cpu_clocks`, `cpu_ratios`, `cpu_cstates`, `mem_timings`, `cpu_temps`, `cpu_power`, `cpu_clocks_enh`, `cpu_limits`, `cpu_bandwidth`, `motherboard`, `chipset`, `ec_sensors`, `dimm_temps`, `smart_hdd`, `smart_nvme`, `disk_io`, `gpu`, `network`, `hw_errors`

Some categories require root for full data (smartctl, dmidecode, turbostat, RAPL). Run with `sudo` for complete output.

### hwinfo-bridge (Windows via RemoteHWInfo)

Use this when running inside WSL2 or Docker on a Windows host.

**Windows side setup:**

1. Run [HWiNFO64](https://www.hwinfo.com/) in **Sensors Only** mode
2. Download [RemoteHWInfo](https://github.com/Demion/remotehwinfo/releases) and run it — serves JSON on port 60000

**Linux/WSL2/Docker side:**

```bash
# From WSL2 (auto-detect Windows host IP)
./hwinfo-bridge -H $(grep nameserver /etc/resolv.conf | awk '{print $2}') -p

# From Docker container
./hwinfo-bridge -H host.docker.internal -p

# Group output by sensor instead of by metric type
./hwinfo-bridge -H host.docker.internal -s -p

# Continuous monitoring
./hwinfo-bridge -H host.docker.internal -r 0 -i 5
```

### Output format

Both tools output JSON with this structure per metric:

```json
{
  "cpu_temperatures": {
    "CPU (Tctl/Tdie)": {
      "current": 73.5,
      "min": 66.3,
      "max": 78.5,
      "avg": 71.5,
      "unit": "°C"
    }
  }
}
```

The bridge adds `sensor` and `label` fields from HWiNFO's naming.

### Piping to other tools

```bash
# Extract GPU temperature with jq
./hwinfo-linux -c gpu | jq '.gpu_nvidia.temperatures.gpu_temperature.current'

# Feed to an LLM for analysis
./hwinfo-bridge -H host.docker.internal | llm "analyze this system health data"

# Log to file with timestamps
./hwinfo-linux -r 0 -i 60 >> /var/log/hwinfo.jsonl
```

## Project structure

```
hwinfo-linux/
├── Makefile
├── src/
│   ├── main.c              # Native collector entry point + CLI
│   ├── bridge.c            # RemoteHWInfo bridge entry point + JSON parser
│   ├── json_builder.c/h    # Lightweight JSON serializer (no dependencies)
│   ├── sysfs_utils.c/h     # File/sysfs/command reading utilities
│   ├── collectors.h        # Collector function declarations
│   ├── collect_cpu.c       # CPU voltages, clocks, ratios, C-states, temps, power, limits, bandwidth
│   ├── collect_memory.c    # System memory + memory timings
│   ├── collect_board.c     # Motherboard, chipset, EC, DIMM temps
│   ├── collect_storage.c   # SMART (HDD + NVMe) + disk I/O
│   ├── collect_gpu.c       # NVIDIA (nvidia-smi) + AMD (amdgpu hwmon)
│   ├── collect_network.c   # All network interfaces
│   └── collect_errors.c    # EDAC, MCE, hardware errors
└── vendor/
    └── remotehwinfo/       # Git submodule: Windows HTTP JSON server
```

## Dependencies

**Build time:** GCC/Clang, Make — no external libraries. Everything is self-contained C11 with POSIX sockets.

**Runtime (native, optional for full data):**

| Tool | Package | What it provides |
|---|---|---|
| lm-sensors | `lm-sensors` | CPU/board temps, voltages, fans |
| nvidia-smi | `nvidia-driver` | GPU metrics |
| smartctl | `smartmontools` | Drive health (SMART) |
| nvme | `nvme-cli` | NVMe health data |
| dmidecode | `dmidecode` | Memory timings, board info |
| turbostat | `linux-tools` | Per-core clocks, C-states, power |

Install all on Debian/Ubuntu:

```bash
sudo apt install lm-sensors smartmontools nvme-cli dmidecode linux-tools-common
```

On Fedora/RHEL:

```bash
sudo dnf install lm_sensors smartmontools nvme-cli dmidecode kernel-tools
```

On Arch:

```bash
sudo pacman -S lm_sensors smartmontools nvme-cli dmidecode linux-tools
```

**Runtime (bridge):** Only needs network access to the RemoteHWInfo server. No additional packages required.

## License

MIT
