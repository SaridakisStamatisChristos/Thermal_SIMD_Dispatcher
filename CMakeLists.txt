cmake_minimum_required(VERSION 3.15)
project(thermal_simd_dispatcher VERSION 0.1.0 LANGUAGES C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(THERMAL_SIMD_DISPATCHER_CPU_FLAGS "-msse4.1" CACHE STRING "CPU-specific compiler flags for thermal_simd")

include(CTest)

file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/include/thermal/simd)
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/include/thermal/simd/version.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/include/thermal/simd/version.h
    @ONLY
)

set(THERMAL_SIMD_CORE_SOURCES
    src/config/runtime_flags.c
    src/logging.c
    src/config_parser.c
    src/statistics.c
    src/runtime_metrics.c
    src/health_check.c
    src/healthcheck/sandbox.cpp
    src/telemetry_helper.c
    src/telemetry/evaluator.cpp
    src/telemetry/history_store.cpp
    src/telemetry/sensors.cpp
    src/observability/metrics.cpp
    src/thermal_config.c
    src/thermal_cpu.c
    src/patcher/trampoline.cpp
    src/thermal_perf.c
    src/thermal_signals.c
    src/policy/policy_config.c
    src/policy/dispatcher_policy.cpp
    src/policy/mpc_controller.cpp
)

add_library(thermal_simd_core STATIC ${THERMAL_SIMD_CORE_SOURCES})
target_include_directories(thermal_simd_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

add_executable(thermal_simd src/main.cpp src/thermal_simd.c)
target_compile_options(thermal_simd PRIVATE -O2 -pthread -fPIC -mno-avx ${THERMAL_SIMD_DISPATCHER_CPU_FLAGS})
target_link_libraries(thermal_simd PRIVATE thermal_simd_core pthread)

add_library(tsd_policy_tuner STATIC src/tools/policy_tuner/tuner.cpp)
target_include_directories(tsd_policy_tuner
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(tsd_policy_tuner
    PUBLIC
        thermal_simd_core)

add_executable(policy_tuner src/tools/policy_tuner/main.cpp)
target_link_libraries(policy_tuner PRIVATE tsd_policy_tuner)

if(BUILD_TESTING)
    add_library(thermal_simd_core_tests STATIC ${THERMAL_SIMD_CORE_SOURCES})
    target_include_directories(thermal_simd_core_tests
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_compile_definitions(thermal_simd_core_tests PRIVATE TSD_ENABLE_TESTS)

    add_executable(test_config_parser tests/test_config_parser.c)
    target_link_libraries(test_config_parser PRIVATE thermal_simd_core_tests)
    target_compile_options(test_config_parser PRIVATE -Wall -Wextra)
    add_test(NAME config_parser COMMAND test_config_parser)

    add_executable(test_statistics tests/test_statistics.c)
    target_link_libraries(test_statistics PRIVATE thermal_simd_core_tests)
    target_compile_options(test_statistics PRIVATE -Wall -Wextra)
    add_test(NAME statistics COMMAND test_statistics)

    add_executable(test_thermal_simd
        tests/test_thermal_simd.c
        src/thermal_simd.c)
    target_compile_definitions(test_thermal_simd PRIVATE TSD_ENABLE_TESTS)
    target_link_libraries(test_thermal_simd PRIVATE thermal_simd_core_tests pthread)
    target_compile_options(test_thermal_simd PRIVATE -Wall -Wextra -O1 -pthread -fPIC)
    target_include_directories(test_thermal_simd PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME thermal_simd COMMAND test_thermal_simd)

    add_executable(test_policy_controller tests/policy/test_policy_controller.c)
    target_link_libraries(test_policy_controller PRIVATE thermal_simd_core_tests)
    target_compile_options(test_policy_controller PRIVATE -Wall -Wextra)
    add_test(NAME policy_controller COMMAND test_policy_controller)

    add_executable(test_healthcheck_runtime_flags tests/healthcheck/runtime_flags.cpp)
    target_link_libraries(test_healthcheck_runtime_flags PRIVATE thermal_simd_core_tests)
    target_compile_options(test_healthcheck_runtime_flags PRIVATE -Wall -Wextra)
    add_test(NAME healthcheck_runtime_flags COMMAND test_healthcheck_runtime_flags)

    add_executable(test_telemetry tests/telemetry/test_telemetry.cpp)
    target_link_libraries(test_telemetry PRIVATE thermal_simd_core_tests)
    target_compile_options(test_telemetry PRIVATE -Wall -Wextra)
    add_test(NAME telemetry_adapters COMMAND test_telemetry)

    add_executable(test_trampoline_security tests/patcher/test_trampoline_security.cpp)
    target_link_libraries(test_trampoline_security PRIVATE thermal_simd_core_tests pthread)
    target_compile_definitions(test_trampoline_security PRIVATE TSD_ENABLE_TESTS)
    target_compile_options(test_trampoline_security PRIVATE -Wall -Wextra -pthread)
    target_include_directories(test_trampoline_security PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME trampoline_security COMMAND test_trampoline_security)

    add_executable(test_integration_metrics_tuner tests/integration/metrics_tuner.cpp)
    target_link_libraries(test_integration_metrics_tuner PRIVATE tsd_policy_tuner pthread)
    target_compile_options(test_integration_metrics_tuner PRIVATE -Wall -Wextra)
    add_test(NAME integration_metrics_tuner COMMAND test_integration_metrics_tuner)

    add_library(tsd_stress_common STATIC
        tests/stress/stress_common.c
        src/thermal_simd.c)
    target_link_libraries(tsd_stress_common
        PUBLIC
            thermal_simd_core_tests
            pthread)
    target_compile_definitions(tsd_stress_common PUBLIC TSD_ENABLE_TESTS)
    target_include_directories(tsd_stress_common
        PUBLIC
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/stress
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_BINARY_DIR}/include)
    target_compile_options(tsd_stress_common PRIVATE -Wall -Wextra -pthread)

    add_executable(stress_patch_request tests/stress/patch_request_stress.c)
    target_link_libraries(stress_patch_request PRIVATE tsd_stress_common)
    target_compile_definitions(stress_patch_request PRIVATE TSD_ENABLE_TESTS)
    target_compile_options(stress_patch_request PRIVATE -Wall -Wextra -pthread)

    add_executable(stress_signal_storm tests/stress/signal_storm.c)
    target_link_libraries(stress_signal_storm PRIVATE tsd_stress_common pthread)
    target_compile_definitions(stress_signal_storm PRIVATE TSD_ENABLE_TESTS)
    target_compile_options(stress_signal_storm PRIVATE -Wall -Wextra -pthread)

    add_executable(stress_telemetry_faults tests/stress/telemetry_faults.c)
    target_link_libraries(stress_telemetry_faults PRIVATE tsd_stress_common pthread)
    target_compile_definitions(stress_telemetry_faults PRIVATE TSD_ENABLE_TESTS)
    target_compile_options(stress_telemetry_faults PRIVATE -Wall -Wextra -pthread)
endif()

install(TARGETS thermal_simd_core
        EXPORT thermal_simd_dispatcherTargets
        ARCHIVE DESTINATION lib
        LIBRARY DESTINATION lib
        RUNTIME DESTINATION bin)

install(DIRECTORY include/
        DESTINATION include
        FILES_MATCHING PATTERN "*.h"
        PATTERN "*.in" EXCLUDE)

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/include/thermal/simd/version.h
        DESTINATION include/thermal/simd)

install(EXPORT thermal_simd_dispatcherTargets
        NAMESPACE thermal::
        DESTINATION lib/cmake/thermal_simd_dispatcher)
