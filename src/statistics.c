#include <thermal/simd/statistics.h>

#include <string.h>

uint64_t tsd_update_ewma(uint64_t prev, uint64_t sample, unsigned shift) {
    if (prev == 0 || shift == 0) {
        return sample;
    }
    if (sample == prev) {
        return prev;
    }
    if (sample > prev) {
        uint64_t delta = sample - prev;
        uint64_t step = delta >> shift;
        if (step == 0) step = 1;
        uint64_t next = prev + step;
        return next > sample ? sample : next;
    }
    uint64_t delta = prev - sample;
    uint64_t step = delta >> shift;
    if (step == 0) step = 1;
    uint64_t next = prev - step;
    return next < sample ? sample : next;
}

uint32_t tsd_compute_trimmed_mean(const uint32_t *values, size_t count) {
    if (!values || count == 0) {
        return 0;
    }
    uint32_t scratch[TSD_RATIO_HISTORY];
    if (count > TSD_RATIO_HISTORY) {
        count = TSD_RATIO_HISTORY;
    }
    memcpy(scratch, values, count * sizeof(uint32_t));
    for (size_t i = 1; i < count; ++i) {
        uint32_t key = scratch[i];
        size_t j = i;
        while (j > 0 && scratch[j - 1] > key) {
            scratch[j] = scratch[j - 1];
            --j;
        }
        scratch[j] = key;
    }
    if (count <= 2) {
        uint64_t sum = 0;
        for (size_t i = 0; i < count; ++i) {
            sum += scratch[i];
        }
        return (uint32_t)(sum / count);
    }
    size_t start = 1;
    size_t end = count - 1;
    uint64_t sum = 0;
    size_t samples = 0;
    for (size_t i = start; i < end; ++i) {
        sum += scratch[i];
        ++samples;
    }
    return samples ? (uint32_t)(sum / samples) : scratch[count / 2];
}
