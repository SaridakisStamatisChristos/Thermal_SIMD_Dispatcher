cmake_minimum_required(VERSION 3.15)
project(thermal_simd_consumer LANGUAGES C)

find_package(thermal_simd_dispatcher 0.3 CONFIG REQUIRED)

add_executable(thermal_simd_consumer main.c)
target_link_libraries(thermal_simd_consumer PRIVATE thermal::simd)
