cmake_minimum_required(VERSION 3.18)

foreach(_required REFLECTGEN_EXE RUNNER_SCRIPT TEST_ROOT)
    if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
        message(FATAL_ERROR "Missing required -D${_required}=...")
    endif()
endforeach()

get_filename_component(TEST_ROOT "${TEST_ROOT}" ABSOLUTE)
set(_output "${TEST_ROOT}/generated_reflect")
set(_input "${TEST_ROOT}/input.h")
set(_targets "${TEST_ROOT}/targets.txt")
set(_config "${TEST_ROOT}/wavetrace_config.json")
set(_state "${TEST_ROOT}/wavetrace_reflect_config.state")
set(_stamp "${TEST_ROOT}/wavetrace_reflect_build.stamp")
set(_aggregate "${_output}/project_reflect_auto.h")

file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${TEST_ROOT}")
file(WRITE "${_input}" "#pragma once\nstruct ConfigTransitionRoot { int value; };\n")
file(TO_CMAKE_PATH "${_input}" _input_cmake)
file(WRITE "${_targets}" "\"${_input_cmake}\"\n")

function(_write_config _enabled _name)
    file(WRITE "${_config}"
        "{\n  \"WaveTrace\": ${_enabled},\n  \"WaveTraceFileName\": \"${_name}\",\n  \"WaveTraceStart\": \"\",\n  \"WaveTraceEnd\": \"\",\n  \"WaveTraceLevel\": \"\",\n  \"wave_ptr_members\": []\n}\n")
endfunction()

function(_run)
    execute_process(
        COMMAND "${CMAKE_COMMAND}"
            "-DWAVETRACE_REFLECTGEN_EXE=${REFLECTGEN_EXE}"
            "-DWAVETRACE_REFLECT_TARGET_LIST=${_targets}"
            "-DWAVETRACE_REFLECT_ROOT_CLASS=ConfigTransitionRoot"
            "-DWAVETRACE_REFLECT_COMPILE_SHARDS=4"
            "-DWAVETRACE_REFLECT_OUTPUT_DIR=${_output}"
            "-DWAVETRACE_REFLECT_AGGREGATE_HEADER=project_reflect_auto.h"
            "-DWAVETRACE_REFLECT_LOG_FILE=${_output}/reflectgen.log"
            "-DWAVETRACE_CONFIG_FILE=${_config}"
            "-DWAVETRACE_REFLECT_BUILD_STAMP=${_stamp}"
            "-DWAVETRACE_REFLECT_CONFIG_STATE_FILE=${_state}"
            -P "${RUNNER_SCRIPT}"
        RESULT_VARIABLE _result
        OUTPUT_VARIABLE _stdout
        ERROR_VARIABLE _stderr)
    if(NOT _result EQUAL 0)
        message(FATAL_ERROR
            "run_reflectgen.cmake failed\nstdout:\n${_stdout}\nstderr:\n${_stderr}")
    endif()
endfunction()

function(_timestamp _path _out)
    file(TIMESTAMP "${_path}" _value "%Y%m%d%H%M%S")
    set(${_out} "${_value}" PARENT_SCOPE)
endfunction()

_write_config(false "disabled-a.wvz4")
_run()
file(READ "${_state}" _state_false)
if(NOT _state_false STREQUAL "WaveTrace=false\n")
    message(FATAL_ERROR "Initial false state was not recorded")
endif()
_timestamp("${_aggregate}" _disabled_timestamp)
file(READ "${_output}/root_class_closure_reflect_auto.h" _initial_disabled_header)
if(_initial_disabled_header MATCHES "ConfigTransitionRoot" OR
        _initial_disabled_header MATCHES "reflect_runtime.h")
    message(FATAL_ERROR "Initial WaveTrace=false did not emit an empty reflection header")
endif()
foreach(_index RANGE 0 3)
    set(_tag "0${_index}")
    set(_shard_base "${_output}/root_class_closure_shard_${_tag}")
    if(NOT EXISTS "${_shard_base}.cpp" OR
            NOT EXISTS "${_shard_base}_input.h" OR
            NOT EXISTS "${_shard_base}_reflect_auto.h")
        message(FATAL_ERROR "Initial WaveTrace=false did not emit shard ${_tag} placeholders")
    endif()
    file(READ "${_shard_base}.cpp" _disabled_shard_cpp)
    if(_disabled_shard_cpp MATCHES "wave_runtime.h" OR
            _disabled_shard_cpp MATCHES "_reflect_auto.h")
        message(FATAL_ERROR "Disabled shard ${_tag} still compiles reflection headers")
    endif()
endforeach()

execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 1)
_write_config(false "disabled-b.wvz4")
_run()
_timestamp("${_aggregate}" _runtime_false_timestamp)
if(NOT _runtime_false_timestamp STREQUAL _disabled_timestamp)
    message(FATAL_ERROR "Runtime-only false config edit rewrote generated reflection")
endif()

execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 1)
_write_config(true "enabled-a.wvz4")
_run()
_timestamp("${_aggregate}" _enabled_timestamp)
if(NOT _enabled_timestamp STRGREATER _runtime_false_timestamp)
    message(FATAL_ERROR "false -> true did not regenerate reflection")
endif()
file(READ "${_output}/root_class_closure_reflect_auto.h" _enabled_header)
if(NOT _enabled_header MATCHES "ConfigTransitionRoot")
    message(FATAL_ERROR "Enabled reflection output does not contain the root type")
endif()

execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 1)
_write_config(true "enabled-b.wvz4")
_run()
_timestamp("${_aggregate}" _runtime_true_timestamp)
if(NOT _runtime_true_timestamp STREQUAL _enabled_timestamp)
    message(FATAL_ERROR "Runtime-only true config edit rewrote generated reflection")
endif()

execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 1)
_write_config(false "disabled-c.wvz4")
_run()
_timestamp("${_aggregate}" _falling_timestamp)
if(NOT _falling_timestamp STREQUAL _enabled_timestamp)
    message(FATAL_ERROR "true -> false alone rewrote generated reflection and would force a rebuild")
endif()

execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 1)
file(APPEND "${_input}" "// source change while disabled\n")
_run()
_timestamp("${_aggregate}" _disabled_rebuild_timestamp)
if(NOT _disabled_rebuild_timestamp STRGREATER _falling_timestamp)
    message(FATAL_ERROR "A required source rebuild while false did not refresh placeholders")
endif()
file(READ "${_output}/root_class_closure_reflect_auto.h" _rebuilt_disabled_header)
if(_rebuilt_disabled_header MATCHES "ConfigTransitionRoot" OR
        _rebuilt_disabled_header MATCHES "reflect_runtime.h")
    message(FATAL_ERROR "Required rebuild while false retained full reflection")
endif()

execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 1)
_write_config(true "enabled-c.wvz4")
_run()
_timestamp("${_aggregate}" _reenabled_timestamp)
if(NOT _reenabled_timestamp STRGREATER _disabled_rebuild_timestamp)
    message(FATAL_ERROR "Second false -> true did not regenerate reflection")
endif()

message(STATUS "WaveTrace config transition dependency test passed")
