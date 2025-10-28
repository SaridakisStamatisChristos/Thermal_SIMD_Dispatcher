#ifndef TSD_LOGGING_H
#define TSD_LOGGING_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TSD_LOG_LEVEL_ERROR = 0,
    TSD_LOG_LEVEL_WARN  = 1,
    TSD_LOG_LEVEL_INFO  = 2,
    TSD_LOG_LEVEL_DEBUG = 3,
} tsd_log_level_t;

void tsd_log_set_level(tsd_log_level_t level);
tsd_log_level_t tsd_log_get_level(void);
int tsd_log_should_log(tsd_log_level_t level);
void tsd_log(tsd_log_level_t level, const char *component, const char *fmt, ...);
int tsd_log_level_from_string(const char *level_str, tsd_log_level_t *out_level);
const char* tsd_log_level_to_string(tsd_log_level_t level);
const char* tsd_log_strerror(int errnum, char *buf, size_t buf_len);

#define tsd_log_error(component, fmt, ...) tsd_log(TSD_LOG_LEVEL_ERROR, component, fmt, ##__VA_ARGS__)
#define tsd_log_warn(component, fmt, ...)  tsd_log(TSD_LOG_LEVEL_WARN,  component, fmt, ##__VA_ARGS__)
#define tsd_log_info(component, fmt, ...)  tsd_log(TSD_LOG_LEVEL_INFO,  component, fmt, ##__VA_ARGS__)
#define tsd_log_debug(component, fmt, ...) tsd_log(TSD_LOG_LEVEL_DEBUG, component, fmt, ##__VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif /* TSD_LOGGING_H */
