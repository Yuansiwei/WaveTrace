cmake_minimum_required(VERSION 3.18)

foreach(_required
        WAVETRACE_REFLECTGEN_EXE
        WAVETRACE_REFLECT_ROOT_CLASS
        WAVETRACE_REFLECT_OUTPUT_DIR
        WAVETRACE_REFLECT_AGGREGATE_HEADER
        WAVETRACE_REFLECT_LOG_FILE)
    if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
        message(FATAL_ERROR "WaveTrace ReflectGen runner missing required variable ${_required}")
    endif()
endforeach()

if(NOT DEFINED WAVETRACE_REFLECT_VERBOSE)
    set(WAVETRACE_REFLECT_VERBOSE OFF)
endif()

if(NOT EXISTS "${WAVETRACE_REFLECTGEN_EXE}")
    message(FATAL_ERROR "WaveTrace ReflectGen executable not found: ${WAVETRACE_REFLECTGEN_EXE}")
endif()

file(MAKE_DIRECTORY "${WAVETRACE_REFLECT_OUTPUT_DIR}")

# CMake only schedules this runner when its declared build dependencies say the
# reflection step is due.  Once scheduled, always launch ReflectGen.  JSON policy
# (including WaveTrace=false) belongs to ReflectGen; duplicating it here caused
# the build wrapper to skip pointer-table synchronization.

function(_wavetrace_reflect_finalize_success)
    if(DEFINED WAVETRACE_REFLECT_BUILD_STAMP AND
            NOT "${WAVETRACE_REFLECT_BUILD_STAMP}" STREQUAL "")
        get_filename_component(_wavetrace_reflect_stamp_dir
            "${WAVETRACE_REFLECT_BUILD_STAMP}" DIRECTORY)
        file(MAKE_DIRECTORY "${_wavetrace_reflect_stamp_dir}")
        execute_process(
            COMMAND "${CMAKE_COMMAND}" -E touch
                "${WAVETRACE_REFLECT_BUILD_STAMP}"
            RESULT_VARIABLE _wavetrace_reflect_stamp_result)
        if(NOT _wavetrace_reflect_stamp_result EQUAL 0)
            message(FATAL_ERROR
                "Failed to update WaveTrace reflection build stamp: ${WAVETRACE_REFLECT_BUILD_STAMP}")
        endif()
    endif()
endfunction()

if(DEFINED WAVETRACE_REFLECT_CLANG_ARGS_FILE AND
        NOT "${WAVETRACE_REFLECT_CLANG_ARGS_FILE}" STREQUAL "")
    if(NOT EXISTS "${WAVETRACE_REFLECT_CLANG_ARGS_FILE}")
        message(FATAL_ERROR
            "WaveTrace ReflectGen clang argument file not found: ${WAVETRACE_REFLECT_CLANG_ARGS_FILE}")
    endif()
    include("${WAVETRACE_REFLECT_CLANG_ARGS_FILE}")
endif()

set(_cmd
    "${WAVETRACE_REFLECTGEN_EXE}"
    --reflect-root-class "${WAVETRACE_REFLECT_ROOT_CLASS}"
)

if(DEFINED WAVETRACE_REFLECT_COMPILE_SHARDS AND WAVETRACE_REFLECT_COMPILE_SHARDS GREATER 0)
    list(APPEND _cmd --compile-shards "${WAVETRACE_REFLECT_COMPILE_SHARDS}")
endif()

if(DEFINED WAVETRACE_REFLECT_TARGET_LIST AND NOT "${WAVETRACE_REFLECT_TARGET_LIST}" STREQUAL "")
    if(NOT EXISTS "${WAVETRACE_REFLECT_TARGET_LIST}")
        message(FATAL_ERROR "WaveTrace ReflectGen target list not found: ${WAVETRACE_REFLECT_TARGET_LIST}")
    endif()
    get_filename_component(_wavetrace_reflect_safety_dir "${WAVETRACE_REFLECT_OUTPUT_DIR}" DIRECTORY)
    if("${_wavetrace_reflect_safety_dir}" STREQUAL "")
        set(_wavetrace_reflect_safety_dir "${WAVETRACE_REFLECT_OUTPUT_DIR}")
    endif()
    list(APPEND _cmd
        --batch-dir "${_wavetrace_reflect_safety_dir}"
        --no-recursive
        --extra-target-list "${WAVETRACE_REFLECT_TARGET_LIST}"
    )
elseif(DEFINED WAVETRACE_REFLECT_BATCH_DIR AND NOT "${WAVETRACE_REFLECT_BATCH_DIR}" STREQUAL "")
    list(APPEND _cmd
        --batch-dir "${WAVETRACE_REFLECT_BATCH_DIR}"
        --recursive
    )
else()
    message(FATAL_ERROR "WaveTrace ReflectGen needs WAVETRACE_REFLECT_TARGET_LIST or WAVETRACE_REFLECT_BATCH_DIR")
endif()

foreach(_arg IN LISTS WAVETRACE_REFLECT_EXTRA_ARGS)
    if(NOT "${_arg}" STREQUAL "")
        list(APPEND _cmd "${_arg}")
    endif()
endforeach()

if(DEFINED WAVETRACE_CONFIG_FILE AND NOT "${WAVETRACE_CONFIG_FILE}" STREQUAL "")
    list(APPEND _cmd --wavetrace-config "${WAVETRACE_CONFIG_FILE}")
endif()

list(APPEND _cmd
    -o "${WAVETRACE_REFLECT_OUTPUT_DIR}"
    --aggregate-header "${WAVETRACE_REFLECT_AGGREGATE_HEADER}"
    --log-file "${WAVETRACE_REFLECT_LOG_FILE}"
    --
)

foreach(_arg IN LISTS WAVETRACE_REFLECT_CLANG_ARGS)
    if(NOT "${_arg}" STREQUAL "")
        list(APPEND _cmd "${_arg}")
    endif()
endforeach()

set(_wavetrace_reflect_aggregate_output "${WAVETRACE_REFLECT_OUTPUT_DIR}/${WAVETRACE_REFLECT_AGGREGATE_HEADER}")

message(STATUS "WaveTrace ReflectGen: ${WAVETRACE_REFLECTGEN_EXE}")
message(STATUS "WaveTrace ReflectGen root: ${WAVETRACE_REFLECT_ROOT_CLASS}")
if(DEFINED WAVETRACE_REFLECT_TARGET_LIST AND NOT "${WAVETRACE_REFLECT_TARGET_LIST}" STREQUAL "")
    message(STATUS "WaveTrace ReflectGen target list: ${WAVETRACE_REFLECT_TARGET_LIST}")
elseif(DEFINED WAVETRACE_REFLECT_BATCH_DIR)
    message(STATUS "WaveTrace ReflectGen batch dir: ${WAVETRACE_REFLECT_BATCH_DIR}")
endif()
message(STATUS "WaveTrace ReflectGen output: ${WAVETRACE_REFLECT_OUTPUT_DIR}/${WAVETRACE_REFLECT_AGGREGATE_HEADER}")
message(STATUS "WaveTrace ReflectGen log: ${WAVETRACE_REFLECT_LOG_FILE}")

execute_process(
    COMMAND ${_cmd}
    RESULT_VARIABLE _result
    OUTPUT_VARIABLE _stdout
    ERROR_VARIABLE _stderr
)

set(_wavetrace_reflect_failed OFF)
set(_wavetrace_reflect_failure_reason "")
if(NOT _result EQUAL 0)
    set(_wavetrace_reflect_failed ON)
    set(_wavetrace_reflect_failure_reason "ReflectGen exited with code ${_result}")
elseif(NOT EXISTS "${_wavetrace_reflect_aggregate_output}")
    set(_wavetrace_reflect_failed ON)
    set(_wavetrace_reflect_failure_reason
        "ReflectGen exited successfully but did not create ${_wavetrace_reflect_aggregate_output}")
endif()

# ReflectGen owns both JSON policy and generated-output decisions.  Mark the
# custom-command stamp only after it has completed successfully.
if(NOT _wavetrace_reflect_failed)
    _wavetrace_reflect_finalize_success()
endif()

set(_wavetrace_reflect_dump_output OFF)
if(WAVETRACE_REFLECT_VERBOSE OR _wavetrace_reflect_failed)
    set(_wavetrace_reflect_dump_output ON)
endif()

set(_log "")
if(EXISTS "${WAVETRACE_REFLECT_LOG_FILE}")
    file(READ "${WAVETRACE_REFLECT_LOG_FILE}" _log)
endif()

if(_wavetrace_reflect_dump_output)
    if(NOT "${_log}" STREQUAL "")
        message(STATUS "===== WaveTrace ReflectGen log: ${WAVETRACE_REFLECT_LOG_FILE} =====")
        message("${_log}")
        message(STATUS "===== End WaveTrace ReflectGen log =====")
    else()
        if(NOT "${_stdout}" STREQUAL "")
            message("${_stdout}")
        endif()
        if(NOT "${_stderr}" STREQUAL "")
            message("${_stderr}")
        endif()
        message(STATUS "WaveTrace ReflectGen did not create a log file: ${WAVETRACE_REFLECT_LOG_FILE}")
    endif()
endif()

if(_wavetrace_reflect_failed)
    # Convert Clang's path:line:column diagnostics to MSBuild's native format so
    # Visual Studio shows the real source locations as clickable Error List rows.
    if(EXISTS "${WAVETRACE_REFLECT_LOG_FILE}")
        file(STRINGS "${WAVETRACE_REFLECT_LOG_FILE}" _wavetrace_reflect_diagnostic_lines)
    else()
        set(_wavetrace_reflect_diagnostic_text "${_stderr}")
        string(REPLACE ";" "\\;" _wavetrace_reflect_diagnostic_text "${_wavetrace_reflect_diagnostic_text}")
        string(REPLACE "\r\n" "\n" _wavetrace_reflect_diagnostic_text "${_wavetrace_reflect_diagnostic_text}")
        string(REPLACE "\r" "\n" _wavetrace_reflect_diagnostic_text "${_wavetrace_reflect_diagnostic_text}")
        string(REPLACE "\n" ";" _wavetrace_reflect_diagnostic_lines "${_wavetrace_reflect_diagnostic_text}")
    endif()
    if(NOT DEFINED _wavetrace_reflect_diagnostic_lines)
        set(_wavetrace_reflect_diagnostic_text "${_stderr}")
        set(_wavetrace_reflect_diagnostic_lines "${_wavetrace_reflect_diagnostic_text}")
    endif()
    set(_wavetrace_reflect_emitted_diagnostics)
    foreach(_wavetrace_reflect_diagnostic_line IN LISTS _wavetrace_reflect_diagnostic_lines)
        if(_wavetrace_reflect_diagnostic_line MATCHES "^(.+):([0-9]+):([0-9]+):[ ]*(fatal error|error|warning):[ ]*(.*)$")
            set(_wavetrace_reflect_diagnostic_kind "${CMAKE_MATCH_4}")
            if(_wavetrace_reflect_diagnostic_kind STREQUAL "fatal error")
                set(_wavetrace_reflect_diagnostic_kind "error")
            endif()
            set(_wavetrace_reflect_vs_diagnostic
                "${CMAKE_MATCH_1}(${CMAKE_MATCH_2},${CMAKE_MATCH_3}): ${_wavetrace_reflect_diagnostic_kind} WTR1002: ${CMAKE_MATCH_5}")
            if(NOT _wavetrace_reflect_vs_diagnostic IN_LIST _wavetrace_reflect_emitted_diagnostics)
                list(APPEND _wavetrace_reflect_emitted_diagnostics "${_wavetrace_reflect_vs_diagnostic}")
                message("${_wavetrace_reflect_vs_diagnostic}")
            endif()
        endif()
    endforeach()

    set(_wavetrace_reflect_error_source "${WAVETRACE_REFLECT_LOG_FILE}")
    if(DEFINED WAVETRACE_REFLECT_TARGET_LIST AND EXISTS "${WAVETRACE_REFLECT_TARGET_LIST}")
        file(STRINGS "${WAVETRACE_REFLECT_TARGET_LIST}" _wavetrace_reflect_error_sources LIMIT_COUNT 1)
        if(_wavetrace_reflect_error_sources)
            list(GET _wavetrace_reflect_error_sources 0 _wavetrace_reflect_error_source)
            string(STRIP "${_wavetrace_reflect_error_source}" _wavetrace_reflect_error_source)
            string(REGEX REPLACE "^\"(.*)\"$" "\\1" _wavetrace_reflect_error_source "${_wavetrace_reflect_error_source}")
        endif()
    endif()

    # MSBuild recognizes this form and adds a clickable entry to Visual Studio's
    # Error List. The full stdout, stderr and ReflectGen log are printed above.
    message("${_wavetrace_reflect_error_source}(1): error WTR1001: ${_wavetrace_reflect_failure_reason}. Full log: ${WAVETRACE_REFLECT_LOG_FILE}")
    message(FATAL_ERROR "WaveTrace ReflectGen failed. See the WTR1001 diagnostic and full log above.")
endif()
