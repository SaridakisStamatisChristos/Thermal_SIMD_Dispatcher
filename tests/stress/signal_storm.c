#define _GNU_SOURCE
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "stress_common.h"

typedef struct {
    _Atomic int *stop_flag;
    atomic_uint *cycles;
    atomic_uint *failures;
} patch_worker_arg_t;

typedef struct {
    pthread_t target;
    int signal_number;
    int interval_us;
    _Atomic int *stop_flag;
    atomic_uint *sent;
} signal_worker_arg_t;

static volatile sig_atomic_t g_signal_count = 0;

static void stress_signal_handler(int sig) {
    (void)sig;
    g_signal_count++;
}

static int parse_positive(const char *value, int fallback) {
    if (!value) {
        return fallback;
    }
    char *end = NULL;
    long parsed = strtol(value, &end, 10);
    if (end == value || parsed <= 0 || parsed > INT_MAX) {
        return fallback;
    }
    return (int)parsed;
}

static int resolve_duration(int argc, char **argv) {
    int seconds = 30;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--duration-seconds") == 0 && i + 1 < argc) {
            seconds = parse_positive(argv[++i], seconds);
        } else if (strncmp(argv[i], "--duration-seconds=", 20) == 0) {
            seconds = parse_positive(argv[i] + 20, seconds);
        }
    }
    const char *duration_env = getenv("TSD_STRESS_DURATION");
    if (duration_env) {
        seconds = parse_positive(duration_env, seconds);
    }
    return seconds;
}

static int resolve_rate(int argc, char **argv) {
    int rate = 250;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--signal-rate") == 0 && i + 1 < argc) {
            rate = parse_positive(argv[++i], rate);
        } else if (strncmp(argv[i], "--signal-rate=", 15) == 0) {
            rate = parse_positive(argv[i] + 15, rate);
        }
    }
    const char *rate_env = getenv("TSD_SIGNAL_RATE");
    if (rate_env) {
        rate = parse_positive(rate_env, rate);
    }
    return rate > 0 ? rate : 1;
}

static void *patch_worker(void *arg) {
    patch_worker_arg_t *ctx = (patch_worker_arg_t *)arg;
    const simd_width_t widths[] = {SIMD_SSE41, SIMD_AVX2, SIMD_AVX512};
    size_t width_count = sizeof(widths) / sizeof(widths[0]);
    while (!atomic_load_explicit(ctx->stop_flag, memory_order_acquire)) {
        for (size_t i = 0; i < width_count; ++i) {
            if (tsd_stress_patch(widths[i]) != 0) {
                atomic_fetch_add_explicit(ctx->failures, 1U, memory_order_relaxed);
            }
        }
        atomic_fetch_add_explicit(ctx->cycles, 1U, memory_order_relaxed);
        tsd_test_run_workload(512);
        sched_yield();
    }
    return NULL;
}

static void *signal_worker(void *arg) {
    signal_worker_arg_t *ctx = (signal_worker_arg_t *)arg;
    while (!atomic_load_explicit(ctx->stop_flag, memory_order_acquire)) {
        if (pthread_kill(ctx->target, ctx->signal_number) == 0) {
            atomic_fetch_add_explicit(ctx->sent, 1U, memory_order_relaxed);
        }
        usleep((useconds_t)(ctx->interval_us > 0 ? ctx->interval_us : 1));
    }
    return NULL;
}

static void install_signal_handlers(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = stress_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGUSR1, &sa, NULL);
    sigaction(SIGUSR2, &sa, NULL);
}

int main(int argc, char **argv) {
    int duration_seconds = resolve_duration(argc, argv);
    int signal_rate = resolve_rate(argc, argv);
    if (duration_seconds <= 0) {
        duration_seconds = 1;
    }
    if (signal_rate <= 0) {
        signal_rate = 1;
    }

    if (tsd_stress_prepare_runtime() != 0) {
        return EXIT_FAILURE;
    }

    install_signal_handlers();

    atomic_int stop_flag = ATOMIC_VAR_INIT(0);
    atomic_uint patch_cycles = ATOMIC_VAR_INIT(0);
    atomic_uint patch_failures = ATOMIC_VAR_INIT(0);
    atomic_uint signals_sent = ATOMIC_VAR_INIT(0);

    patch_worker_arg_t patch_arg = {
        .stop_flag = &stop_flag,
        .cycles = &patch_cycles,
        .failures = &patch_failures,
    };

    pthread_t patch_thread;
    if (pthread_create(&patch_thread, NULL, patch_worker, &patch_arg) != 0) {
        fprintf(stderr, "failed to spawn patch worker\n");
        tsd_stress_teardown_runtime();
        return EXIT_FAILURE;
    }

    int interval_us = 1000000 / signal_rate;
    if (interval_us <= 0) {
        interval_us = 1;
    }

    signal_worker_arg_t signal_args[2];
    pthread_t signal_threads[2];
    const int signals[2] = {SIGUSR1, SIGUSR2};

    for (int i = 0; i < 2; ++i) {
        signal_args[i].target = patch_thread;
        signal_args[i].signal_number = signals[i];
        signal_args[i].interval_us = interval_us;
        signal_args[i].stop_flag = &stop_flag;
        signal_args[i].sent = &signals_sent;
        if (pthread_create(&signal_threads[i], NULL, signal_worker, &signal_args[i]) != 0) {
            fprintf(stderr, "failed to create signal worker %d\n", i);
            atomic_store_explicit(&stop_flag, 1, memory_order_release);
            for (int j = 0; j < i; ++j) {
                pthread_join(signal_threads[j], NULL);
            }
            pthread_join(patch_thread, NULL);
            tsd_stress_teardown_runtime();
            return EXIT_FAILURE;
        }
    }

    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += duration_seconds;

    while (1) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec > deadline.tv_sec ||
            (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec)) {
            break;
        }
        tsd_test_run_workload(128);
        usleep(50000);
    }

    atomic_store_explicit(&stop_flag, 1, memory_order_release);

    for (int i = 0; i < 2; ++i) {
        pthread_join(signal_threads[i], NULL);
    }
    pthread_join(patch_thread, NULL);

    unsigned int handled = (unsigned int)g_signal_count;
    unsigned int sent = atomic_load_explicit(&signals_sent, memory_order_relaxed);
    unsigned int cycles = atomic_load_explicit(&patch_cycles, memory_order_relaxed);
    unsigned int failures = atomic_load_explicit(&patch_failures, memory_order_relaxed);

    printf("signal_storm summary\n");
    printf("  duration_seconds: %d\n", duration_seconds);
    printf("  signal_rate_per_thread: %d\n", signal_rate);
    printf("  signals_sent: %u\n", sent);
    printf("  signals_handled: %u\n", handled);
    printf("  patch_cycles: %u\n", cycles);
    printf("  patch_failures: %u\n", failures);
    fflush(stdout);

    tsd_stress_teardown_runtime();

    int rc = 0;
    if (failures != 0) {
        fprintf(stderr, "patch operations failed under signal storm\n");
        rc = 1;
    }
    if (handled < (unsigned int)(duration_seconds * signal_rate)) {
        fprintf(stderr, "fewer signals handled (%u) than expected baseline (%d)\n",
                handled, duration_seconds * signal_rate);
        rc = 1;
    }
    if (cycles == 0U) {
        fprintf(stderr, "patch thread did not complete any cycles\n");
        rc = 1;
    }

    return rc == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
