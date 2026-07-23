cmake_minimum_required(VERSION 3.18)
project(WaveTraceShardSignalMatrixProbe LANGUAGES CXX)

set(GCD_MODELING_MODE "2" CACHE STRING "gcdMODELING_MODE used by the shard probe")

foreach(_required WAVETRACE_ROOT SHARD_DIR SHARD_COUNT WAVETRACE_RUNTIME_CONFIG)
    if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
        message(FATAL_ERROR "Missing required -D${_required}=...")
    endif()
endforeach()

file(TO_CMAKE_PATH "${SHARD_DIR}" _shard_dir)
file(TO_CMAKE_PATH "${WAVETRACE_ROOT}" _wavetrace_root)
file(TO_CMAKE_PATH "${WAVETRACE_RUNTIME_CONFIG}" _wavetrace_runtime_config)
file(GLOB _shard_sources CONFIGURE_DEPENDS
    "${_shard_dir}/root_class_closure_shard_*.cpp")
list(LENGTH _shard_sources _actual_shard_count)

if(SHARD_COUNT GREATER 0)
    if(NOT _actual_shard_count EQUAL SHARD_COUNT)
        message(FATAL_ERROR
            "Expected ${SHARD_COUNT} generated shard sources, found ${_actual_shard_count}")
    endif()

    add_library(wavetrace_signal_matrix_shards STATIC ${_shard_sources})
    target_compile_features(wavetrace_signal_matrix_shards PRIVATE cxx_std_14)
    target_compile_definitions(wavetrace_signal_matrix_shards PRIVATE
        gcdMODELING_MODE=${GCD_MODELING_MODE}
        WAVE_RUNTIME_AVAILABLE=1
        WAVETRACE_COMPILED_SHARDS=1)
    target_include_directories(wavetrace_signal_matrix_shards PRIVATE
        "${_wavetrace_root}"
        "${_shard_dir}")
    if(MSVC)
        target_compile_options(wavetrace_signal_matrix_shards PRIVATE /bigobj /MP8)
    endif()
elseif(NOT _actual_shard_count EQUAL 0)
    message(FATAL_ERROR "Unsharded mode unexpectedly found ${_actual_shard_count} shard sources")
endif()

add_executable(wavetrace_shard_signal_matrix_probe shard_signal_matrix_probe.cpp)
set_source_files_properties("${WAVETRACE_RUNTIME_CONFIG}" PROPERTIES HEADER_FILE_ONLY TRUE)
target_sources(wavetrace_shard_signal_matrix_probe PRIVATE "${WAVETRACE_RUNTIME_CONFIG}")
source_group("configs" FILES "${WAVETRACE_RUNTIME_CONFIG}")
target_compile_features(wavetrace_shard_signal_matrix_probe PRIVATE cxx_std_14)
target_compile_definitions(wavetrace_shard_signal_matrix_probe PRIVATE
    gcdMODELING_MODE=${GCD_MODELING_MODE}
    WAVE_RUNTIME_AVAILABLE=1
    "WAVETRACE_CONFIG_PATH=\"${_wavetrace_runtime_config}\"")
target_include_directories(wavetrace_shard_signal_matrix_probe PRIVATE
    "${_wavetrace_root}"
    "${_shard_dir}")
if(SHARD_COUNT GREATER 0)
    target_compile_definitions(wavetrace_shard_signal_matrix_probe PRIVATE
        WAVETRACE_COMPILED_SHARDS=1)
    target_link_libraries(wavetrace_shard_signal_matrix_probe PRIVATE
        wavetrace_signal_matrix_shards)
endif()
if(MSVC)
    target_compile_options(wavetrace_shard_signal_matrix_probe PRIVATE /bigobj /MP8)
endif()
