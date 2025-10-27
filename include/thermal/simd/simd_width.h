#ifndef TSD_SIMD_WIDTH_H
#define TSD_SIMD_WIDTH_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SIMD_SSE41 = 0,
    SIMD_AVX2,
    SIMD_AVX512
} simd_width_t;

#ifdef __cplusplus
}
#endif

#endif /* TSD_SIMD_WIDTH_H */
