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

message(STATUS "WaveTrace ReflectGen: ${WAVETRACE_REFLECTGEN_EXE}")
message(STATUS "WaveTrace ReflectGen root: ${WAVETRACE_REFLECT_ROOT_CLASS}")
if(DEFINED WAVETRACE_REFLECT_TARGET_LIST AND NOT "${WAVETRACE_REFLECT_TARGET_LIST}" STREQUAL "")
    message(STATUS "WaveTrace ReflectGen target list: ${WAVETRACE_REFLECT_TARGET_LIST}")
elseif(DEFINED WAVETRACE_REFLECT_BATCH_DIR)
    message(STATUS "WaveTrace ReflectGen batch dir: ${WAVETRACE_REFLECT_BATCH_DIR}")
endif()
message(STATUS "WaveTrace ReflectGen output: ${WAVETRACE_REFLECT_OUTPUT_DIR}/${WAVETRACE_REFLECT_AGGREGATE_HEADER}")

execute_process(
    COMMAND ${_cmd}
    RESULT_VARIABLE _result
    OUTPUT_VARIABLE _stdout
    ERROR_VARIABLE _stderr
)

if(NOT "${_stdout}" STREQUAL "")
    message("${_stdout}")
endif()
if(NOT "${_stderr}" STREQUAL "")
    message("${_stderr}")
endif()

if(EXISTS "${WAVETRACE_REFLECT_LOG_FILE}")
    file(READ "${WAVETRACE_REFLECT_LOG_FILE}" _log)
    if(NOT "${_log}" STREQUAL "")
        message(STATUS "===== WaveTrace ReflectGen log: ${WAVETRACE_REFLECT_LOG_FILE} =====")
        message("${_log}")
        message(STATUS "===== End WaveTrace ReflectGen log =====")
    endif()
else()
    message(STATUS "WaveTrace ReflectGen did not create a log file: ${WAVETRACE_REFLECT_LOG_FILE}")
endif()

if(NOT _result EQUAL 0)
    message(FATAL_ERROR "WaveTrace ReflectGen failed with exit code ${_result}")
endif()
