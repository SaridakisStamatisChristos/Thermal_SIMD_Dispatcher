#ifndef TSD_THERMAL_CPU_H
#define TSD_THERMAL_CPU_H

#include <stdint.h>

#include <thermal/simd/simd_width.h>
#include <thermal/simd/thermal_config.h>

#ifdef __cplusplus
extern "C" {
#endif

extern uint8_t g_tsd_avx_available;

simd_width_t tsd_detect_max_simd(const tsd_runtime_config *cfg);
int tsd_cpu_has_sse41(void);

#ifdef TSD_ENABLE_TESTS
void tsd_cpu_set_detect_override(simd_width_t (*fn)(void));
void tsd_cpu_clear_detect_override(void);
simd_width_t tsd_cpu_detect_ignoring_override(const tsd_runtime_config *cfg);
#endif

#ifdef __cplusplus
}
#endif

#endif /* TSD_THERMAL_CPU_H */
