#include "collectors.h"
#include "sysfs_utils.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <glob.h>

void collect_hw_errors(json_buf_t *j)
{
    json_obj_open(j, "hardware_errors");

    int found = 0;

    /* EDAC - Error Detection and Correction */
    glob_t g;
    if (glob("/sys/devices/system/edac/mc/mc*/ce_count", 0, NULL, &g) == 0) {
        long long total_ce = 0, total_ue = 0;
        for (size_t i = 0; i < g.gl_pathc; i++) {
            total_ce += read_file_ll(g.gl_pathv[i]);
        }
        globfree(&g);

        if (glob("/sys/devices/system/edac/mc/mc*/ue_count", 0, NULL, &g) == 0) {
            for (size_t i = 0; i < g.gl_pathc; i++)
                total_ue += read_file_ll(g.gl_pathv[i]);
            globfree(&g);
        }

        json_metric_i(j, "edac_correctable_errors", total_ce, "count");
        json_metric_i(j, "edac_uncorrectable_errors", total_ue, "count");
        json_metric_i(j, "total_errors", total_ce + total_ue, "count");
        found = 1;
    }

    /* Machine check exceptions */
    if (glob("/sys/devices/system/machinecheck/machinecheck*/", 0, NULL, &g) == 0) {
        json_int(j, "machinecheck_cpus", (long long)g.gl_pathc);
        globfree(&g);
        found = 1;
    }

    /* Try rasdaemon/ras-mc-ctl */
    char buf[4096];
    if (run_command("ras-mc-ctl --summary 2>/dev/null", buf, sizeof(buf)) >= 0 && buf[0]) {
        json_str(j, "rasdaemon_summary", buf);
        found = 1;
    }

    /* Check MCE log */
    if (run_command("mcelog --client 2>/dev/null", buf, sizeof(buf)) >= 0 && buf[0]) {
        json_str(j, "mcelog_output", buf);
        found = 1;
    }

    /* dmesg hardware errors (last 50 lines of MCE/hardware errors) */
    if (run_command("dmesg 2>/dev/null | grep -i -E 'mce|hardware error|machine check|edac' | tail -10", buf, sizeof(buf)) >= 0 && buf[0]) {
        json_str(j, "dmesg_hw_errors", buf);
        found = 1;
    }

    if (!found)
        json_str(j, "status", "no hardware error sources available (requires EDAC, rasdaemon, or mcelog)");

    json_obj_close(j);
}
