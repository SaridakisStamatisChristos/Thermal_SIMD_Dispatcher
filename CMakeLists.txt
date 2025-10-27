cmake_minimum_required(VERSION 3.15)
project(thermal_simd_dispatcher C)
set(CMAKE_C_STANDARD 11)

set(THERMAL_SIMD_DISPATCHER_CPU_FLAGS "-msse4.1" CACHE STRING "CPU-specific compiler flags for thermal_simd")

add_executable(thermal_simd src/thermal_simd.c)
target_compile_options(thermal_simd PRIVATE -O2 -pthread -fPIC -mno-avx ${THERMAL_SIMD_DISPATCHER_CPU_FLAGS})
target_link_libraries(thermal_simd pthread)
