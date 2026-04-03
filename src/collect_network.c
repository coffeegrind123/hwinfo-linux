#include "collectors.h"
#include "sysfs_utils.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <dirent.h>

void collect_network(json_buf_t *j)
{
    json_obj_open(j, "network");

    /* Read /proc/net/dev twice with 100ms gap for rate calculation */
    char buf1[8192], buf2[8192];
    if (read_file_str("/proc/net/dev", buf1, sizeof(buf1)) <= 0) {
        json_str(j, "status", "cannot read /proc/net/dev");
        json_obj_close(j);
        return;
    }
    usleep(100000);
    read_file_str("/proc/net/dev", buf2, sizeof(buf2));

    /* Parse each interface */
    DIR *dir = opendir("/sys/class/net");
    if (!dir) {
        json_str(j, "status", "cannot enumerate network interfaces");
        json_obj_close(j);
        return;
    }

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        const char *ifname = ent->d_name;

        /* Find interface in proc/net/dev */
        char search[300];
        snprintf(search, sizeof(search), "%s:", ifname);

        char *line1 = strstr(buf1, search);
        char *line2 = strstr(buf2, search);
        if (!line1 || !line2) continue;

        /* Skip to after the colon */
        line1 = strchr(line1, ':') + 1;
        line2 = strchr(line2, ':') + 1;

        /* /proc/net/dev format after iface:
           rx_bytes rx_packets rx_errs rx_drop rx_fifo rx_frame rx_compressed rx_multicast
           tx_bytes tx_packets tx_errs tx_drop tx_fifo tx_colls tx_carrier tx_compressed */
        unsigned long long rx1, rx_p1, rx_e1, rx_d1, rx_fifo1, rx_frame1, rx_comp1, rx_multi1;
        unsigned long long tx1, tx_p1, tx_e1, tx_d1, tx_fifo1, tx_colls1, tx_carrier1, tx_comp1;
        unsigned long long rx2, tx2;

        if (sscanf(line1, " %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
                   &rx1, &rx_p1, &rx_e1, &rx_d1, &rx_fifo1, &rx_frame1, &rx_comp1, &rx_multi1,
                   &tx1, &tx_p1, &tx_e1, &tx_d1, &tx_fifo1, &tx_colls1, &tx_carrier1, &tx_comp1) < 16)
            continue;

        unsigned long long dummy;
        if (sscanf(line2, " %llu %llu %llu %llu %llu %llu %llu %llu %llu",
                   &rx2, &dummy, &dummy, &dummy, &dummy, &dummy, &dummy, &dummy, &tx2) < 9)
            continue;

        double dt = 0.1; /* 100ms */
        double dl_rate = (double)(rx2 - rx1) / dt / 1024.0; /* KB/s */
        double ul_rate = (double)(tx2 - tx1) / dt / 1024.0;

        json_obj_open(j, ifname);

        /* Get interface type */
        char type_path[512], operstate_path[512], speed_path[512];
        snprintf(type_path, sizeof(type_path), "/sys/class/net/%s/type", ifname);
        snprintf(operstate_path, sizeof(operstate_path), "/sys/class/net/%s/operstate", ifname);
        snprintf(speed_path, sizeof(speed_path), "/sys/class/net/%s/speed", ifname);

        char operstate[32];
        if (read_file_str(operstate_path, operstate, sizeof(operstate)) > 0)
            json_str(j, "operstate", operstate);

        long long speed = read_file_ll(speed_path);
        if (speed > 0)
            json_metric_i(j, "link_speed", speed, "Mbps");

        /* Determine interface type */
        char wireless_path[512];
        snprintf(wireless_path, sizeof(wireless_path), "/sys/class/net/%s/wireless", ifname);
        if (dir_exists(wireless_path) || strncmp(ifname, "wl", 2) == 0)
            json_str(j, "type", "wireless");
        else if (strcmp(ifname, "lo") == 0)
            json_str(j, "type", "loopback");
        else if (strncmp(ifname, "eth", 3) == 0 || strncmp(ifname, "en", 2) == 0)
            json_str(j, "type", "ethernet");
        else if (strncmp(ifname, "docker", 6) == 0 || strncmp(ifname, "br-", 3) == 0)
            json_str(j, "type", "bridge");
        else if (strncmp(ifname, "veth", 4) == 0)
            json_str(j, "type", "virtual");
        else
            json_str(j, "type", "other");

        /* MAC address */
        char mac_path[512], mac[32];
        snprintf(mac_path, sizeof(mac_path), "/sys/class/net/%s/address", ifname);
        if (read_file_str(mac_path, mac, sizeof(mac)) > 0)
            json_str(j, "mac_address", mac);

        json_metric_i(j, "total_download", (long long)(rx1 / (1024*1024)), "MB");
        json_metric_i(j, "total_upload", (long long)(tx1 / (1024*1024)), "MB");
        json_metric_f(j, "current_download_rate", dl_rate, 3, "KB/s");
        json_metric_f(j, "current_upload_rate", ul_rate, 3, "KB/s");

        json_metric_i(j, "rx_packets", (long long)rx_p1, "count");
        json_metric_i(j, "tx_packets", (long long)tx_p1, "count");
        json_metric_i(j, "rx_errors", (long long)rx_e1, "count");
        json_metric_i(j, "tx_errors", (long long)tx_e1, "count");
        json_metric_i(j, "rx_dropped", (long long)rx_d1, "count");
        json_metric_i(j, "tx_dropped", (long long)tx_d1, "count");

        json_obj_close(j);
    }
    closedir(dir);

    json_obj_close(j);
}
