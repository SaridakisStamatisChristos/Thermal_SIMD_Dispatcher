#ifndef TSD_CONFIG_PARSER_H
#define TSD_CONFIG_PARSER_H

#include <stdint.h>
#include <stddef.h>

int tsd_parse_int_option(const char *value, long min, long max, int *out);
int tsd_parse_ms_option(const char *value, int min_ms, int max_ms, int *out_us);
int tsd_parse_ratio_option(const char *value, double min, double max, double *ratio_out, uint64_t *scaled_out);
int tsd_parse_double_option(const char *value, double min, double max, double *out);
int tsd_compute_ticks_from_ms(int interval_us, int ms, int *out_ticks, long long *raw_ticks_out);

#endif
