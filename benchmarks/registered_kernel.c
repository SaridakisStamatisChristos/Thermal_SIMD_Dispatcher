#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <immintrin.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <config/runtime_flags.h>
#include <observability/metrics_exporter.h>
#include <thermal/simd/adaptive_dispatch.h>
#include <thermal/simd/logging.h>
#include <thermal/simd/runtime.h>
#include <thermal/simd/thermal_config.h>
#include <thermal/simd/thermal_cpu.h>

#if !defined(__GNUC__) && !defined(__clang__)
#error "registered-kernel benchmark requires GCC or Clang target attributes"
#endif

#define TSD_TARGET(features) __attribute__((target(features), noinline))

typedef enum benchmark_mode_e {
    BENCH_SSE41 = 0,
    BENCH_AVX2,
    BENCH_AVX512,
    BENCH_ADAPTIVE,
} benchmark_mode_t;

typedef struct benchmark_options_s {
    benchmark_mode_t mode;
    simd_width_t adaptive_max;
    size_t work_items;
    size_t chunk_items;
    unsigned int rounds;
    double duration_seconds;
    double warmup_seconds;
    int run_forever;
    uint16_t metrics_port;
    const char *metrics_bind;
    const char *result_json;
} benchmark_options_t;

typedef struct kernel_context_s {
    uint32_t *input;
    uint32_t *output;
    size_t item_count;
    unsigned int rounds;
} kernel_context_t;

typedef struct benchmark_result_s {
    uint64_t completed_passes;
    uint64_t completed_work_items;
    uint64_t width_work_items[3];
    uint64_t checksum;
    double elapsed_seconds;
    int pinned_cpu;
    int stopped_by_signal;
} benchmark_result_t;

static volatile sig_atomic_t g_stop_requested = 0;
static kernel_context_t *g_baseline_context = NULL;

static void stop_handler(int signal_number) {
    (void)signal_number;
    g_stop_requested = 1;
}

static int install_signal_handlers(void) {
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = stop_handler;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) != 0 || sigaction(SIGTERM, &action, NULL) != 0) {
        return -1;
    }
    return 0;
}

static uint32_t mix_scalar(uint32_t value, unsigned int rounds) {
    for (unsigned int round = 0; round < rounds; ++round) {
        value = value * UINT32_C(1664525) + UINT32_C(1013904223);
        value ^= value >> 13;
        value = value * UINT32_C(2246822519) + UINT32_C(3266489917);
        value ^= value << 7;
    }
    return value;
}

TSD_TARGET("sse4.1")
static int kernel_sse41(void *opaque, size_t offset, size_t work_items) {
    kernel_context_t *context = (kernel_context_t *)opaque;
    if (!context || offset > context->item_count || work_items > context->item_count - offset) {
        return EINVAL;
    }

    const __m128i multiplier_a = _mm_set1_epi32((int)UINT32_C(1664525));
    const __m128i addend_a = _mm_set1_epi32((int)UINT32_C(1013904223));
    const __m128i multiplier_b = _mm_set1_epi32((int)UINT32_C(2246822519));
    const __m128i addend_b = _mm_set1_epi32((int)UINT32_C(3266489917));
    size_t index = offset;
    const size_t end = offset + work_items;
    for (; index + 4 <= end; index += 4) {
        __m128i value = _mm_loadu_si128((const __m128i *)(const void *)(context->input + index));
        for (unsigned int round = 0; round < context->rounds; ++round) {
            value = _mm_add_epi32(_mm_mullo_epi32(value, multiplier_a), addend_a);
            value = _mm_xor_si128(value, _mm_srli_epi32(value, 13));
            value = _mm_add_epi32(_mm_mullo_epi32(value, multiplier_b), addend_b);
            value = _mm_xor_si128(value, _mm_slli_epi32(value, 7));
        }
        _mm_storeu_si128((__m128i *)(void *)(context->output + index), value);
    }
    for (; index < end; ++index) {
        context->output[index] = mix_scalar(context->input[index], context->rounds);
    }
    return 0;
}

TSD_TARGET("avx2")
static int kernel_avx2(void *opaque, size_t offset, size_t work_items) {
    kernel_context_t *context = (kernel_context_t *)opaque;
    if (!context || offset > context->item_count || work_items > context->item_count - offset) {
        return EINVAL;
    }

    const __m256i multiplier_a = _mm256_set1_epi32((int)UINT32_C(1664525));
    const __m256i addend_a = _mm256_set1_epi32((int)UINT32_C(1013904223));
    const __m256i multiplier_b = _mm256_set1_epi32((int)UINT32_C(2246822519));
    const __m256i addend_b = _mm256_set1_epi32((int)UINT32_C(3266489917));
    size_t index = offset;
    const size_t end = offset + work_items;
    for (; index + 8 <= end; index += 8) {
        __m256i value = _mm256_loadu_si256((const __m256i *)(const void *)(context->input + index));
        for (unsigned int round = 0; round < context->rounds; ++round) {
            value = _mm256_add_epi32(_mm256_mullo_epi32(value, multiplier_a), addend_a);
            value = _mm256_xor_si256(value, _mm256_srli_epi32(value, 13));
            value = _mm256_add_epi32(_mm256_mullo_epi32(value, multiplier_b), addend_b);
            value = _mm256_xor_si256(value, _mm256_slli_epi32(value, 7));
        }
        _mm256_storeu_si256((__m256i *)(void *)(context->output + index), value);
    }
    for (; index < end; ++index) {
        context->output[index] = mix_scalar(context->input[index], context->rounds);
    }
    return 0;
}

TSD_TARGET("avx512f")
static int kernel_avx512(void *opaque, size_t offset, size_t work_items) {
    kernel_context_t *context = (kernel_context_t *)opaque;
    if (!context || offset > context->item_count || work_items > context->item_count - offset) {
        return EINVAL;
    }

    const __m512i multiplier_a = _mm512_set1_epi32((int)UINT32_C(1664525));
    const __m512i addend_a = _mm512_set1_epi32((int)UINT32_C(1013904223));
    const __m512i multiplier_b = _mm512_set1_epi32((int)UINT32_C(2246822519));
    const __m512i addend_b = _mm512_set1_epi32((int)UINT32_C(3266489917));
    size_t index = offset;
    const size_t end = offset + work_items;
    for (; index + 16 <= end; index += 16) {
        __m512i value = _mm512_loadu_si512((const void *)(context->input + index));
        for (unsigned int round = 0; round < context->rounds; ++round) {
            value = _mm512_add_epi32(_mm512_mullo_epi32(value, multiplier_a), addend_a);
            value = _mm512_xor_si512(value, _mm512_srli_epi32(value, 13));
            value = _mm512_add_epi32(_mm512_mullo_epi32(value, multiplier_b), addend_b);
            value = _mm512_xor_si512(value, _mm512_slli_epi32(value, 7));
        }
        _mm512_storeu_si512((void *)(context->output + index), value);
    }
    for (; index < end; ++index) {
        context->output[index] = mix_scalar(context->input[index], context->rounds);
    }
    return 0;
}

static const char *mode_name(benchmark_mode_t mode) {
    switch (mode) {
        case BENCH_SSE41: return "sse41";
        case BENCH_AVX2: return "avx2";
        case BENCH_AVX512: return "avx512";
        case BENCH_ADAPTIVE: return "adaptive";
    }
    return "invalid";
}

static const char *width_name(simd_width_t width) {
    switch (width) {
        case SIMD_SSE41: return "sse41";
        case SIMD_AVX2: return "avx2";
        case SIMD_AVX512: return "avx512";
    }
    return "invalid";
}

static int parse_mode(const char *value, benchmark_mode_t *out) {
    if (strcmp(value, "sse41") == 0) *out = BENCH_SSE41;
    else if (strcmp(value, "avx2") == 0) *out = BENCH_AVX2;
    else if (strcmp(value, "avx512") == 0) *out = BENCH_AVX512;
    else if (strcmp(value, "adaptive") == 0) *out = BENCH_ADAPTIVE;
    else return -1;
    return 0;
}

static int parse_max_isa(const char *value, simd_width_t *out) {
    if (strcmp(value, "avx2") == 0) *out = SIMD_AVX2;
    else if (strcmp(value, "avx512") == 0) *out = SIMD_AVX512;
    else return -1;
    return 0;
}

static int parse_size(const char *value, size_t minimum, size_t maximum, size_t *out) {
    errno = 0;
    char *end = NULL;
    unsigned long long parsed = strtoull(value, &end, 10);
    if (errno != 0 || !end || *end != '\0' || parsed < minimum || parsed > maximum) return -1;
    *out = (size_t)parsed;
    return 0;
}

static int parse_unsigned(const char *value, unsigned int minimum, unsigned int maximum,
                          unsigned int *out) {
    size_t parsed = 0;
    if (parse_size(value, minimum, maximum, &parsed) != 0) return -1;
    *out = (unsigned int)parsed;
    return 0;
}

static int parse_double_value(const char *value, double minimum, double maximum, double *out) {
    errno = 0;
    char *end = NULL;
    double parsed = strtod(value, &end);
    if (errno != 0 || !end || *end != '\0' || !isfinite(parsed) ||
        parsed < minimum || parsed > maximum) return -1;
    *out = parsed;
    return 0;
}

static void print_usage(const char *program) {
    fprintf(stderr,
            "usage: %s --mode=sse41|avx2|avx512|adaptive [options]\n"
            "  --max-isa=avx2|avx512       adaptive-mode ceiling (default: avx2)\n"
            "  --duration-seconds=N        measured duration (default: 5)\n"
            "  --warmup-seconds=N          unreported warmup (default: 0)\n"
            "  --run-forever               adaptive run ends on SIGINT/SIGTERM\n"
            "  --work-items=N              uint32 items per pass; multiple of 16 (default: 1048576)\n"
            "  --chunk-items=N             decision boundary; multiple of 16 (default: 65536)\n"
            "  --rounds=N                  integer-mix rounds per item (default: 32)\n"
            "  --metrics-port=N            adaptive metrics port (default: 19464)\n"
            "  --metrics-bind=ADDR         adaptive metrics address (default: 127.0.0.1)\n"
            "  --result-json=PATH          atomically write machine-readable result\n",
            program);
}

static int parse_options(int argc, char **argv, benchmark_options_t *options) {
    *options = (benchmark_options_t){
        .mode = BENCH_SSE41,
        .adaptive_max = SIMD_AVX2,
        .work_items = 1048576U,
        .chunk_items = 65536U,
        .rounds = 32U,
        .duration_seconds = 5.0,
        .warmup_seconds = 0.0,
        .run_forever = 0,
        .metrics_port = 19464U,
        .metrics_bind = "127.0.0.1",
        .result_json = NULL,
    };
    int mode_seen = 0;
    for (int index = 1; index < argc; ++index) {
        const char *argument = argv[index];
        if (strncmp(argument, "--mode=", 7) == 0) {
            if (parse_mode(argument + 7, &options->mode) != 0) return -1;
            mode_seen = 1;
        } else if (strncmp(argument, "--max-isa=", 10) == 0) {
            if (parse_max_isa(argument + 10, &options->adaptive_max) != 0) return -1;
        } else if (strncmp(argument, "--duration-seconds=", 19) == 0) {
            if (parse_double_value(argument + 19, 0.001, 86400.0, &options->duration_seconds) != 0) return -1;
        } else if (strncmp(argument, "--warmup-seconds=", 17) == 0) {
            if (parse_double_value(argument + 17, 0.0, 3600.0, &options->warmup_seconds) != 0) return -1;
        } else if (strcmp(argument, "--run-forever") == 0) {
            options->run_forever = 1;
        } else if (strncmp(argument, "--work-items=", 13) == 0) {
            if (parse_size(argument + 13, 16U, (size_t)1U << 29, &options->work_items) != 0) return -1;
        } else if (strncmp(argument, "--chunk-items=", 14) == 0) {
            if (parse_size(argument + 14, 1U, (size_t)1U << 29, &options->chunk_items) != 0) return -1;
        } else if (strncmp(argument, "--rounds=", 9) == 0) {
            if (parse_unsigned(argument + 9, 1U, 10000U, &options->rounds) != 0) return -1;
        } else if (strncmp(argument, "--metrics-port=", 15) == 0) {
            size_t port = 0;
            if (parse_size(argument + 15, 1U, 65535U, &port) != 0) return -1;
            options->metrics_port = (uint16_t)port;
        } else if (strncmp(argument, "--metrics-bind=", 15) == 0) {
            options->metrics_bind = argument + 15;
            if (options->metrics_bind[0] == '\0') return -1;
        } else if (strncmp(argument, "--result-json=", 14) == 0) {
            options->result_json = argument + 14;
            if (options->result_json[0] == '\0') return -1;
        } else if (strcmp(argument, "--help") == 0) {
            print_usage(argv[0]);
            exit(EXIT_SUCCESS);
        } else {
            return -1;
        }
    }
    if (!mode_seen || options->chunk_items > options->work_items ||
        options->work_items % 16U != 0U || options->chunk_items % 16U != 0U ||
        (options->run_forever && options->mode != BENCH_ADAPTIVE)) {
        return -1;
    }
    return 0;
}

static uint64_t monotonic_ns(void) {
    struct timespec timestamp;
    if (clock_gettime(CLOCK_MONOTONIC, &timestamp) != 0) return 0;
    return (uint64_t)timestamp.tv_sec * UINT64_C(1000000000) + (uint64_t)timestamp.tv_nsec;
}

static int allocate_context(const benchmark_options_t *options, kernel_context_t *context) {
    uint32_t *input = NULL;
    uint32_t *output = NULL;
    if (posix_memalign((void **)&input, 64U, options->work_items * sizeof(*input)) != 0 ||
        posix_memalign((void **)&output, 64U, options->work_items * sizeof(*output)) != 0) {
        free(input);
        free(output);
        return -1;
    }
    for (size_t index = 0; index < options->work_items; ++index) {
        input[index] = (uint32_t)index * UINT32_C(2654435761) ^ UINT32_C(0xa5a5a5a5);
        output[index] = 0U;
    }
    *context = (kernel_context_t){
        .input = input,
        .output = output,
        .item_count = options->work_items,
        .rounds = options->rounds,
    };
    return 0;
}

static void free_context(kernel_context_t *context) {
    if (!context) return;
    free(context->input);
    free(context->output);
    memset(context, 0, sizeof(*context));
}

static uint64_t checksum_output(const kernel_context_t *context) {
    uint64_t hash = UINT64_C(1469598103934665603);
    for (size_t index = 0; index < context->item_count; ++index) {
        uint32_t value = context->output[index];
        for (unsigned int byte = 0; byte < 4U; ++byte) {
            hash ^= (uint8_t)(value >> (byte * 8U));
            hash *= UINT64_C(1099511628211);
        }
    }
    return hash;
}

static int pin_fixed_workload(void) {
    cpu_set_t allowed;
    if (sched_getaffinity(0, sizeof(allowed), &allowed) != 0) return -1;
    int selected = -1;
    for (size_t cpu = 0; cpu < (size_t)CPU_SETSIZE; ++cpu) {
        if (CPU_ISSET(cpu, &allowed)) {
            selected = (int)cpu;
            break;
        }
    }
    if (selected < 0) {
        errno = ENODEV;
        return -1;
    }
    cpu_set_t pinned;
    CPU_ZERO(&pinned);
    CPU_SET((size_t)selected, &pinned);
    if (sched_setaffinity(0, sizeof(pinned), &pinned) != 0) return -1;
    return selected;
}

static tsd_kernel_fn_v2 fixed_kernel(benchmark_mode_t mode) {
    switch (mode) {
        case BENCH_SSE41: return kernel_sse41;
        case BENCH_AVX2: return kernel_avx2;
        case BENCH_AVX512: return kernel_avx512;
        case BENCH_ADAPTIVE: break;
    }
    return NULL;
}

static int run_phase(const benchmark_options_t *options,
                     kernel_context_t *context,
                     tsd_kernel_dispatch_v2_t *dispatch,
                     double duration_seconds,
                     int collect,
                     benchmark_result_t *result) {
    tsd_kernel_fn_v2 direct = fixed_kernel(options->mode);
    uint64_t start_ns = monotonic_ns();
    if (start_ns == 0) return -1;

    do {
        size_t offset = 0;
        while (offset < options->work_items) {
            size_t count = options->work_items - offset;
            if (count > options->chunk_items) count = options->chunk_items;
            simd_width_t used = SIMD_SSE41;
            int rc;
            if (options->mode == BENCH_ADAPTIVE) {
                rc = tsd_kernel_dispatch_v2_execute(dispatch, offset, count, &used);
            } else {
                rc = direct(context, offset, count);
                used = (simd_width_t)options->mode;
            }
            if (rc != 0) {
                errno = rc;
                return -1;
            }
            if (used < SIMD_SSE41 || used > SIMD_AVX512) {
                errno = ERANGE;
                return -1;
            }
            if (collect) {
                result->completed_work_items += (uint64_t)count;
                result->width_work_items[used] += (uint64_t)count;
            }
            offset += count;
        }
        if (collect) result->completed_passes++;
        uint64_t now_ns = monotonic_ns();
        if (now_ns == 0) return -1;
        double elapsed = (double)(now_ns - start_ns) / 1e9;
        if ((!collect || !options->run_forever) && elapsed >= duration_seconds) break;
    } while (!g_stop_requested);

    uint64_t end_ns = monotonic_ns();
    if (end_ns == 0) return -1;
    if (collect) result->elapsed_seconds = (double)(end_ns - start_ns) / 1e9;
    return 0;
}

static void baseline_workload(void) {
    if (!g_baseline_context) return;
    size_t count = g_baseline_context->item_count < 64U ? g_baseline_context->item_count : 64U;
    (void)kernel_sse41(g_baseline_context, 0U, count);
}

static int emit_result(FILE *stream,
                       const benchmark_options_t *options,
                       const benchmark_result_t *result) {
    double throughput = result->elapsed_seconds > 0.0
                            ? (double)result->completed_work_items / result->elapsed_seconds
                            : 0.0;
    int written = fprintf(
        stream,
        "{\n"
        "  \"schema_version\": 1,\n"
        "  \"mode\": \"%s\",\n"
        "  \"adaptive_max_isa\": \"%s\",\n"
        "  \"work_items_per_pass\": %zu,\n"
        "  \"chunk_items\": %zu,\n"
        "  \"rounds_per_item\": %u,\n"
        "  \"elapsed_seconds\": %.9f,\n"
        "  \"completed_passes\": %" PRIu64 ",\n"
        "  \"completed_work_items\": %" PRIu64 ",\n"
        "  \"items_per_second\": %.3f,\n"
        "  \"checksum_fnv1a64\": \"%016" PRIx64 "\",\n"
        "  \"pinned_cpu\": %d,\n"
        "  \"stopped_by_signal\": %s,\n"
        "  \"width_work_items\": {\"sse41\": %" PRIu64 ", \"avx2\": %" PRIu64
        ", \"avx512\": %" PRIu64 "},\n"
        "  \"effective_policy\": {\"temp_ceiling_c\": %d, \"safety_margin_c\": %d, "
        "\"emergency_margin_c\": %d, \"thermal_temp_weight_milli\": %d, "
        "\"thermal_ratio_weight_milli\": %d}\n"
        "}\n",
        mode_name(options->mode), width_name(options->adaptive_max), options->work_items,
        options->chunk_items, options->rounds, result->elapsed_seconds,
        result->completed_passes, result->completed_work_items, throughput, result->checksum,
        result->pinned_cpu, result->stopped_by_signal ? "true" : "false",
        result->width_work_items[SIMD_SSE41], result->width_work_items[SIMD_AVX2],
        result->width_work_items[SIMD_AVX512], g_tsd_config.predictive_temp_ceiling_c,
        g_tsd_config.predictive_safety_margin_c, g_tsd_config.predictive_emergency_margin_c,
        g_tsd_config.thermal_temp_weight_milli, g_tsd_config.thermal_ratio_weight_milli);
    return written < 0 ? -1 : 0;
}

static int write_result(const benchmark_options_t *options, const benchmark_result_t *result) {
    if (!options->result_json) return emit_result(stdout, options, result);

    size_t length = strlen(options->result_json);
    char *temporary = malloc(length + 5U);
    if (!temporary) return -1;
    memcpy(temporary, options->result_json, length);
    memcpy(temporary + length, ".tmp", 5U);

    FILE *stream = fopen(temporary, "w");
    if (!stream) {
        free(temporary);
        return -1;
    }
    int rc = emit_result(stream, options, result);
    if (rc == 0 && fflush(stream) != 0) rc = -1;
    if (rc == 0 && fsync(fileno(stream)) != 0) rc = -1;
    if (fclose(stream) != 0) rc = -1;
    if (rc == 0 && rename(temporary, options->result_json) != 0) rc = -1;
    if (rc != 0) (void)unlink(temporary);
    free(temporary);
    return rc;
}

int main(int argc, char **argv) {
    benchmark_options_t options;
    if (parse_options(argc, argv, &options) != 0) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }
    if (install_signal_handlers() != 0) {
        perror("sigaction");
        return EXIT_FAILURE;
    }

    tsd_runtime_config_set_defaults(&g_tsd_config);
    g_tsd_config.allow_avx512 = options.adaptive_max == SIMD_AVX512;
    if (tsd_runtime_config_refresh_ticks(&g_tsd_config) != 0) {
        fprintf(stderr, "failed to initialize runtime timing configuration\n");
        return EXIT_FAILURE;
    }
    tsd_log_set_level(TSD_LOG_LEVEL_INFO);

    tsd_runtime_config detection = g_tsd_config;
    detection.allow_avx512 = 1;
    simd_width_t host_max = tsd_detect_max_simd(&detection);
    simd_width_t required = options.mode == BENCH_ADAPTIVE
                                ? options.adaptive_max
                                : (simd_width_t)options.mode;
    if (!tsd_cpu_has_sse41() || host_max < required) {
        fprintf(stderr, "requested mode %s is unavailable on this host\n", mode_name(options.mode));
        return EXIT_FAILURE;
    }

    kernel_context_t context;
    if (allocate_context(&options, &context) != 0) {
        fprintf(stderr, "failed to allocate aligned benchmark buffers\n");
        return EXIT_FAILURE;
    }

    benchmark_result_t result;
    memset(&result, 0, sizeof(result));
    result.pinned_cpu = -1;
    tsd_kernel_dispatch_v2_t *dispatch = NULL;
    tsd_runtime_t *runtime = NULL;
    kernel_context_t baseline_context;
    memset(&baseline_context, 0, sizeof(baseline_context));
    int metrics_started = 0;
    int status = EXIT_FAILURE;

    if (options.mode == BENCH_ADAPTIVE) {
        benchmark_options_t baseline_options = options;
        baseline_options.work_items = 64U;
        baseline_options.chunk_items = 64U;
        if (allocate_context(&baseline_options, &baseline_context) != 0) {
            fprintf(stderr, "failed to allocate isolated monitor-baseline buffers\n");
            goto cleanup;
        }
        tsd_kernel_variants_v2_t variants = {
            .sse41 = kernel_sse41,
            .avx2 = kernel_avx2,
            .avx512 = kernel_avx512,
            .context = &context,
        };
        if (tsd_kernel_dispatch_v2_create(&variants, &dispatch) != 0) {
            perror("adaptive dispatch creation");
            goto cleanup;
        }
        if (tsd_metrics_exporter_start(options.metrics_bind, options.metrics_port) != 0) {
            perror("metrics exporter start");
            goto cleanup;
        }
        metrics_started = 1;
        tsd_runtime_flags_init();
        g_baseline_context = &baseline_context;
        if (tsd_runtime_start(&runtime, baseline_workload) != 0) {
            perror("adaptive runtime start");
            goto cleanup;
        }
    } else {
        result.pinned_cpu = pin_fixed_workload();
        if (result.pinned_cpu < 0) {
            perror("workload affinity");
            goto cleanup;
        }
    }

    if (options.warmup_seconds > 0.0 &&
        run_phase(&options, &context, dispatch, options.warmup_seconds, 0, &result) != 0) {
        perror("benchmark warmup");
        goto cleanup;
    }
    if (!g_stop_requested &&
        run_phase(&options, &context, dispatch, options.duration_seconds, 1, &result) != 0) {
        perror("benchmark measurement");
        goto cleanup;
    }
    result.stopped_by_signal = g_stop_requested ? 1 : 0;
    result.checksum = checksum_output(&context);
    if (result.completed_passes == 0 || result.completed_work_items == 0) {
        fprintf(stderr, "benchmark completed no measured work\n");
        goto cleanup;
    }
    status = EXIT_SUCCESS;

cleanup:
    if (runtime) {
        tsd_runtime_request_stop(runtime);
        if (tsd_runtime_stop(runtime) != 0) status = EXIT_FAILURE;
        if (tsd_runtime_destroy(runtime) != 0) status = EXIT_FAILURE;
    }
    g_baseline_context = NULL;
    if (metrics_started) tsd_metrics_exporter_stop();
    if (dispatch) tsd_kernel_dispatch_v2_destroy(dispatch);
    if (status == EXIT_SUCCESS && write_result(&options, &result) != 0) {
        perror("write benchmark result");
        status = EXIT_FAILURE;
    }
    free_context(&baseline_context);
    free_context(&context);
    return status;
}
