cmake_minimum_required(VERSION 3.18)

foreach(_required AQROOT AQARCH ARCH CMODEL_REGGEN_STAMP)
	if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
		message(FATAL_ERROR "cmodel reggen runner missing required variable ${_required}")
	endif()
endforeach()

set(_reggen_python "${AQROOT}/tools/bin/python/python")
set(_reggen_script "${AQROOT}/tools/bin/gcDefineGen.py")
set(_cmodel_dir "${AQARCH}/cmodel")
set(_reggen_inputs ${CMODEL_REGGEN_INPUTS})
set(_reggen_outputs ${CMODEL_REGGEN_OUTPUTS})

if(NOT _reggen_inputs)
	list(APPEND _reggen_inputs "${_reggen_script}")
endif()
if(NOT _reggen_outputs)
	list(APPEND _reggen_outputs
		"${_cmodel_dir}/inc/gcDefines.h"
		"${_cmodel_dir}/isa/src/isa_instructions.h")
endif()

if(WIN32 AND NOT EXISTS "${_reggen_python}")
	foreach(_reggen_python_ext .exe .bat .cmd)
		if(EXISTS "${_reggen_python}${_reggen_python_ext}")
			set(_reggen_python "${_reggen_python}${_reggen_python_ext}")
			break()
		endif()
	endforeach()
endif()

set(_need_reggen FALSE)
if(NOT EXISTS "${CMODEL_REGGEN_STAMP}")
	set(_need_reggen TRUE)
endif()

foreach(_reggen_output IN LISTS _reggen_outputs)
	if(NOT EXISTS "${_reggen_output}")
		set(_need_reggen TRUE)
	endif()
endforeach()

foreach(_reggen_input IN LISTS _reggen_inputs)
	if(EXISTS "${_reggen_input}" AND "${_reggen_input}" IS_NEWER_THAN "${CMODEL_REGGEN_STAMP}")
		set(_need_reggen TRUE)
	endif()
endforeach()

if(_need_reggen)
	message(STATUS "Running cmodel register generator")
	execute_process(
		COMMAND "${_reggen_python}" "${_reggen_script}" "${_cmodel_dir}" "${ARCH}"
		RESULT_VARIABLE _reggen_result
		OUTPUT_VARIABLE _reggen_stdout
		ERROR_VARIABLE _reggen_stderr
	)

	if(NOT "${_reggen_stdout}" STREQUAL "")
		message("${_reggen_stdout}")
	endif()
	if(NOT "${_reggen_stderr}" STREQUAL "")
		message("${_reggen_stderr}")
	endif()
	if(NOT _reggen_result EQUAL 0)
		message(FATAL_ERROR "cmodel reggen failed with exit code ${_reggen_result}")
	endif()
else()
	return()
endif()

set(_isa_instructions_header "${_cmodel_dir}/isa/src/isa_instructions.h")
if(WIN32 AND EXISTS "${_isa_instructions_header}")
	file(TO_NATIVE_PATH "${_isa_instructions_header}" _isa_instructions_header_native)
	execute_process(
		COMMAND attrib -r "${_isa_instructions_header_native}"
		RESULT_VARIABLE _attrib_result
		ERROR_VARIABLE _attrib_stderr
	)
	if(NOT _attrib_result EQUAL 0)
		message(WARNING "attrib -r failed for ${_isa_instructions_header_native}: ${_attrib_stderr}")
	endif()
endif()

file(TOUCH "${CMODEL_REGGEN_STAMP}")
