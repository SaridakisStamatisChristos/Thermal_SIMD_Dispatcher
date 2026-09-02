#ifndef TSD_TELEMETRY_HELPER_H
#define TSD_TELEMETRY_HELPER_H

#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Raw fields are the authoritative safety channel. They must not be delayed
 * by EWMA/filtering before emergency and fail-closed decisions are made.
 *
 * Filtered fields are an optional control/forecast channel. A direct helper
 * normally produces only raw values; the fusion bridge may populate both.
 */
typedef struct {
    int temp_available;
    int freq_ratio_available;
    int32_t package_temp_millic;
    uint32_t freq_ratio_milli;

    int filtered_temp_available;
    int filtered_freq_ratio_available;
    int32_t filtered_package_temp_millic;
    uint32_t filtered_freq_ratio_milli;
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
    time_t temp_retry_deadline;
    time_t freq_retry_deadline;
    time_t msr_retry_deadline;
    int temp_backoff_seconds;
    int freq_backoff_seconds;
    int msr_backoff_seconds;
} tsd_telemetry_helper_t;

int tsd_telemetry_helper_init(tsd_telemetry_helper_t *helper, int cpu);
void tsd_telemetry_helper_destroy(tsd_telemetry_helper_t *helper);
int tsd_telemetry_helper_sample(tsd_telemetry_helper_t *helper, tsd_telemetry_sample_t *out);

#ifdef __cplusplus
}
#endif

#endif /* TSD_TELEMETRY_HELPER_H */
