#ifndef TSD_OBSERVABILITY_METRICS_EXPORTER_H
#define TSD_OBSERVABILITY_METRICS_EXPORTER_H

#include <stdint.h>

#include <thermal/simd/simd_width.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct tsd_metrics_tls_config_s {
    const char *certificate_path;
    const char *private_key_path;
    const char *ca_certificate_path;
    int require_client_auth;
} tsd_metrics_tls_config_t;

typedef struct tsd_metrics_basic_auth_s {
    const char *username;
    const char *password;
} tsd_metrics_basic_auth_t;

typedef struct tsd_metrics_exporter_config_s {
    const char *bind_address;
    uint16_t port;
    const tsd_metrics_tls_config_t *tls;
    const tsd_metrics_basic_auth_t *basic_auth;
    const char *statsd_host;
    uint16_t statsd_port;
} tsd_metrics_exporter_config_t;

int tsd_metrics_exporter_start_with_config(const tsd_metrics_exporter_config_t *config);
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
