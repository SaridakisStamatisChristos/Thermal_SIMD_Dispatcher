cmake_minimum_required(VERSION 3.15)
project(thermal_simd_dispatcher C)
set(CMAKE_C_STANDARD 11)

set(THERMAL_SIMD_DISPATCHER_CPU_FLAGS "-msse4.1" CACHE STRING "CPU-specific compiler flags for thermal_simd")

add_library(thermal_simd_core STATIC
    src/config_parser.c
    src/statistics.c
    src/thermal_config.c
    src/thermal_cpu.c
    src/thermal_trampoline.c
    src/thermal_perf.c
    src/thermal_signals.c
)
target_include_directories(thermal_simd_core PUBLIC src)

add_executable(thermal_simd src/thermal_simd.c)
target_compile_options(thermal_simd PRIVATE -O2 -pthread -fPIC -mno-avx ${THERMAL_SIMD_DISPATCHER_CPU_FLAGS})
target_link_libraries(thermal_simd PRIVATE thermal_simd_core pthread)

enable_testing()

add_executable(test_config_parser tests/test_config_parser.c)
target_link_libraries(test_config_parser PRIVATE thermal_simd_core)
target_compile_options(test_config_parser PRIVATE -Wall -Wextra)
add_test(NAME config_parser COMMAND test_config_parser)

add_executable(test_statistics tests/test_statistics.c)
target_link_libraries(test_statistics PRIVATE thermal_simd_core)
target_compile_options(test_statistics PRIVATE -Wall -Wextra)
add_test(NAME statistics COMMAND test_statistics)

add_executable(test_thermal_simd
    tests/test_thermal_simd.c
    src/thermal_simd.c)
target_compile_definitions(test_thermal_simd PRIVATE TSD_ENABLE_TESTS)
target_link_libraries(test_thermal_simd PRIVATE thermal_simd_core pthread)
target_compile_options(test_thermal_simd PRIVATE -Wall -Wextra -O1 -pthread -fPIC)
add_test(NAME thermal_simd COMMAND test_thermal_simd)
