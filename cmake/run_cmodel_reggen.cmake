cmake_minimum_required(VERSION 3.18)

foreach(_required AQROOT AQARCH ARCH CMODEL_REGGEN_STAMP)
	if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
		message(FATAL_ERROR "cmodel reggen runner missing required variable ${_required}")
	endif()
endforeach()

set(_reggen_python "${AQROOT}/tools/bin/python/python")
set(_reggen_script "${AQROOT}/tools/bin/gcDefineGen.py")
set(_cmodel_dir "${AQARCH}/cmodel")

if(WIN32 AND NOT EXISTS "${_reggen_python}")
	foreach(_reggen_python_ext .exe .bat .cmd)
		if(EXISTS "${_reggen_python}${_reggen_python_ext}")
			set(_reggen_python "${_reggen_python}${_reggen_python_ext}")
			break()
		endif()
	endforeach()
endif()

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
