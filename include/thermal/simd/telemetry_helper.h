#ifndef TSD_TELEMETRY_HELPER_H
#define TSD_TELEMETRY_HELPER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int temp_available;
    int freq_ratio_available;
    int32_t package_temp_millic;
    uint32_t freq_ratio_milli;
} tsd_telemetry_sample_t;

typedef struct {
    int cpu;
    int temp_available;
    int freq_sysfs_available;
    int msr_fd;
    int msr_available;
    uint64_t freq_max_khz;
    uint64_t last_aperf;
    uint64_t last_mperf;
    char temp_path[256];
    char freq_cur_path[256];
} tsd_telemetry_helper_t;

int tsd_telemetry_helper_init(tsd_telemetry_helper_t *helper, int cpu);
void tsd_telemetry_helper_destroy(tsd_telemetry_helper_t *helper);
int tsd_telemetry_helper_sample(tsd_telemetry_helper_t *helper, tsd_telemetry_sample_t *out);

#ifdef __cplusplus
}
#endif

#endif /* TSD_TELEMETRY_HELPER_H */
