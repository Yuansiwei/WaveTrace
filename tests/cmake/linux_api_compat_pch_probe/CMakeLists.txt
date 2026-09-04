cmake_minimum_required(VERSION 3.18)
project(WaveTraceLinuxApiCompatPchProbe LANGUAGES CXX)

if(NOT DEFINED WAVETRACE_ROOT)
    message(FATAL_ERROR "WAVETRACE_ROOT is required")
endif()

add_library(cmodel STATIC cmodel.cpp)
target_compile_features(cmodel PUBLIC cxx_std_14)
target_include_directories(cmodel PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${WAVETRACE_ROOT}")
target_precompile_headers(cmodel PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/vsiPrecomp.h")

add_executable(pch_consumer consumer.cpp)
target_compile_features(pch_consumer PRIVATE cxx_std_14)
target_link_libraries(pch_consumer PRIVATE cmodel)

add_executable(standard_header_consumer standard_header_consumer.cpp)
target_compile_features(standard_header_consumer PRIVATE cxx_std_14)
target_precompile_headers(standard_header_consumer PRIVATE
    <cmath>
    <cctype>
    <cstdint>
    <cstdlib>)
target_link_libraries(standard_header_consumer PRIVATE cmodel)
