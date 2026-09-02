#ifndef TSD_THERMAL_EVAL_TYPES_H
#define TSD_THERMAL_EVAL_TYPES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint64_t cpi_milli;
    uint32_t ratio_milli;
    uint32_t trimmed_ratio_milli;
    uint64_t llc_mpki_milli;
    uint64_t severity_milli;
    uint64_t thermal_severity_milli;
    int memory_bound;

    /* Authoritative raw safety channel. */
    int temp_available;
    int freq_ratio_available;
    int32_t package_temp_millic;
    uint32_t freq_ratio_milli;

    /* Optional filtered control/forecast channel. */
    int filtered_temp_available;
    int filtered_freq_ratio_available;
    int32_t filtered_package_temp_millic;
    uint32_t filtered_freq_ratio_milli;
} tsd_thermal_eval_t;

#ifdef __cplusplus
}
#endif

#endif /* TSD_THERMAL_EVAL_TYPES_H */
