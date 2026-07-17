cmake_minimum_required(VERSION 3.18)

foreach(_required REFLECTGEN_EXE RUNNER_SCRIPT TEST_ROOT)
    if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
        message(FATAL_ERROR "Missing required -D${_required}=...")
    endif()
endforeach()

get_filename_component(TEST_ROOT "${TEST_ROOT}" ABSOLUTE)
set(_output_dir "${TEST_ROOT}/generated_reflect")
set(_config "${TEST_ROOT}/wavetrace_config.json")
set(_settings "${TEST_ROOT}/wavetrace_reflection.settings")
set(_log "${_output_dir}/reflectgen.log")

file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${TEST_ROOT}")
file(WRITE "${_config}"
    "{\n  \"WaveTrace\": false,\n  \"WaveTraceFileName\": \"wave.wvz4\",\n  \"WaveTraceStart\": \"\",\n  \"WaveTraceEnd\": \"\",\n  \"wave_ptr_members\": []\n}\n")

function(_run_reflectgen _shards)
    execute_process(
        COMMAND "${CMAKE_COMMAND}"
            "-DWAVETRACE_REFLECTGEN_EXE=${REFLECTGEN_EXE}"
            "-DWAVETRACE_REFLECT_BATCH_DIR=${TEST_ROOT}"
            "-DWAVETRACE_REFLECT_ROOT_CLASS=IncrementalShardTestRoot"
            "-DWAVETRACE_REFLECT_COMPILE_SHARDS=${_shards}"
            "-DWAVETRACE_REFLECT_OUTPUT_DIR=${_output_dir}"
            "-DWAVETRACE_REFLECT_AGGREGATE_HEADER=project_reflect_auto.h"
            "-DWAVETRACE_REFLECT_LOG_FILE=${_log}"
            "-DWAVETRACE_CONFIG_FILE=${_config}"
            "-DWAVETRACE_REFLECT_SETTINGS_FILE=${_settings}"
            -P "${RUNNER_SCRIPT}"
        RESULT_VARIABLE _result
        OUTPUT_VARIABLE _stdout
        ERROR_VARIABLE _stderr)
    if(NOT _result EQUAL 0)
        message(FATAL_ERROR
            "run_reflectgen.cmake failed for ${_shards} shards\nstdout:\n${_stdout}\nstderr:\n${_stderr}")
    endif()
endfunction()

function(_require_file _path)
    if(NOT EXISTS "${_path}")
        message(FATAL_ERROR "Expected generated file is missing: ${_path}")
    endif()
endfunction()

# Establish a fresh aggregate produced by the old, unsharded shape.
file(WRITE "${_settings}" "compile_shards=0\n")
_run_reflectgen(0)
_require_file("${_output_dir}/project_reflect_auto.h")

# The aggregate is already fresh here. Missing byproducts must nevertheless
# force generation of all 32 shard translation units and headers.
file(WRITE "${_settings}" "compile_shards=32\n")
_run_reflectgen(32)
foreach(_index RANGE 0 31)
    if(_index LESS 10)
        set(_tag "0${_index}")
    else()
        set(_tag "${_index}")
    endif()
    set(_base "${_output_dir}/root_class_closure_shard_${_tag}")
    _require_file("${_base}.cpp")
    _require_file("${_base}_input.h")
    _require_file("${_base}_reflect_auto.h")
endforeach()
_require_file("${_output_dir}/root_class_closure_root_input.h")
_require_file("${_output_dir}/root_class_closure_shards_registry.h")

# Losing one byproduct must repair it even when every tracked input and the
# aggregate header are otherwise unchanged.
file(REMOVE "${_output_dir}/root_class_closure_shard_17.cpp")
_run_reflectgen(32)
_require_file("${_output_dir}/root_class_closure_shard_17.cpp")

# A settings-only 32 -> 16 change must regenerate the registry rather than
# accepting the still-fresh 32-shard aggregate.
execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 1)
file(WRITE "${_settings}" "compile_shards=16\n")
_run_reflectgen(16)
file(READ "${_output_dir}/root_class_closure_shards_registry.h" _registry)
if(NOT _registry MATCHES "wavetrace_register_reflection_shard_15")
    message(FATAL_ERROR "16-shard registry does not contain shard 15")
endif()
if(_registry MATCHES "wavetrace_register_reflection_shard_16")
    message(FATAL_ERROR "16-shard registry still contains stale shard 16")
endif()

# Exercise the real AST/reflection path as well as the disabled placeholder
# path above.  Forty independent dependency types ensure every one of the 32
# requested shards receives generated reflection code.
set(_enabled_root "${TEST_ROOT}_enabled")
set(_enabled_output "${_enabled_root}/generated_reflect")
set(_enabled_header "${_enabled_root}/input.h")
set(_enabled_preamble_header "${_enabled_root}/preamble.h")
set(_enabled_records_header "${_enabled_root}/records.h")
set(_enabled_targets "${_enabled_root}/targets.txt")
set(_enabled_config "${_enabled_root}/wavetrace_config.json")
set(_enabled_settings "${_enabled_root}/wavetrace_reflection.settings")
file(REMOVE_RECURSE "${_enabled_root}")
file(MAKE_DIRECTORY "${_enabled_root}")
file(WRITE "${_enabled_preamble_header}"
    "#pragma once\nusing ShardDependencyScalar = int;\n")
set(_enabled_records_source "#pragma once\n")
foreach(_index RANGE 0 39)
    if(_index LESS 10)
        set(_tag "0${_index}")
    else()
        set(_tag "${_index}")
    endif()
    string(APPEND _enabled_records_source
        "struct Dependency${_tag} { ShardDependencyScalar value; };\n")
endforeach()
file(WRITE "${_enabled_records_header}" "${_enabled_records_source}")
set(_enabled_source
    "#include \"preamble.h\"\n#include \"records.h\"\n\n")
string(APPEND _enabled_source "struct IncrementalShardTestRoot {\n")
foreach(_index RANGE 0 39)
    if(_index LESS 10)
        set(_tag "0${_index}")
    else()
        set(_tag "${_index}")
    endif()
    string(APPEND _enabled_source "    Dependency${_tag} dependency_${_tag};\n")
endforeach()
string(APPEND _enabled_source "};\n")
file(WRITE "${_enabled_header}" "${_enabled_source}")
file(TO_CMAKE_PATH "${_enabled_header}" _enabled_header_cmake)
file(WRITE "${_enabled_targets}" "\"${_enabled_header_cmake}\"\n")
file(WRITE "${_enabled_config}"
    "{\n  \"WaveTrace\": true,\n  \"WaveTraceFileName\": \"wave.wvz4\",\n  \"WaveTraceStart\": \"\",\n  \"WaveTraceEnd\": \"\",\n  \"wave_ptr_members\": []\n}\n")
file(WRITE "${_enabled_settings}" "compile_shards=32\n")
execute_process(
    COMMAND "${CMAKE_COMMAND}"
        "-DWAVETRACE_REFLECTGEN_EXE=${REFLECTGEN_EXE}"
        "-DWAVETRACE_REFLECT_TARGET_LIST=${_enabled_targets}"
        "-DWAVETRACE_REFLECT_ROOT_CLASS=IncrementalShardTestRoot"
        "-DWAVETRACE_REFLECT_COMPILE_SHARDS=32"
        "-DWAVETRACE_REFLECT_OUTPUT_DIR=${_enabled_output}"
        "-DWAVETRACE_REFLECT_AGGREGATE_HEADER=project_reflect_auto.h"
        "-DWAVETRACE_REFLECT_LOG_FILE=${_enabled_output}/reflectgen.log"
        "-DWAVETRACE_CONFIG_FILE=${_enabled_config}"
        "-DWAVETRACE_REFLECT_SETTINGS_FILE=${_enabled_settings}"
        -P "${RUNNER_SCRIPT}"
    RESULT_VARIABLE _enabled_result
    OUTPUT_VARIABLE _enabled_stdout
    ERROR_VARIABLE _enabled_stderr)
if(NOT _enabled_result EQUAL 0)
    message(FATAL_ERROR
        "Enabled 32-shard ReflectGen run failed\nstdout:\n${_enabled_stdout}\nstderr:\n${_enabled_stderr}")
endif()
foreach(_index RANGE 0 31)
    if(_index LESS 10)
        set(_tag "0${_index}")
    else()
        set(_tag "${_index}")
    endif()
    set(_shard_cpp "${_enabled_output}/root_class_closure_shard_${_tag}.cpp")
    set(_shard_input "${_enabled_output}/root_class_closure_shard_${_tag}_input.h")
    _require_file("${_shard_cpp}")
    _require_file("${_shard_input}")
    file(READ "${_shard_cpp}" _shard_cpp_text)
    if(NOT _shard_cpp_text MATCHES "#include \"wave_runtime.h\"" OR
            NOT _shard_cpp_text MATCHES "#include \"root_class_closure_shard_${_tag}_reflect_auto.h\"")
        message(FATAL_ERROR "Enabled shard ${_tag} contains no generated reflection body")
    endif()
    file(READ "${_shard_input}" _shard_input_text)
    if(NOT _shard_input_text MATCHES "__reflectgen_batch_all_headers__\\.h")
        message(FATAL_ERROR "Enabled shard ${_tag} does not reuse the parsed umbrella input")
    endif()
endforeach()

message(STATUS "run_reflectgen incremental shard regression test passed")
