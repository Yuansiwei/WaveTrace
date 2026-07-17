cmake_minimum_required(VERSION 3.18)
project(WaveTraceShardSignalMatrixProbe LANGUAGES CXX)

foreach(_required WAVETRACE_ROOT SHARD_DIR SHARD_COUNT)
    if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
        message(FATAL_ERROR "Missing required -D${_required}=...")
    endif()
endforeach()

file(TO_CMAKE_PATH "${SHARD_DIR}" _shard_dir)
file(TO_CMAKE_PATH "${WAVETRACE_ROOT}" _wavetrace_root)
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
        WAVE_RUNTIME_AVAILABLE=1
        WAVETRACE_COMPILED_SHARDS=1)
    target_include_directories(wavetrace_signal_matrix_shards PRIVATE
        "${_wavetrace_root}"
        "${_shard_dir}")
    if(MSVC)
        target_compile_options(wavetrace_signal_matrix_shards PRIVATE /bigobj /MP)
    endif()
elseif(NOT _actual_shard_count EQUAL 0)
    message(FATAL_ERROR "Unsharded mode unexpectedly found ${_actual_shard_count} shard sources")
endif()

add_executable(wavetrace_shard_signal_matrix_probe shard_signal_matrix_probe.cpp)
target_compile_features(wavetrace_shard_signal_matrix_probe PRIVATE cxx_std_14)
target_compile_definitions(wavetrace_shard_signal_matrix_probe PRIVATE
    WAVE_RUNTIME_AVAILABLE=1)
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
    target_compile_options(wavetrace_shard_signal_matrix_probe PRIVATE /bigobj)
endif()
