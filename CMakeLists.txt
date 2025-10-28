cmake_minimum_required(VERSION 3.15)
project(thermal_simd_dispatcher VERSION 0.1.0 LANGUAGES C)
set(CMAKE_C_STANDARD 11)

set(THERMAL_SIMD_DISPATCHER_CPU_FLAGS "-msse4.1" CACHE STRING "CPU-specific compiler flags for thermal_simd")

include(CTest)

file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/include/thermal/simd)
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/include/thermal/simd/version.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/include/thermal/simd/version.h
    @ONLY
)

set(THERMAL_SIMD_CORE_SOURCES
    src/logging.c
    src/config_parser.c
    src/statistics.c
    src/runtime_metrics.c
    src/health_check.c
    src/telemetry_helper.c
    src/thermal_config.c
    src/thermal_cpu.c
    src/thermal_trampoline.c
    src/thermal_perf.c
    src/thermal_signals.c
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

add_executable(thermal_simd src/thermal_simd.c)
target_compile_options(thermal_simd PRIVATE -O2 -pthread -fPIC -mno-avx ${THERMAL_SIMD_DISPATCHER_CPU_FLAGS})
target_link_libraries(thermal_simd PRIVATE thermal_simd_core pthread)

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
