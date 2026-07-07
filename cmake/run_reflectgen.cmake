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

set(_cmd
    "${WAVETRACE_REFLECTGEN_EXE}"
    --reflect-root-class "${WAVETRACE_REFLECT_ROOT_CLASS}"
    --allow-errors
)

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
set(_wavetrace_reflect_inputs "${WAVETRACE_REFLECTGEN_EXE}")
foreach(_wavetrace_reflect_list_file WAVETRACE_REFLECT_TARGET_LIST WAVETRACE_REFLECT_INPUT_LIST)
    if(DEFINED ${_wavetrace_reflect_list_file} AND EXISTS "${${_wavetrace_reflect_list_file}}")
        list(APPEND _wavetrace_reflect_inputs "${${_wavetrace_reflect_list_file}}")
        file(STRINGS "${${_wavetrace_reflect_list_file}}" _wavetrace_reflect_list_entries)
        foreach(_wavetrace_reflect_list_entry IN LISTS _wavetrace_reflect_list_entries)
            string(STRIP "${_wavetrace_reflect_list_entry}" _wavetrace_reflect_list_entry)
            string(REGEX REPLACE "^\"(.*)\"$" "\\1" _wavetrace_reflect_list_entry "${_wavetrace_reflect_list_entry}")
            if(NOT "${_wavetrace_reflect_list_entry}" STREQUAL "")
                list(APPEND _wavetrace_reflect_inputs "${_wavetrace_reflect_list_entry}")
            endif()
        endforeach()
    endif()
endforeach()

set(_wavetrace_reflect_need_run FALSE)
if(NOT EXISTS "${_wavetrace_reflect_aggregate_output}")
    set(_wavetrace_reflect_need_run TRUE)
endif()
foreach(_wavetrace_reflect_input IN LISTS _wavetrace_reflect_inputs)
    if(EXISTS "${_wavetrace_reflect_input}" AND "${_wavetrace_reflect_input}" IS_NEWER_THAN "${_wavetrace_reflect_aggregate_output}")
        set(_wavetrace_reflect_need_run TRUE)
    endif()
endforeach()
if(NOT _wavetrace_reflect_need_run)
    return()
endif()

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

set(_wavetrace_reflect_dump_output OFF)
if(WAVETRACE_REFLECT_VERBOSE OR NOT _result EQUAL 0)
    set(_wavetrace_reflect_dump_output ON)
endif()

if(_wavetrace_reflect_dump_output AND NOT "${_stdout}" STREQUAL "")
    message("${_stdout}")
endif()
if(_wavetrace_reflect_dump_output AND NOT "${_stderr}" STREQUAL "")
    message("${_stderr}")
endif()

if(EXISTS "${WAVETRACE_REFLECT_LOG_FILE}")
    if(_wavetrace_reflect_dump_output)
        file(READ "${WAVETRACE_REFLECT_LOG_FILE}" _log)
        if(NOT "${_log}" STREQUAL "")
            message(STATUS "===== WaveTrace ReflectGen log: ${WAVETRACE_REFLECT_LOG_FILE} =====")
            message("${_log}")
            message(STATUS "===== End WaveTrace ReflectGen log =====")
        endif()
    endif()
elseif(_wavetrace_reflect_dump_output)
    message(STATUS "WaveTrace ReflectGen did not create a log file: ${WAVETRACE_REFLECT_LOG_FILE}")
endif()

if(NOT _result EQUAL 0)
    message(FATAL_ERROR "WaveTrace ReflectGen failed with exit code ${_result}")
endif()
