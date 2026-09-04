cmake_minimum_required(VERSION 3.15)
project(thermal_simd_dispatcher VERSION 0.4.0 LANGUAGES C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(THERMAL_SIMD_DISPATCHER_CPU_FLAGS "-msse4.1" CACHE STRING "CPU-specific compiler flags for thermal_simd")

include(CTest)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
option(TSD_BUILD_BENCHMARKS "Build deterministic software-overhead benchmarks" OFF)

find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

# The public library is a static archive containing both C and C++ objects.
# A consumer is allowed to be a pure-C CMake project, so its final link may be
# driven by the C compiler and would not otherwise receive the C++ ABI/runtime.
# Export the runtime selected by this build explicitly instead of relying on
# the consumer to know our implementation language. Linux is the only
# supported platform and libm is likewise part of the archive's public static
# link closure (for llround and related math helpers).
set(TSD_CXX_RUNTIME_LIBRARIES "")
foreach(_tsd_lib IN LISTS CMAKE_CXX_IMPLICIT_LINK_LIBRARIES)
    if(_tsd_lib MATCHES "^(stdc\\+\\+|c\\+\\+|c\\+\\+abi|supc\\+\\+)$")
        list(APPEND TSD_CXX_RUNTIME_LIBRARIES "${_tsd_lib}")
    endif()
endforeach()
list(REMOVE_DUPLICATES TSD_CXX_RUNTIME_LIBRARIES)
if(NOT TSD_CXX_RUNTIME_LIBRARIES)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        set(TSD_CXX_RUNTIME_LIBRARIES stdc++)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        # Ubuntu/Clang uses libstdc++ unless configured otherwise; the implicit
        # library scan above wins automatically for libc++ toolchains.
        set(TSD_CXX_RUNTIME_LIBRARIES stdc++)
    else()
        message(FATAL_ERROR "Unable to determine C++ runtime for static thermal_simd_core consumers")
    endif()
endif()

file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/include/thermal/simd)
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/include/thermal/simd/version.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/include/thermal/simd/version.h
    @ONLY
)

# The lifecycle and trampoline implementation files are compiled under private
# names. Small public wrappers then enforce handle lifetime and live safety
# authorization without duplicating the monitor or executable-memory engines.
set_source_files_properties(src/thermal_simd.c PROPERTIES COMPILE_DEFINITIONS TSD_RUNTIME_INTERNAL_IMPL)
set_source_files_properties(src/patcher/trampoline.cpp PROPERTIES COMPILE_DEFINITIONS TSD_TRAMPOLINE_INTERNAL_IMPL)

# Sources shared with the white-box test archive. The production core adds the
# runtime/orchestration implementation and public handle wrapper below; tests
# compile the implementation separately with TSD_ENABLE_TESTS so test-only
# hooks never leak into the installed library.
set(THERMAL_SIMD_CORE_SOURCES
    src/config/runtime_flags.c
    src/dispatch/adaptive_dispatch.c
    src/runtime_guard.c
    src/workload_counter.c
    src/logging.c
    src/config_parser.c
    src/third_party/jsmn.c
    src/statistics.c
    src/runtime_metrics.c
    src/health_check.c
    src/healthcheck/sandbox.cpp
    src/telemetry_helper.c
    src/telemetry/evaluator.cpp
    src/telemetry/history_store.cpp
    src/telemetry/sensors.cpp
    src/telemetry/bus.cpp
    src/telemetry/collector.cpp
    src/telemetry/fusion.cpp
    src/telemetry/fusion_bridge.cpp
    src/observability/metrics.cpp
    src/observability/statsd_exporter.cpp
    src/observability/telemetry_state.cpp
    src/thermal_config.c
    src/thermal_cpu.c
    src/patcher/trampoline.cpp
    src/patcher/trampoline_state.c
    src/patcher/trampoline_guard.c
    src/thermal_perf.c
    src/thermal_signals.c
    src/policy/policy_config.c
    src/policy/dispatcher_policy.cpp
    src/policy/arx_model.cpp
    src/policy/mpc_controller.cpp
)

add_library(thermal_simd_core STATIC ${THERMAL_SIMD_CORE_SOURCES} src/runtime_api.c src/thermal_simd.c)
set_target_properties(thermal_simd_core PROPERTIES EXPORT_NAME simd)
target_include_directories(thermal_simd_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_definitions(thermal_simd_core
    PRIVATE
        TSD_DEFAULT_COEFF_PATH="${CMAKE_CURRENT_SOURCE_DIR}/config/controller_coeffs.json"
        TSD_INSTALLED_COEFF_PATH="${CMAKE_INSTALL_FULL_SYSCONFDIR}/thermal-simd/controller_coeffs.json"
)
# SSE4.1 is the documented minimum ISA. Keep ordinary compiler-generated core
# code out of AVX so the dispatch layer alone controls wider-vector execution.
target_compile_options(thermal_simd_core PRIVATE -fPIC -mno-avx ${THERMAL_SIMD_DISPATCHER_CPU_FLAGS})
target_link_libraries(thermal_simd_core
    PUBLIC
        Threads::Threads
        OpenSSL::SSL
        OpenSSL::Crypto
        m
        ${TSD_CXX_RUNTIME_LIBRARIES}
)

add_executable(thermal_simd src/main.cpp)
target_compile_options(thermal_simd PRIVATE -O2 -pthread -fPIC -mno-avx ${THERMAL_SIMD_DISPATCHER_CPU_FLAGS})
target_link_libraries(thermal_simd PRIVATE thermal_simd_core Threads::Threads)

add_library(tsd_policy_tuner STATIC src/tools/policy_tuner/tuner.cpp)
target_include_directories(tsd_policy_tuner
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(tsd_policy_tuner PUBLIC thermal_simd_core)

add_executable(policy_tuner src/tools/policy_tuner/main.cpp)
target_link_libraries(policy_tuner PRIVATE tsd_policy_tuner)

if(TSD_BUILD_BENCHMARKS)
    add_executable(benchmark_dispatch benchmarks/dispatch_overhead.c)
    target_link_libraries(benchmark_dispatch PRIVATE thermal_simd_core)
    target_compile_options(benchmark_dispatch PRIVATE -O3 -Wall -Wextra)

    add_executable(benchmark_registered_kernel benchmarks/registered_kernel.c)
    target_link_libraries(benchmark_registered_kernel PRIVATE thermal_simd_core Threads::Threads)
    target_compile_options(benchmark_registered_kernel
        PRIVATE -O3 -Wall -Wextra -Wpedantic -Werror=return-type -mno-avx -msse4.1)
endif()

if(BUILD_TESTING)
    # CMake's Release flags define NDEBUG, which would otherwise compile away
    # assert()-based regression checks. Tests must validate the same invariants
    # in every configuration, including the normal Release CI job.
    add_compile_options(-UNDEBUG)

    add_test(NAME hil_common_helpers
             COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/hil/test_hil_common.sh)

    add_library(thermal_simd_core_tests STATIC ${THERMAL_SIMD_CORE_SOURCES})
    target_include_directories(thermal_simd_core_tests
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_compile_definitions(thermal_simd_core_tests PRIVATE TSD_ENABLE_TESTS)
    target_compile_definitions(thermal_simd_core_tests
        PRIVATE
            TSD_DEFAULT_COEFF_PATH="${CMAKE_CURRENT_SOURCE_DIR}/config/controller_coeffs.json"
            TSD_INSTALLED_COEFF_PATH="${CMAKE_INSTALL_FULL_SYSCONFDIR}/thermal-simd/controller_coeffs.json"
    )
    target_link_libraries(thermal_simd_core_tests PUBLIC OpenSSL::SSL OpenSSL::Crypto Threads::Threads)

    add_executable(test_config_parser tests/test_config_parser.c)
    target_link_libraries(test_config_parser PRIVATE thermal_simd_core_tests)
    target_compile_options(test_config_parser PRIVATE -Wall -Wextra)
    add_test(NAME config_parser COMMAND test_config_parser)

    add_executable(test_statistics tests/test_statistics.c)
    target_link_libraries(test_statistics PRIVATE thermal_simd_core_tests)
    target_compile_options(test_statistics PRIVATE -Wall -Wextra)
    add_test(NAME statistics COMMAND test_statistics)

    add_executable(test_logging_metrics tests/test_logging_metrics.c)
    target_link_libraries(test_logging_metrics PRIVATE thermal_simd_core_tests Threads::Threads)
    target_compile_options(test_logging_metrics PRIVATE -Wall -Wextra -pthread)
    add_test(NAME logging_metrics COMMAND test_logging_metrics)

    add_executable(test_runtime_config_cli tests/config/test_runtime_config_cli.c)
    target_link_libraries(test_runtime_config_cli PRIVATE thermal_simd_core_tests m)
    target_compile_options(test_runtime_config_cli PRIVATE -Wall -Wextra)
    add_test(NAME runtime_config_cli COMMAND test_runtime_config_cli)

    add_executable(test_runtime_lifecycle tests/runtime/test_runtime_lifecycle.c)
    target_link_libraries(test_runtime_lifecycle PRIVATE thermal_simd_core Threads::Threads)
    target_compile_options(test_runtime_lifecycle PRIVATE -Wall -Wextra -pthread)
    target_include_directories(test_runtime_lifecycle PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME runtime_lifecycle COMMAND test_runtime_lifecycle)

    # Deterministic shutdown-failure injection belongs in an isolated white-box
    # target so the production lifecycle suite continues exercising the exact
    # installed runtime/trampoline semantics.
    add_executable(test_runtime_stop_faults
        tests/runtime/test_runtime_stop_faults.c
        src/runtime_api.c
        src/thermal_simd.c)
    target_link_libraries(test_runtime_stop_faults PRIVATE thermal_simd_core_tests Threads::Threads m ${TSD_CXX_RUNTIME_LIBRARIES})
    target_compile_definitions(test_runtime_stop_faults PRIVATE TSD_ENABLE_TESTS)
    target_compile_options(test_runtime_stop_faults PRIVATE -Wall -Wextra -pthread -fPIC -mno-avx ${THERMAL_SIMD_DISPATCHER_CPU_FLAGS})
    target_include_directories(test_runtime_stop_faults PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    set_target_properties(test_runtime_stop_faults PROPERTIES LINKER_LANGUAGE CXX)
    add_test(NAME runtime_stop_faults COMMAND test_runtime_stop_faults)

    add_executable(test_thermal_simd tests/test_thermal_simd.c src/thermal_simd.c)
    target_compile_definitions(test_thermal_simd PRIVATE TSD_ENABLE_TESTS)
    target_link_libraries(test_thermal_simd PRIVATE thermal_simd_core_tests Threads::Threads)
    target_compile_options(test_thermal_simd PRIVATE -Wall -Wextra -O1 -pthread -fPIC)
    target_include_directories(test_thermal_simd PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME thermal_simd COMMAND test_thermal_simd)

    add_executable(test_perf_resilience tests/perf/test_perf_resilience.c)
    target_link_libraries(test_perf_resilience PRIVATE thermal_simd_core_tests Threads::Threads)
    target_compile_definitions(test_perf_resilience PRIVATE TSD_ENABLE_TESTS)
    target_compile_options(test_perf_resilience PRIVATE -Wall -Wextra -pthread)
    add_test(NAME perf_resilience COMMAND test_perf_resilience)

    add_executable(test_adaptive_dispatch tests/dispatch/test_adaptive_dispatch.c)
    target_link_libraries(test_adaptive_dispatch PRIVATE thermal_simd_core_tests Threads::Threads)
    target_compile_options(test_adaptive_dispatch PRIVATE -Wall -Wextra)
    add_test(NAME adaptive_dispatch COMMAND test_adaptive_dispatch)

    add_executable(test_policy_controller tests/policy/test_policy_controller.c)
    target_link_libraries(test_policy_controller PRIVATE thermal_simd_core_tests)
    target_compile_options(test_policy_controller PRIVATE -Wall -Wextra)
    add_test(NAME policy_controller COMMAND test_policy_controller)

    add_executable(test_arx_model tests/policy/test_arx_model.cpp)
    target_link_libraries(test_arx_model PRIVATE thermal_simd_core_tests)
    target_compile_options(test_arx_model PRIVATE -Wall -Wextra)
    target_include_directories(test_arx_model PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_definitions(test_arx_model PRIVATE TSD_ENABLE_TESTS)
    add_test(NAME policy_arx_model COMMAND test_arx_model)

    add_executable(test_healthcheck_runtime_flags tests/healthcheck/runtime_flags.cpp)
    target_link_libraries(test_healthcheck_runtime_flags PRIVATE thermal_simd_core_tests)
    target_compile_options(test_healthcheck_runtime_flags PRIVATE -Wall -Wextra)
    add_test(NAME healthcheck_runtime_flags COMMAND test_healthcheck_runtime_flags)

    add_executable(test_telemetry tests/telemetry/test_telemetry.cpp)
    target_link_libraries(test_telemetry PRIVATE thermal_simd_core_tests)
    target_compile_options(test_telemetry PRIVATE -Wall -Wextra)
    add_test(NAME telemetry_adapters COMMAND test_telemetry)

    add_executable(test_telemetry_fusion tests/telemetry/test_fusion_thread.cpp)
    target_link_libraries(test_telemetry_fusion PRIVATE thermal_simd_core_tests Threads::Threads)
    target_compile_definitions(test_telemetry_fusion PRIVATE TSD_ENABLE_TESTS)
    target_compile_options(test_telemetry_fusion PRIVATE -Wall -Wextra -pthread)
    add_test(NAME telemetry_fusion_thread COMMAND test_telemetry_fusion)

    add_executable(test_telemetry_fusion_stress tests/telemetry/test_fusion_stress.cpp)
    target_link_libraries(test_telemetry_fusion_stress PRIVATE thermal_simd_core_tests Threads::Threads)
    target_compile_options(test_telemetry_fusion_stress PRIVATE -Wall -Wextra -pthread)
    add_test(NAME telemetry_fusion_stress COMMAND test_telemetry_fusion_stress)

    add_executable(test_trampoline_security tests/patcher/test_trampoline_security.cpp)
    target_link_libraries(test_trampoline_security PRIVATE thermal_simd_core_tests Threads::Threads)
    target_compile_definitions(test_trampoline_security PRIVATE TSD_ENABLE_TESTS)
    target_compile_options(test_trampoline_security PRIVATE -Wall -Wextra -pthread)
    target_include_directories(test_trampoline_security PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME trampoline_security COMMAND test_trampoline_security)

    add_executable(test_integration_metrics_tuner tests/integration/metrics_tuner.cpp)
    target_link_libraries(test_integration_metrics_tuner PRIVATE tsd_policy_tuner Threads::Threads)
    target_compile_options(test_integration_metrics_tuner PRIVATE -Wall -Wextra)
    add_test(NAME integration_metrics_tuner COMMAND test_integration_metrics_tuner)

    add_executable(test_observability_metrics tests/observability/test_metrics_exporter.cpp)
    target_link_libraries(test_observability_metrics PRIVATE thermal_simd_core_tests Threads::Threads OpenSSL::SSL OpenSSL::Crypto)
    target_compile_options(test_observability_metrics PRIVATE -Wall -Wextra -pthread)
    add_test(NAME observability_metrics COMMAND test_observability_metrics)

    add_library(tsd_stress_common STATIC tests/stress/stress_common.c src/thermal_simd.c)
    target_link_libraries(tsd_stress_common PUBLIC thermal_simd_core_tests Threads::Threads)
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
    add_test(NAME stress_patch_request_smoke COMMAND stress_patch_request --threads=2 --iterations=250)

    add_executable(stress_signal_storm tests/stress/signal_storm.c)
    target_link_libraries(stress_signal_storm PRIVATE tsd_stress_common Threads::Threads)
    target_compile_definitions(stress_signal_storm PRIVATE TSD_ENABLE_TESTS)
    target_compile_options(stress_signal_storm PRIVATE -Wall -Wextra -pthread)
    add_test(NAME stress_signal_storm_smoke COMMAND stress_signal_storm --duration-seconds=2 --signal-rate=50)

    add_executable(stress_telemetry_faults tests/stress/telemetry_faults.c)
    target_link_libraries(stress_telemetry_faults PRIVATE tsd_stress_common Threads::Threads)
    target_compile_definitions(stress_telemetry_faults PRIVATE TSD_ENABLE_TESTS)
    target_compile_options(stress_telemetry_faults PRIVATE -Wall -Wextra -pthread)
    add_test(NAME stress_telemetry_faults_smoke COMMAND stress_telemetry_faults --cycles=2)
endif()

set(TSD_CMAKE_INSTALL_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/thermal_simd_dispatcher")
configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/thermal_simd_dispatcherConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/thermal_simd_dispatcherConfig.cmake
    INSTALL_DESTINATION ${TSD_CMAKE_INSTALL_DIR}
)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/thermal_simd_dispatcherConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(TARGETS thermal_simd_core thermal_simd
        EXPORT thermal_simd_dispatcherTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

install(DIRECTORY include/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        FILES_MATCHING PATTERN "*.h"
        PATTERN "*.in" EXCLUDE)

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/include/thermal/simd/version.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/thermal/simd)

install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/config/controller_coeffs.json
        DESTINATION ${CMAKE_INSTALL_SYSCONFDIR}/thermal-simd)

install(FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/LICENSE
        ${CMAKE_CURRENT_SOURCE_DIR}/NOTICE
        ${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY_NOTICES.md
        DESTINATION ${CMAKE_INSTALL_DOCDIR})

install(EXPORT thermal_simd_dispatcherTargets
        NAMESPACE thermal::
        DESTINATION ${TSD_CMAKE_INSTALL_DIR})
install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/thermal_simd_dispatcherConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/thermal_simd_dispatcherConfigVersion.cmake
        DESTINATION ${TSD_CMAKE_INSTALL_DIR})
