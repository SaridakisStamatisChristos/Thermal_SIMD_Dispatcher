#ifndef TSD_OBSERVABILITY_METRICS_EXPORTER_H
#define TSD_OBSERVABILITY_METRICS_EXPORTER_H

#include <stdint.h>

#include <thermal/simd/simd_width.h>

#ifdef __cplusplus
extern "C" {
#endif

int tsd_metrics_exporter_start(const char *bind_address, uint16_t port);
void tsd_metrics_exporter_stop(void);
uint16_t tsd_metrics_exporter_listen_port(void);
void tsd_metrics_exporter_record_patch(simd_width_t from, simd_width_t to, int rc, uint64_t dwell_ms);
void tsd_metrics_exporter_observe_dwell(simd_width_t width, uint64_t dwell_ms);
void tsd_metrics_exporter_record_sensor_health(const char *sensor_name,
                                              int socket,
                                              double health,
                                              double quality,
                                              int valid);

#ifdef __cplusplus
}
#endif

#endif /* TSD_OBSERVABILITY_METRICS_EXPORTER_H */
