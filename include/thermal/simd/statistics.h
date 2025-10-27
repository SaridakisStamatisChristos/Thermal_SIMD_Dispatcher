#ifndef TSD_STATISTICS_H
#define TSD_STATISTICS_H

#include <stdint.h>
#include <stddef.h>

#define TSD_RATIO_HISTORY 8

uint64_t tsd_update_ewma(uint64_t prev, uint64_t sample, unsigned shift);
uint32_t tsd_compute_trimmed_mean(const uint32_t *values, size_t count);

#endif
