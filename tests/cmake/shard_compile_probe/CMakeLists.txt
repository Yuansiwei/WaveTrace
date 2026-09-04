cmake_minimum_required(VERSION 3.18)
project(WaveTraceShardCompileProbe LANGUAGES CXX)

foreach(_required WAVETRACE_ROOT SHARD_DIR)
    if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
        message(FATAL_ERROR "Missing required -D${_required}=...")
    endif()
endforeach()

file(GLOB _shard_sources CONFIGURE_DEPENDS
    "${SHARD_DIR}/root_class_closure_shard_*.cpp")
list(LENGTH _shard_sources _shard_count)
if(NOT _shard_count EQUAL 32)
    message(FATAL_ERROR "Expected 32 generated shard sources, found ${_shard_count}")
endif()

add_library(wavetrace_shard_compile_probe STATIC ${_shard_sources})
target_compile_features(wavetrace_shard_compile_probe PRIVATE cxx_std_14)
target_compile_definitions(wavetrace_shard_compile_probe PRIVATE
    WAVE_RUNTIME_AVAILABLE=1
    WAVETRACE_COMPILED_SHARDS=1)
target_include_directories(wavetrace_shard_compile_probe PRIVATE
    "${WAVETRACE_ROOT}"
    "${SHARD_DIR}")
if(MSVC)
    target_compile_options(wavetrace_shard_compile_probe PRIVATE /bigobj /MP32)
endif()

add_executable(wavetrace_shard_runtime_probe shard_runtime_probe.cpp)
target_compile_features(wavetrace_shard_runtime_probe PRIVATE cxx_std_14)
target_compile_definitions(wavetrace_shard_runtime_probe PRIVATE
    WAVE_RUNTIME_AVAILABLE=1
    WAVETRACE_COMPILED_SHARDS=1)
target_include_directories(wavetrace_shard_runtime_probe PRIVATE
    "${WAVETRACE_ROOT}"
    "${SHARD_DIR}")
target_link_libraries(wavetrace_shard_runtime_probe PRIVATE
    wavetrace_shard_compile_probe)
if(MSVC)
    target_compile_options(wavetrace_shard_runtime_probe PRIVATE /bigobj)
endif()
