#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include <thermal/simd/policy/dispatcher_policy.h>

static tsd_thermal_eval_t make_sample(uint32_t ratio_milli, int32_t temp_millic) {
    tsd_thermal_eval_t sample = {0};
    sample.ratio_milli = ratio_milli;
    sample.trimmed_ratio_milli = ratio_milli;
    sample.llc_mpki_milli = 0;
    sample.severity_milli = ratio_milli;
    sample.thermal_severity_milli = ratio_milli;
    sample.temp_available = 1;
    sample.package_temp_millic = temp_millic;
    sample.filtered_temp_available = 1;
    sample.filtered_package_temp_millic = temp_millic;
    sample.freq_ratio_available = 0;
    sample.memory_bound = 0;
    return sample;
}

static tsd_thermal_eval_t make_sample_without_temperature(uint32_t ratio_milli) {
    tsd_thermal_eval_t sample = make_sample(ratio_milli, 0);
    sample.temp_available = 0;
    sample.package_temp_millic = 0;
    sample.filtered_temp_available = 0;
    sample.filtered_package_temp_millic = 0;
    sample.thermal_severity_milli = 0;
    return sample;
}

static void test_predictive_convergence(void) {
    tsd_policy_config cfg;
    tsd_policy_config_set_defaults(&cfg);
    cfg.slo_ratio_milli = 1500;
    cfg.slo_temp_millic = 85000;
    cfg.transition_penalty_down_milli = 400;
    cfg.transition_penalty_up_milli = 600;
    cfg.forecast_horizon = 4;

    tsd_dispatcher_policy_state *state = tsd_dispatcher_policy_create(&cfg);
    assert(state != NULL);

    for (int i = 0; i < 4; ++i) {
        tsd_thermal_eval_t sample = make_sample(2600 + (uint32_t)(i * 150), 95000);
        tsd_dispatcher_policy_record(state, &sample, SIMD_AVX512);
    }

    simd_width_t target = SIMD_AVX512;
    int fallback = 0;
    int rc = tsd_dispatcher_policy_recommend(state, SIMD_AVX512, SIMD_AVX512, &target, &fallback);
    assert(fallback == 0);
    assert(rc == 1);
    assert(target < SIMD_AVX512);

    tsd_dispatcher_policy_destroy(state);
}

static void test_predictive_stability(void) {
    tsd_policy_config cfg;
    tsd_policy_config_set_defaults(&cfg);
    cfg.slo_ratio_milli = 1500;
    cfg.forecast_horizon = 5;
    cfg.transition_penalty_down_milli = 800;
    cfg.transition_penalty_up_milli = 800;

    tsd_dispatcher_policy_state *state = tsd_dispatcher_policy_create(&cfg);
    assert(state != NULL);

    for (int i = 0; i < 5; ++i) {
        tsd_thermal_eval_t sample = make_sample(1400, 80000);
        tsd_dispatcher_policy_record(state, &sample, SIMD_AVX2);
    }

    simd_width_t target = SIMD_AVX2;
    int fallback = 0;
    int rc = tsd_dispatcher_policy_recommend(state, SIMD_AVX2, SIMD_AVX512, &target, &fallback);
    assert(fallback == 0);
    assert(rc == 0);
    assert(target == SIMD_AVX2);

    tsd_dispatcher_policy_destroy(state);
}

static void test_missing_temperature_does_not_invent_thermal_relief(void) {
    tsd_policy_config cfg;
    tsd_policy_config_set_defaults(&cfg);
    cfg.slo_ratio_milli = 1500;
    cfg.slo_temp_millic = 85000;
    cfg.transition_penalty_down_milli = 49;
    cfg.transition_penalty_up_milli = 1000;
    cfg.forecast_horizon = 3;

    tsd_dispatcher_policy_state *state = tsd_dispatcher_policy_create(&cfg);
    assert(state != NULL);

    for (int i = 0; i < 3; ++i) {
        tsd_thermal_eval_t sample = make_sample_without_temperature(1600);
        tsd_dispatcher_policy_record(state, &sample, SIMD_AVX2);
    }

    simd_width_t target = SIMD_AVX2;
    int fallback = 0;
    int rc = tsd_dispatcher_policy_recommend(state, SIMD_AVX2, SIMD_AVX2, &target, &fallback);

    /* Missing temperature is neither 0 C nor evidence that narrowing will
       relieve thermal throttling. Keep the current width instead of inventing
       a physical benefit from absent telemetry. */
    assert(fallback == 0);
    assert(rc == 0);
    assert(target == SIMD_AVX2);

    tsd_dispatcher_policy_destroy(state);
}

static void test_predictive_fallback(void) {
    tsd_policy_config cfg;
    tsd_policy_config_set_defaults(&cfg);
    cfg.forecast_horizon = 3;

    tsd_dispatcher_policy_state *state = tsd_dispatcher_policy_create(&cfg);
    assert(state != NULL);

    tsd_dispatcher_policy_force_fallback(state);
    simd_width_t target = SIMD_AVX2;
    int fallback = 0;
    int rc = tsd_dispatcher_policy_recommend(state, SIMD_AVX2, SIMD_AVX512, &target, &fallback);
    assert(rc == 0);
    assert(fallback == 1);
    assert(target == SIMD_AVX2);

    tsd_dispatcher_policy_destroy(state);
}

int main(void) {
    test_predictive_convergence();
    test_predictive_stability();
    test_missing_temperature_does_not_invent_thermal_relief();
    test_predictive_fallback();
    printf("policy controller tests passed\n");
    return 0;
}
