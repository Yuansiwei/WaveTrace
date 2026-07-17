cmake_minimum_required(VERSION 3.18)
project(cmodel VERSION 1.0.0 LANGUAGES C CXX)

# Set C++ standard
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Existing .bat files usually pass these as process environment variables
# rather than CMake -D cache variables. Import them before deriving defaults.
foreach(_cmodel_env_path WORKDIR AQROOT AQARCH AQTOOLS AQCMODEL VIVANTE_SDK_DIR CMODEL_OUTPUT_DIR)
	if(NOT DEFINED ${_cmodel_env_path} AND DEFINED ENV{${_cmodel_env_path}})
		file(TO_CMAKE_PATH "$ENV{${_cmodel_env_path}}" _cmodel_env_path_value)
		set(${_cmodel_env_path} "${_cmodel_env_path_value}")
	endif()
endforeach()
if(NOT DEFINED ARCH AND DEFINED ENV{ARCH})
	set(ARCH "$ENV{ARCH}")
endif()

#define custom configuration

option(GPGPU_ONLY "only GPGPU build" ON)
option(ISAMODEL "amodel config" OFF)
option(BUILD_SYSTEMC "systemc build" OFF)
get_property(CMODEL_MULTI_CONFIG GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(CMODEL_MULTI_CONFIG)
	set(CMODEL_DEFAULT_CONFIGS Debug Release RelWithDebInfo MinSizeRel Profile)
	foreach(_cmodel_config IN LISTS CMODEL_DEFAULT_CONFIGS)
		if(NOT _cmodel_config IN_LIST CMAKE_CONFIGURATION_TYPES)
			list(APPEND CMAKE_CONFIGURATION_TYPES "${_cmodel_config}")
		endif()
	endforeach()
	set(CMAKE_CONFIGURATION_TYPES "${CMAKE_CONFIGURATION_TYPES}" CACHE STRING "Available build configurations" FORCE)
else()
	if(NOT CMAKE_BUILD_TYPE)
		set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
	endif()
	set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel Profile)
endif()
set(CMODEL_MSVC_MP_COUNT "32" CACHE STRING "MSVC /MP process count for cmodel")
set(CMODEL_RELEASE_DEBUG_INFO ON CACHE BOOL "Emit Release debug info and link flags so profilers can resolve symbols")
set(CMODEL_VERBOSE_BUILD OFF CACHE BOOL "Enable verbose generated build command output")
if(WIN32)
	if(MSVC)
		add_compile_options(/Zf)
		set(CMAKE_CXX_FLAGS_ALL_DEBUG " /O1 /DDEBUG /vmg -Zm300 /bigobj")
	
	# Custom All_Release flags
		set(CMAKE_CXX_FLAGS_ALL_RELEASE "/O2 /DNDEBUG /vmg -Zm300 /bigobj")
		set(CMAKE_C_FLAGS_PROFILE "${CMAKE_C_FLAGS_RELEASE} /Z7 /Oy-" CACHE STRING "C flags for Profile builds" FORCE)
		set(CMAKE_CXX_FLAGS_PROFILE "${CMAKE_CXX_FLAGS_RELEASE} /Z7 /Oy-" CACHE STRING "C++ flags for Profile builds" FORCE)
		set(CMAKE_EXE_LINKER_FLAGS_PROFILE "${CMAKE_EXE_LINKER_FLAGS_RELEASE} /DEBUG:FULL /OPT:REF /OPT:ICF /INCREMENTAL:NO" CACHE STRING "Executable linker flags for Profile builds" FORCE)
		set(CMAKE_SHARED_LINKER_FLAGS_PROFILE "${CMAKE_SHARED_LINKER_FLAGS_RELEASE} /DEBUG:FULL /OPT:REF /OPT:ICF /INCREMENTAL:NO" CACHE STRING "Shared linker flags for Profile builds" FORCE)
		set(CMAKE_MODULE_LINKER_FLAGS_PROFILE "${CMAKE_MODULE_LINKER_FLAGS_RELEASE} /DEBUG:FULL /OPT:REF /OPT:ICF /INCREMENTAL:NO" CACHE STRING "Module linker flags for Profile builds" FORCE)
	endif()
	# Custom All_Debug flags

endif()
if(WIN32)
	if(NOT DEFINED  AQROOT)
		set(AQROOT ${WORKDIR}/SW/Rel9x/dev9.0.x/projects.dev)
	endif()
	if(NOT DEFINED  AQARCH)
		set(AQARCH ${WORKDIR}/HW/projects.se/arch/XAQ2)
	endif()
	if (NOT DEFINED AQCMODEL)
		set(AQCMODEL ${WORKDIR}/cmodel)
	endif()
	if(NOT VIVANTE_SDK_DIR)
		set(VIVANTE_SDK_DIR ${WORKDIR}/RUNDIR/BUILD)
	endif()
	if(NOT CMODEL_OUTPUT_DIR)
		set(CMODEL_OUTPUT_DIR ${VIVANTE_SDK_DIR}/lib)
	endif()
endif()
foreach(_cmodel_cmake_path WORKDIR AQROOT AQARCH AQTOOLS AQCMODEL VIVANTE_SDK_DIR CMODEL_OUTPUT_DIR)
	if(DEFINED ${_cmodel_cmake_path})
		file(TO_CMAKE_PATH "${${_cmodel_cmake_path}}" _cmodel_cmake_path_value)
		set(${_cmodel_cmake_path} "${_cmodel_cmake_path_value}")
	endif()
endforeach()
if(NOT DEFINED DV_DUMP)
	set(DV_DUMP OFF)
endif()
set(DV_DUMP OFF)

set(WAVETRACE_DEFAULT_ENABLED OFF)
if(MSVC)
	set(WAVETRACE_DEFAULT_ENABLED ON)
endif()
option(ENABLE_WAVETRACE "enable WaveTrace waveform tracing for MSVC builds" ${WAVETRACE_DEFAULT_ENABLED})
option(ENABLE_WAVETRACE_API_COMPAT "provide WaveTrace business-code types and macros without enabling waveform tracing" ON)
set(WAVETRACE_ENABLED OFF)
if(MSVC AND ENABLE_WAVETRACE)
	set(WAVETRACE_ENABLED ON)
endif()
set(WAVETRACE_ROOT "${CMAKE_CURRENT_LIST_DIR}/WaveTracer" CACHE PATH "WaveTrace package root")
set(WAVETRACE_REFLECT_ROOT_CLASS "cmoTopLevel" CACHE STRING "WaveTrace reflection root class")
if(WAVETRACE_REFLECT_ROOT_CLASS STREQUAL "cmTopLevel")
	set(WAVETRACE_REFLECT_ROOT_CLASS "cmoTopLevel" CACHE STRING "WaveTrace reflection root class" FORCE)
endif()
set(WAVETRACE_REFLECT_ENTRY_HEADER "" CACHE FILEPATH "Header that defines the WaveTrace reflection root class. Empty means auto-detect from ALL_HEADERS.")
set(WAVETRACE_REFLECT_BATCH_DIR "${CMAKE_CURRENT_SOURCE_DIR}" CACHE PATH "Directory scanned by ReflectGen")
set(WAVETRACE_REFLECT_EXTRA_ARGS "" CACHE STRING "Extra ReflectGen arguments, for example --allow-errors")
set(WAVETRACE_REFLECT_EXTRA_CLANG_ARGS "" CACHE STRING "Extra clang parse arguments for ReflectGen, for example -Ipath -DMACRO=1")
set(WAVETRACE_REFLECT_VERBOSE OFF CACHE BOOL "Print full WaveTrace ReflectGen stdout/stderr/log on successful builds")
set(WAVETRACE_REFLECT_COMPILE_SHARDS "8" CACHE STRING "Number of independently compiled WaveTrace reflection shards; 0 disables")
if(NOT WAVETRACE_REFLECT_COMPILE_SHARDS MATCHES "^[0-9]+$" OR WAVETRACE_REFLECT_COMPILE_SHARDS GREATER 64)
	message(FATAL_ERROR "WAVETRACE_REFLECT_COMPILE_SHARDS must be an integer from 0 through 64")
endif()

set(WAVETRACE_API_COMPAT_ENABLED OFF)
if(ENABLE_WAVETRACE_API_COMPAT OR WAVETRACE_ENABLED)
	if(NOT EXISTS "${WAVETRACE_ROOT}/reflect_macro.h")
		if(DEFINED ENV{WAVETRACE_ROOT} AND EXISTS "$ENV{WAVETRACE_ROOT}/reflect_macro.h")
			set(WAVETRACE_ROOT "$ENV{WAVETRACE_ROOT}" CACHE PATH "WaveTrace package root" FORCE)
		elseif(EXISTS "${CMAKE_CURRENT_LIST_DIR}/reflect_macro.h")
			set(WAVETRACE_ROOT "${CMAKE_CURRENT_LIST_DIR}" CACHE PATH "WaveTrace package root" FORCE)
		elseif(EXISTS "${CMAKE_CURRENT_LIST_DIR}/WaveTracer/reflect_macro.h")
			set(WAVETRACE_ROOT "${CMAKE_CURRENT_LIST_DIR}/WaveTracer" CACHE PATH "WaveTrace package root" FORCE)
		elseif(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../WaveTracer/reflect_macro.h")
			set(WAVETRACE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../WaveTracer" CACHE PATH "WaveTrace package root" FORCE)
		elseif(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../WaveTrace/reflect_macro.h")
			set(WAVETRACE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../WaveTrace" CACHE PATH "WaveTrace package root" FORCE)
		endif()
	endif()
	if(NOT EXISTS "${WAVETRACE_ROOT}/reflect_macro.h")
		message(FATAL_ERROR "WaveTrace API compatibility is enabled but reflect_macro.h was not found. Put the WaveTrace package next to this CMake file, set WAVETRACE_ROOT, set ENV{WAVETRACE_ROOT}, or explicitly set ENABLE_WAVETRACE_API_COMPAT=OFF for business code that does not use wave::* APIs. Current WAVETRACE_ROOT='${WAVETRACE_ROOT}'")
	endif()
	set(WAVETRACE_API_COMPAT_ENABLED ON)
endif()

if(WAVETRACE_ENABLED)
	set(WAVETRACE_BUILD_TOP_DIR "${CMAKE_BINARY_DIR}/WaveTracer")
	# User-owned outputs stay in the original WaveTracer source package. Only
	# transient argument/input lists remain under the CMake build tree.
	set(WAVETRACE_GEN_DIR "${WAVETRACE_ROOT}/generated_reflect")
	set(WAVETRACE_CONFIG_FILE "${WAVETRACE_ROOT}/wavetrace_config.json" CACHE FILEPATH "WaveTrace reflection/runtime configuration and WavePtr switches")
	if(NOT EXISTS "${WAVETRACE_CONFIG_FILE}")
		get_filename_component(_wavetrace_config_dir "${WAVETRACE_CONFIG_FILE}" DIRECTORY)
		file(MAKE_DIRECTORY "${_wavetrace_config_dir}")
		file(WRITE "${WAVETRACE_CONFIG_FILE}" "{\n  \"WaveTrace\": true,\n  \"WaveTraceFileName\": \"wave.wvz4\",\n  \"WaveTraceStart\": \"\",\n  \"WaveTraceEnd\": \"\",\n  \"WaveTraceLevel\": \"\",\n  \"wave_ptr_members\": []\n}\n")
	endif()
	set(WAVETRACE_TOOL_DIR "${WAVETRACE_ROOT}/tools/bin")
	set(WAVETRACE_ZSTD_INCLUDE_DIR "${WAVETRACE_ROOT}/third_party/zstd/include" CACHE PATH "Prebuilt zstd include directory")
	set(WAVETRACE_CLANG_RESOURCE_DIR "${WAVETRACE_ROOT}/tools/lib/clang/22" CACHE PATH "Clang resource directory used by WaveTrace ReflectGen")
	set(WAVETRACE_REFLECTGEN_EXE "${WAVETRACE_TOOL_DIR}/wavetrace_reflectgen.exe" CACHE FILEPATH "Prebuilt WaveTrace ReflectGen executable")
	if(WIN32)
		set(WAVETRACE_ZSTD_LIB_RELEASE "${WAVETRACE_ROOT}/tools/lib/zstd_release.lib" CACHE FILEPATH "Prebuilt static zstd Release library")
		set(WAVETRACE_ZSTD_LIB_DEBUG "${WAVETRACE_ROOT}/tools/lib/zstd_debug.lib" CACHE FILEPATH "Prebuilt static zstd Debug library")
	endif()

	if(NOT EXISTS "${WAVETRACE_ZSTD_INCLUDE_DIR}/zstd.h")
		message(FATAL_ERROR "WaveTrace zstd header was not found under '${WAVETRACE_ZSTD_INCLUDE_DIR}'")
	endif()
	if(NOT EXISTS "${WAVETRACE_CLANG_RESOURCE_DIR}/include/stddef.h" AND EXISTS "${WAVETRACE_ROOT}/third_party/llvm/llvm-local/lib/clang/22/include/stddef.h")
		set(WAVETRACE_CLANG_RESOURCE_DIR "${WAVETRACE_ROOT}/third_party/llvm/llvm-local/lib/clang/22" CACHE PATH "Clang resource directory used by WaveTrace ReflectGen" FORCE)
	endif()
	if(NOT EXISTS "${WAVETRACE_CLANG_RESOURCE_DIR}/include/stddef.h")
		message(FATAL_ERROR "WaveTrace clang resource directory was not found: '${WAVETRACE_CLANG_RESOURCE_DIR}'. Expected include/stddef.h under it.")
	endif()
	if(NOT EXISTS "${WAVETRACE_REFLECTGEN_EXE}")
		message(FATAL_ERROR "WaveTrace prebuilt ReflectGen executable was not found: '${WAVETRACE_REFLECTGEN_EXE}'")
	endif()
	set(WAVETRACE_REFLECTGEN_DEPENDS "${WAVETRACE_REFLECTGEN_EXE}")
	if(WIN32)
		if(NOT EXISTS "${WAVETRACE_ZSTD_LIB_RELEASE}")
			message(FATAL_ERROR "WaveTrace prebuilt static zstd Release library was not found: '${WAVETRACE_ZSTD_LIB_RELEASE}'")
		endif()
		if(NOT EXISTS "${WAVETRACE_ZSTD_LIB_DEBUG}")
			message(FATAL_ERROR "WaveTrace prebuilt static zstd Debug library was not found: '${WAVETRACE_ZSTD_LIB_DEBUG}'")
		endif()
		if(NOT EXISTS "${WAVETRACE_TOOL_DIR}/libclang.dll")
			message(FATAL_ERROR "WaveTrace ReflectGen runtime dependency was not found: '${WAVETRACE_TOOL_DIR}/libclang.dll'")
		endif()
	endif()
endif()
set(WAVETRACE_CMODEL_FORCE_INCLUDE_MSVC)
set(WAVETRACE_CMODEL_FORCE_INCLUDE_CLANG)
if(WAVETRACE_API_COMPAT_ENABLED)
	set(WAVETRACE_CMODEL_FORCE_INCLUDE_MSVC "/FI${WAVETRACE_ROOT}/reflect_macro.h")
	# Keep the option and its argument as one de-duplication unit.  CMake may
	# otherwise turn two "-include <header>" pairs into one -include followed by
	# a bare header path, which GCC then treats as a second input file.
	set(WAVETRACE_CMODEL_FORCE_INCLUDE_CLANG
		"SHELL:-include \"${WAVETRACE_ROOT}/reflect_macro.h\"")
endif()
# Collect all source files
set(ALL_SOURCES)
set(ALL_HEADERS)
set(RT_CONFIG_FILES)
set(ALL_INCLUDE_DIRECTORY)
if(WIN32)
	list(APPEND ALL_INCLUDE_DIRECTORY ${AQTOOLS}/GPU/perf/systemc-2.3.3/src ${CMAKE_CURRENT_SOURCE_DIR})
elseif(UNIX)
	list(APPEND ALL_INCLUDE_DIRECTORY ${AQTOOLS}/GPU/perf/systemc-2.3.3/src ${CMAKE_CURRENT_SOURCE_DIR})
endif()

add_subdirectory(blt)
add_subdirectory(CacheSubsystem)
add_subdirectory(CFI)
add_subdirectory(cluster)
add_subdirectory(common)
add_subdirectory(configs)
add_subdirectory(CWD)
add_subdirectory(dma)
add_subdirectory(DPPU)
add_subdirectory(dump)
add_subdirectory(FA)
add_subdirectory(float)
add_subdirectory(GenSource)
add_subdirectory(HostInterface)
add_subdirectory(inc)
add_subdirectory(isa)
add_subdirectory(L2L3C)
add_subdirectory(memory)
add_subdirectory(pa30)
add_subdirectory(pixel)
add_subdirectory(PPU)
add_subdirectory(PWD)
add_subdirectory(QPPU)
add_subdirectory(raster)
add_subdirectory(shader)
add_subdirectory(SysWrapper)
add_subdirectory(texture)
add_subdirectory(tfb)
add_subdirectory(toplevel)
add_subdirectory(ts)
add_subdirectory(tx)
add_subdirectory(cmonitor)
add_subdirectory(prof_ctrl)
add_subdirectory(${AQTOOLS}/GcTest/mathlib ${CMAKE_CURRENT_BINARY_DIR}/mathlib)

#prepare reggen
set(CMODEL_INC   ${AQARCH}/cmodel/inc)
set(REG_DIR      ${AQARCH}/reg)

# add runtime config files to "configs" filter, and do not set depend of them
source_group("configs" FILES ${RT_CONFIG_FILES})
foreach(file ${RT_CONFIG_FILES})
	set_source_files_properties(${file}
		PROPERTIES
			HEADER_FILE_ONLY TRUE
	)
endforeach()

# Add include directories for all folders
# Create library with all sources
set(GENERATED_CPP "${CMAKE_CURRENT_SOURCE_DIR}/GenSource/vsiRegisterList.cpp")
list(APPEND ALL_SOURCES ${GENERATED_CPP})

add_library(cmodel STATIC ${ALL_SOURCES} ${ALL_HEADERS} ${RT_CONFIG_FILES})

if(WAVETRACE_ENABLED)
	set_source_files_properties("${WAVETRACE_CONFIG_FILE}" PROPERTIES HEADER_FILE_ONLY TRUE)
	target_sources(cmodel PRIVATE "${WAVETRACE_CONFIG_FILE}")
	source_group("configs" FILES "${WAVETRACE_CONFIG_FILE}")
endif()

if(WAVETRACE_API_COMPAT_ENABLED)
	target_include_directories(cmodel PUBLIC "${WAVETRACE_ROOT}")
endif()

target_precompile_headers(cmodel PUBLIC inc/vsiPrecomp.h)
if(MSVC)
	set_target_properties(cmodel PROPERTIES OUTPUT_NAME "arch.cmodel")
	target_compile_definitions(cmodel PUBLIC _MSVC)
endif()
if(CMODEL_VERBOSE_BUILD)
	set(CMAKE_VERBOSE_MAKEFILE ON)
endif()
# Set output directories
if(ISAMODEL)
	target_compile_definitions(cmodel PUBLIC gcdMODELING_MODE=2)
endif()
if(WIN32)

	set(CMODEL_PATH "${WORKDIR}\\HW\\projects.se\\arch\\XAQ2\\cmodel")
	#file(CMODEL_PATH_DEFINE "${CMODEL_PATH}" SANITIZED_PATH)
	string(REPLACE "\\" "/" CMODEL_PATH "${AQARCH}/cmodel")
	target_compile_options(cmodel PUBLIC -DAQ2_DMA=1 -DXAQ2=1 -DEMULATOR -DPATH_TO_CMODEL="${CMODEL_PATH}" $<$<CONFIG:Debug>:-DUSE_PRINTF> $<$<CONFIG:Debug>:-D_DEBUG> $<$<BOOL:${DV_DUMP}>:-DDV_DUMP=1> $<$<BOOL:${ISAMODEL}>:-DgcdMODELING_MODE=2>)
	if (MSVC)
		target_compile_options(cmodel PRIVATE ${WAVETRACE_CMODEL_FORCE_INCLUDE_MSVC}
											/FI${AQARCH}/cmodel/inc/vsiPrecomp.h
											/vmg -Zm300 /bigobj -D_CRT_SECURE_NO_WARNINGS
											/MP${CMODEL_MSVC_MP_COUNT}
											"$<$<AND:$<BOOL:${CMODEL_RELEASE_DEBUG_INFO}>,$<CONFIG:Release>>:/Z7>"
											"$<$<AND:$<BOOL:${CMODEL_RELEASE_DEBUG_INFO}>,$<CONFIG:Release>>:/Oy->"
											"$<$<CONFIG:RelWithDebInfo>:/Z7>"
											"$<$<CONFIG:RelWithDebInfo>:/Oy->"
											"$<$<CONFIG:Profile>:/Z7>"
											"$<$<CONFIG:Profile>:/Oy->")
		set_property(TARGET cmodel APPEND PROPERTY INTERFACE_LINK_OPTIONS
			"$<$<AND:$<BOOL:${CMODEL_RELEASE_DEBUG_INFO}>,$<CONFIG:Release>>:/DEBUG:FULL>"
			"$<$<AND:$<BOOL:${CMODEL_RELEASE_DEBUG_INFO}>,$<CONFIG:Release>>:/OPT:REF>"
			"$<$<AND:$<BOOL:${CMODEL_RELEASE_DEBUG_INFO}>,$<CONFIG:Release>>:/OPT:ICF>"
			"$<$<AND:$<BOOL:${CMODEL_RELEASE_DEBUG_INFO}>,$<CONFIG:Release>>:/INCREMENTAL:NO>"
			"$<$<CONFIG:RelWithDebInfo>:/DEBUG:FULL>"
			"$<$<CONFIG:RelWithDebInfo>:/OPT:REF>"
			"$<$<CONFIG:RelWithDebInfo>:/OPT:ICF>"
			"$<$<CONFIG:RelWithDebInfo>:/INCREMENTAL:NO>"
			"$<$<CONFIG:Profile>:/DEBUG:FULL>"
			"$<$<CONFIG:Profile>:/OPT:REF>"
			"$<$<CONFIG:Profile>:/OPT:ICF>"
			"$<$<CONFIG:Profile>:/INCREMENTAL:NO>")
	else()
		# vsiPrecomp.h is already supplied by target_precompile_headers().
		target_compile_options(cmodel PRIVATE ${WAVETRACE_CMODEL_FORCE_INCLUDE_CLANG})
		#target_compile_options(cmodel PRIVATE -flto)
		target_link_libraries(cmodel PRIVATE winpthread)
	endif()
	if (ENABLE_CMODEL_DENORM)
		target_compile_options(cmodel PUBLIC -DgcdCMODEL_DENORM_SUPPORT=1)
	endif()

	
	
	#add pre-build event
	if(MSVC)
		set(CMODEL_REGGEN_PYTHON "${AQROOT}/tools/bin/python/python")
		if(NOT EXISTS "${CMODEL_REGGEN_PYTHON}")
			foreach(_cmodel_python_ext .exe .bat .cmd)
				if(EXISTS "${CMODEL_REGGEN_PYTHON}${_cmodel_python_ext}")
					set(CMODEL_REGGEN_PYTHON "${CMODEL_REGGEN_PYTHON}${_cmodel_python_ext}")
					break()
				endif()
			endforeach()
		endif()
		add_custom_command(
			TARGET cmodel
			PRE_BUILD
			COMMAND "${CMODEL_REGGEN_PYTHON}" "${AQROOT}/tools/bin/gcDefineGen.py" "${AQARCH}/cmodel" "${ARCH}"
			COMMAND attrib -r "${AQARCH}/cmodel/isa/src/isa_instructions.h"
			VERBATIM
			# isa_instructions.h generation remains disabled, matching the original build.
			)
	endif()
	set(CMODEL_MONITOR_LIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/cmonitor/cmodel_monitor/cmodel_monitor/x64")
	file(TO_CMAKE_PATH "${CMODEL_MONITOR_LIB_DIR}" CMODEL_MONITOR_LIB_DIR)
	if(MSVC)
		target_link_directories(cmodel INTERFACE
			"$<$<CONFIG:Debug>:${AQTOOLS}/GPU/perf/systemc-2.3.3/msvc10/SystemC/x64/Debug>"
			"$<$<NOT:$<CONFIG:Debug>>:${AQTOOLS}/GPU/perf/systemc-2.3.3/msvc10/SystemC/x64/Release>"
			"$<$<CONFIG:Debug>:${CMODEL_MONITOR_LIB_DIR}/Debug>"
			"$<$<CONFIG:Debug>:${CMODEL_MONITOR_LIB_DIR}>"
			"$<$<NOT:$<CONFIG:Debug>>:${CMODEL_MONITOR_LIB_DIR}/Release>"
			"${CMODEL_MONITOR_LIB_DIR}")
		target_link_libraries(cmodel PUBLIC SystemC vsiMathlib cmonitor_protocol.lib)
	else()
		if(CMAKE_BUILD_TYPE STREQUAL "Debug")
			add_subdirectory(${AQTOOLS}/GPU/perf/systemc-2.3.3 ${CMAKE_BINARY_DIR}/systemcLib)
			target_link_directories(cmodel INTERFACE "${CMODEL_MONITOR_LIB_DIR}/Debug")
			target_link_directories(cmodel INTERFACE "${CMODEL_MONITOR_LIB_DIR}")
			target_link_libraries(cmodel PUBLIC systemc vsiMathlib cmonitor_protocol.lib)
		else()
			target_link_directories(cmodel INTERFACE ${AQTOOLS}/GPU/perf/systemc-2.3.3/msvc10/SystemC/x64/Release)
			target_link_directories(cmodel INTERFACE "${CMODEL_MONITOR_LIB_DIR}/Release")
			target_link_libraries(cmodel PUBLIC SystemC vsiMathlib cmonitor_protocol.lib)
		endif()
	endif()
	if(CMODEL_CODE_COVERAGE AND MINGW)
		message(STATUS "==============set codecov===============")
		target_compile_options(cmodel PRIVATE -fprofile-arcs -ftest-coverage)
		target_link_options(cmodel PRIVATE -fprofile-arcs -ftest-coverage)
		target_link_libraries(cmodel PRIVATE gcov)
	endif()
	target_include_directories(cmodel PUBLIC ${ALL_INCLUDE_DIRECTORY})
	target_include_directories(cmodel PUBLIC ${AQTOOLS}/GPU/perf/nlohmannjson-3.12.0/include)
	target_include_directories(cmodel PUBLIC ${AQTOOLS}/GcTest/mathlib/templ)
	target_include_directories(cmodel PUBLIC ${AQTOOLS}/GcTest/mathlib)
	if(MINGW)
		set_target_properties(cmodel PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
		target_compile_options(cmodel PRIVATE -Wa,-mbig-obj)
	endif()
	# Use source_group to maintain original folder structure
	foreach(source_file ${ALL_SOURCES})
	get_filename_component(PARENT_DIR_SOURCE "${source_file}" DIRECTORY)
	file(RELATIVE_PATH RELATIVE_PATH "${CMAKE_CURRENT_SOURCE_DIR}" "${PARENT_DIR_SOURCE}")
	if(RELATIVE_PATH STREQUAL "")
		set(group_name_source "Source Files")
	else()
		string(REPLACE "/" "\\" group_name_source "${RELATIVE_PATH}")
	endif()
	source_group("${group_name_source}" FILES "${source_file}")
	#message(STATUS "Grouped "Grouped ${RELATIVE_PATH} -> ${group_name_source})
	endforeach()
	foreach(header_file ${ALL_HEADERS})
	get_filename_component(PARENT_DIR_HEADER "${header_file}" DIRECTORY)
	file(RELATIVE_PATH RELATIVE_PATH "${CMAKE_CURRENT_SOURCE_DIR}" "${PARENT_DIR_HEADER}")
	if(RELATIVE_PATH STREQUAL "")
		set(group_name_header "Header Files")
	else()
		string(REPLACE "/" "\\" group_name_header "${RELATIVE_PATH}")
	endif()
	source_group("${group_name_header}" FILES "${header_file}")
	#message(STATUS "Grouped "Grouped ${RELATIVE_PATH} -> ${group_name_header})
	endforeach()
elseif(UNIX)

	target_compile_options(cmodel PUBLIC -DAQ2_DMA=1 -DXAQ2=1 -DEMULATOR $<$<CONFIG:Debug>:-DUSE_PRINTF> $<$<CONFIG:Debug>:-D_DEBUG>
    -DvsidUSE_VSI_HEADERS=1)
	target_compile_definitions(cmodel PRIVATE PATH_TO_CMODEL="${AQARCH}/cmodel")
	if (MSVC)
		target_compile_options(cmodel PRIVATE ${WAVETRACE_CMODEL_FORCE_INCLUDE_MSVC}
											/FI${AQARCH}/cmodel/inc/vsiPrecomp.h
											/vmg -Zm300 /bigobj)
		set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-return-type -fPIC -g")
    else()
		set_target_properties(cmodel PROPERTIES CXX_STANDARD 14 CXX_STANDARD_REQUIRED ON)
		# vsiPrecomp.h is already supplied by target_precompile_headers().
		target_compile_options(cmodel PRIVATE ${WAVETRACE_CMODEL_FORCE_INCLUDE_CLANG})
		if(DV_DUMP)
			set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-return-type -fPIC -g")
		endif()
    endif()
	if (ENABLE_CMODEL_DENORM)
		target_compile_options(cmodel PUBLIC -DgcdCMODEL_DENORM_SUPPORT=1)
    endif()
	
	target_compile_options(cmodel PUBLIC -DLINUXEMULATOR -DLINUX)
	target_include_directories(cmodel PUBLIC ${ALL_INCLUDE_DIRECTORY})
	if (ENABLE_AMDL)
		if(BUILD_SYSTEMC)
			add_subdirectory(${AQTOOLS}/GPU/perf/systemc-2.3.3 ${CMAKE_BINARY_DIR}/systemcLib)
			target_link_libraries(cmodel PUBLIC systemc vsiMathlib)
		else()
			target_link_directories(cmodel INTERFACE ${AQTOOLS}/GPU/perf/systemc-2.3.3/lnxBuild/release/lib)
		endif()
	else()
		target_link_directories(cmodel INTERFACE ${AQTOOLS}/GPU/perf/systemc-2.3.3/lnxBuild/release_amd64/lib-linux64/)
	endif()
	if(BUILD_CMODEL_TEST_BENCH)
		target_compile_definitions(cmodel PRIVATE -DCMODEL_TEST_BENCH=1)
	endif()
	target_include_directories(cmodel PUBLIC ${AQTOOLS}/GPU/perf/nlohmannjson-3.12.0/include)
	target_include_directories(cmodel PUBLIC ${AQTOOLS}/GcTest/mathlib/templ)
	target_include_directories(cmodel PUBLIC ${AQTOOLS}/GcTest/mathlib)
	target_link_libraries(cmodel INTERFACE systemc vsiMathlib)
	
endif()

if(WAVETRACE_ENABLED)
	string(REPLACE "\\" "/" WAVETRACE_CMODEL_PATH "${AQARCH}/cmodel")
	separate_arguments(WAVETRACE_REFLECT_EXTRA_ARGS_LIST WINDOWS_COMMAND "${WAVETRACE_REFLECT_EXTRA_ARGS}")
	separate_arguments(WAVETRACE_REFLECT_EXTRA_CLANG_ARGS_LIST WINDOWS_COMMAND "${WAVETRACE_REFLECT_EXTRA_CLANG_ARGS}")

	set(WAVETRACE_REFLECT_CLANG_ARGS
		-std=c++14
		-ferror-limit=0
		-I${CMAKE_CURRENT_SOURCE_DIR}
		-I${AQARCH}/cmodel/inc
		-I${AQTOOLS}/GPU/perf/systemc-2.3.3/src
		-I${AQTOOLS}/GPU/perf/nlohmannjson-3.12.0/include
		-I${AQTOOLS}/GcTest/mathlib/templ
		-I${AQTOOLS}/GcTest/mathlib
		-I${WAVETRACE_ROOT}
		-I${WAVETRACE_ZSTD_INCLUDE_DIR}
		-resource-dir=${WAVETRACE_CLANG_RESOURCE_DIR}
		"-DANON(x)="
	)

	list(APPEND WAVETRACE_REFLECT_CLANG_ARGS
			-fms-compatibility
			-fms-extensions
			-fdelayed-template-parsing
			-D_MSVC
			-DWIN32
			-D_WIN32
			-D_WIN64
			-DAQ2_DMA=1
			-DXAQ2=1
			-DEMULATOR
			-DNOMINMAX
			-D_CRT_SECURE_NO_WARNINGS
			"-DPATH_TO_CMODEL=\"${WAVETRACE_CMODEL_PATH}\""
	)

	list(APPEND WAVETRACE_REFLECT_CLANG_ARGS
		"$<$<CONFIG:Debug>:-DUSE_PRINTF>"
		"$<$<CONFIG:Debug>:-D_DEBUG>"
	)
	if(DV_DUMP)
		list(APPEND WAVETRACE_REFLECT_CLANG_ARGS -DDV_DUMP=1)
	endif()
	if(ISAMODEL)
		list(APPEND WAVETRACE_REFLECT_CLANG_ARGS -DgcdMODELING_MODE=2)
	endif()
	if(ENABLE_CMODEL_DENORM)
		list(APPEND WAVETRACE_REFLECT_CLANG_ARGS -DgcdCMODEL_DENORM_SUPPORT=1)
	endif()
	if(BUILD_CMODEL_TEST_BENCH)
		list(APPEND WAVETRACE_REFLECT_CLANG_ARGS -DCMODEL_TEST_BENCH=1)
	endif()

	macro(_wavetrace_append_reflect_include _wavetrace_include_dir)
		if(NOT "${_wavetrace_include_dir}" STREQUAL "" AND NOT "${_wavetrace_include_dir}" MATCHES "\\$<")
			get_filename_component(_wavetrace_include_abs "${_wavetrace_include_dir}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
			file(TO_CMAKE_PATH "${_wavetrace_include_abs}" _wavetrace_include_abs)
			list(APPEND WAVETRACE_REFLECT_CLANG_ARGS "-I${_wavetrace_include_abs}")
		endif()
	endmacro()

	macro(_wavetrace_append_reflect_define _wavetrace_define)
		if(NOT "${_wavetrace_define}" STREQUAL "" AND NOT "${_wavetrace_define}" MATCHES "\\$<")
			string(REGEX REPLACE "^[-/][Dd]" "" _wavetrace_define_name "${_wavetrace_define}")
			list(APPEND WAVETRACE_REFLECT_CLANG_ARGS "-D${_wavetrace_define_name}")
		endif()
	endmacro()

	macro(_wavetrace_append_reflect_compile_options _wavetrace_options_var)
		set(_wavetrace_next_forced_include OFF)
		foreach(_wavetrace_opt IN LISTS ${_wavetrace_options_var})
			if("${_wavetrace_opt}" STREQUAL "" OR "${_wavetrace_opt}" MATCHES "\\$<")
				continue()
			endif()
			if(_wavetrace_next_forced_include)
				get_filename_component(_wavetrace_forced_include_abs "${_wavetrace_opt}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
				file(TO_CMAKE_PATH "${_wavetrace_forced_include_abs}" _wavetrace_forced_include_abs)
				list(APPEND WAVETRACE_REFLECT_CLANG_ARGS -include "${_wavetrace_forced_include_abs}")
				set(_wavetrace_next_forced_include OFF)
			elseif("${_wavetrace_opt}" STREQUAL "-include")
				set(_wavetrace_next_forced_include ON)
			elseif("${_wavetrace_opt}" MATCHES "^/FI(.+)$")
				get_filename_component(_wavetrace_forced_include_abs "${CMAKE_MATCH_1}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
				file(TO_CMAKE_PATH "${_wavetrace_forced_include_abs}" _wavetrace_forced_include_abs)
				list(APPEND WAVETRACE_REFLECT_CLANG_ARGS -include "${_wavetrace_forced_include_abs}")
			elseif("${_wavetrace_opt}" MATCHES "^[-/]I(.+)$")
				_wavetrace_append_reflect_include("${CMAKE_MATCH_1}")
			elseif("${_wavetrace_opt}" MATCHES "^[-/][Dd](.+)$")
				_wavetrace_append_reflect_define("${CMAKE_MATCH_1}")
			endif()
		endforeach()
	endmacro()

	foreach(_wavetrace_dir IN LISTS ALL_INCLUDE_DIRECTORY)
		_wavetrace_append_reflect_include("${_wavetrace_dir}")
	endforeach()
	foreach(_wavetrace_prop IN ITEMS INCLUDE_DIRECTORIES INTERFACE_INCLUDE_DIRECTORIES)
		get_target_property(_wavetrace_prop_values cmodel ${_wavetrace_prop})
		if(_wavetrace_prop_values)
			foreach(_wavetrace_dir IN LISTS _wavetrace_prop_values)
				_wavetrace_append_reflect_include("${_wavetrace_dir}")
			endforeach()
		endif()
	endforeach()
	foreach(_wavetrace_prop IN ITEMS COMPILE_DEFINITIONS INTERFACE_COMPILE_DEFINITIONS)
		get_target_property(_wavetrace_prop_values cmodel ${_wavetrace_prop})
		if(_wavetrace_prop_values)
			foreach(_wavetrace_def IN LISTS _wavetrace_prop_values)
				_wavetrace_append_reflect_define("${_wavetrace_def}")
			endforeach()
		endif()
	endforeach()
	foreach(_wavetrace_prop IN ITEMS COMPILE_OPTIONS INTERFACE_COMPILE_OPTIONS)
		get_target_property(_wavetrace_prop_values cmodel ${_wavetrace_prop})
		if(_wavetrace_prop_values)
			_wavetrace_append_reflect_compile_options(_wavetrace_prop_values)
		endif()
	endforeach()
	list(APPEND WAVETRACE_REFLECT_CLANG_ARGS ${WAVETRACE_REFLECT_EXTRA_CLANG_ARGS_LIST})

	# Keep the generated Visual Studio CustomBuild command below cmd.exe's
	# command-line limit.  Real cmodel projects can contribute hundreds of
	# include paths, so passing this list through -D would truncate the final
	# "-P run_reflectgen.cmake" and make cmake configure the working directory.
	set(WAVETRACE_REFLECT_CLANG_ARGS_FILE
		"${WAVETRACE_BUILD_TOP_DIR}/wavetrace_reflect_clang_args_$<CONFIG>.cmake")
	set(_wavetrace_reflect_clang_args_file_content
		"set(WAVETRACE_REFLECT_CLANG_ARGS)\n")
	foreach(_wavetrace_clang_arg IN LISTS WAVETRACE_REFLECT_CLANG_ARGS)
		string(APPEND _wavetrace_reflect_clang_args_file_content
			"list(APPEND WAVETRACE_REFLECT_CLANG_ARGS [==[${_wavetrace_clang_arg}]==])\n")
	endforeach()
	file(GENERATE
		OUTPUT "${WAVETRACE_REFLECT_CLANG_ARGS_FILE}"
		CONTENT "${_wavetrace_reflect_clang_args_file_content}")

	file(MAKE_DIRECTORY "${WAVETRACE_GEN_DIR}")
	set(WAVETRACE_REFLECT_TARGET_LIST "${WAVETRACE_GEN_DIR}/wavetrace_reflect_targets.txt")
	file(WRITE "${WAVETRACE_REFLECT_TARGET_LIST}" "")
	set(WAVETRACE_REFLECT_TARGET_HEADER_COUNT 0)
	set(WAVETRACE_REFLECT_ENTRY_HEADERS)
	if(NOT "${WAVETRACE_REFLECT_ENTRY_HEADER}" STREQUAL "")
		get_filename_component(_wavetrace_entry_abs "${WAVETRACE_REFLECT_ENTRY_HEADER}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
		if(NOT EXISTS "${_wavetrace_entry_abs}")
			message(FATAL_ERROR "WAVETRACE_REFLECT_ENTRY_HEADER does not exist: ${_wavetrace_entry_abs}")
		endif()
		list(APPEND WAVETRACE_REFLECT_ENTRY_HEADERS "${_wavetrace_entry_abs}")
	else()
		string(REGEX REPLACE "^.*::" "" _wavetrace_root_simple "${WAVETRACE_REFLECT_ROOT_CLASS}")
		foreach(_wavetrace_reflect_header IN LISTS ALL_HEADERS)
			if("${_wavetrace_reflect_header}" STREQUAL "" OR "${_wavetrace_reflect_header}" MATCHES "\\$<")
				continue()
			endif()
			get_filename_component(_wavetrace_reflect_header_abs "${_wavetrace_reflect_header}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
			if(NOT EXISTS "${_wavetrace_reflect_header_abs}")
				continue()
			endif()
			file(STRINGS "${_wavetrace_reflect_header_abs}" _wavetrace_root_hits
				REGEX "(^|[^A-Za-z0-9_])${_wavetrace_root_simple}([^A-Za-z0-9_]|$)"
				LIMIT_COUNT 1)
			if(_wavetrace_root_hits)
				list(APPEND WAVETRACE_REFLECT_ENTRY_HEADERS "${_wavetrace_reflect_header_abs}")
			endif()
		endforeach()
	endif()
	list(REMOVE_DUPLICATES WAVETRACE_REFLECT_ENTRY_HEADERS)
	if(NOT WAVETRACE_REFLECT_ENTRY_HEADERS)
		message(FATAL_ERROR "WaveTrace could not find a header mentioning root class '${WAVETRACE_REFLECT_ROOT_CLASS}' in ALL_HEADERS. Set WAVETRACE_REFLECT_ENTRY_HEADER to the header that defines it.")
	endif()
	foreach(_wavetrace_reflect_header_abs IN LISTS WAVETRACE_REFLECT_ENTRY_HEADERS)
		file(TO_CMAKE_PATH "${_wavetrace_reflect_header_abs}" _wavetrace_reflect_header_abs)
		file(APPEND "${WAVETRACE_REFLECT_TARGET_LIST}" "\"${_wavetrace_reflect_header_abs}\"\n")
		math(EXPR WAVETRACE_REFLECT_TARGET_HEADER_COUNT "${WAVETRACE_REFLECT_TARGET_HEADER_COUNT} + 1")
	endforeach()
	message(STATUS "WaveTrace ReflectGen entry headers: ${WAVETRACE_REFLECT_TARGET_HEADER_COUNT} -> ${WAVETRACE_REFLECT_TARGET_LIST}")
	foreach(_wavetrace_entry_header IN LISTS WAVETRACE_REFLECT_ENTRY_HEADERS)
		message(STATUS "  WaveTrace entry header: ${_wavetrace_entry_header}")
	endforeach()
	message(STATUS "WaveTrace reflection compile shards: ${WAVETRACE_REFLECT_COMPILE_SHARDS}")

	# The aggregate header alone is not enough to decide whether reflection is
	# current: changing the compile-shard count also changes a set of generated
	# .cpp/.h byproducts.  Keep a content-stable settings file so both CMake's
	# dependency graph and the runner's incremental check observe such changes.
	set(WAVETRACE_REFLECT_SETTINGS_FILE
		"${WAVETRACE_BUILD_TOP_DIR}/wavetrace_reflect_generation.settings")
	set(_wavetrace_reflect_settings_content
		"schema=1\ncompile_shards=${WAVETRACE_REFLECT_COMPILE_SHARDS}\nroot_class=${WAVETRACE_REFLECT_ROOT_CLASS}\nentry_header=${WAVETRACE_REFLECT_ENTRY_HEADER}\nentry_headers=${WAVETRACE_REFLECT_ENTRY_HEADERS}\nextra_args=${WAVETRACE_REFLECT_EXTRA_ARGS}\nextra_clang_args=${WAVETRACE_REFLECT_EXTRA_CLANG_ARGS}\nreflectgen=${WAVETRACE_REFLECTGEN_EXE}\nconfig=${WAVETRACE_CONFIG_FILE}\n")
	file(GENERATE
		OUTPUT "${WAVETRACE_REFLECT_SETTINGS_FILE}"
		CONTENT "${_wavetrace_reflect_settings_content}")

	set(WAVETRACE_REFLECT_DEPENDS ${WAVETRACE_REFLECTGEN_DEPENDS} "${WAVETRACE_REFLECT_TARGET_LIST}" ${ALL_HEADERS})
	set(WAVETRACE_REFLECT_INPUT_LIST "${WAVETRACE_BUILD_TOP_DIR}/wavetrace_reflect_inputs.txt")
	file(WRITE "${WAVETRACE_REFLECT_INPUT_LIST}" "")
	foreach(_wavetrace_reflect_input IN LISTS WAVETRACE_REFLECT_DEPENDS)
		if(NOT "${_wavetrace_reflect_input}" STREQUAL "" AND NOT "${_wavetrace_reflect_input}" MATCHES "\\$<")
			get_filename_component(_wavetrace_reflect_input_abs "${_wavetrace_reflect_input}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
			file(TO_CMAKE_PATH "${_wavetrace_reflect_input_abs}" _wavetrace_reflect_input_abs)
			file(APPEND "${WAVETRACE_REFLECT_INPUT_LIST}" "${_wavetrace_reflect_input_abs}\n")
		endif()
	endforeach()
	list(APPEND WAVETRACE_REFLECT_DEPENDS "${WAVETRACE_REFLECT_INPUT_LIST}")
	list(APPEND WAVETRACE_REFLECT_DEPENDS "${WAVETRACE_CONFIG_FILE}")
	list(APPEND WAVETRACE_REFLECT_DEPENDS "${WAVETRACE_REFLECT_CLANG_ARGS_FILE}")

	set(WAVETRACE_RUN_REFLECTGEN_SCRIPT "${WAVETRACE_ROOT}/cmake/run_reflectgen.cmake")
	if(NOT EXISTS "${WAVETRACE_RUN_REFLECTGEN_SCRIPT}")
		message(FATAL_ERROR "WaveTrace ReflectGen runner script was not found: ${WAVETRACE_RUN_REFLECTGEN_SCRIPT}")
	endif()
	list(APPEND WAVETRACE_REFLECT_DEPENDS
		"${WAVETRACE_RUN_REFLECTGEN_SCRIPT}"
		"${WAVETRACE_REFLECT_SETTINGS_FILE}")

	set(WAVETRACE_REFLECT_SHARD_SOURCES)
	set(WAVETRACE_REFLECT_SHARD_HEADERS)
	set(WAVETRACE_REFLECT_SHARD_BYPRODUCTS)
	if(WAVETRACE_REFLECT_COMPILE_SHARDS GREATER 0)
		math(EXPR _wavetrace_last_shard "${WAVETRACE_REFLECT_COMPILE_SHARDS} - 1")
		foreach(_wavetrace_shard_index RANGE 0 ${_wavetrace_last_shard})
			if(_wavetrace_shard_index LESS 10)
				set(_wavetrace_shard_tag "0${_wavetrace_shard_index}")
			else()
				set(_wavetrace_shard_tag "${_wavetrace_shard_index}")
			endif()
			set(_wavetrace_shard_base "${WAVETRACE_GEN_DIR}/root_class_closure_shard_${_wavetrace_shard_tag}")
			list(APPEND WAVETRACE_REFLECT_SHARD_SOURCES "${_wavetrace_shard_base}.cpp")
			list(APPEND WAVETRACE_REFLECT_SHARD_HEADERS "${_wavetrace_shard_base}_reflect_auto.h")
			list(APPEND WAVETRACE_REFLECT_SHARD_BYPRODUCTS
				"${_wavetrace_shard_base}.cpp"
				"${_wavetrace_shard_base}_input.h"
				"${_wavetrace_shard_base}_reflect_auto.h")
		endforeach()
		list(APPEND WAVETRACE_REFLECT_SHARD_BYPRODUCTS
			"${WAVETRACE_GEN_DIR}/root_class_closure_root_input.h"
			"${WAVETRACE_GEN_DIR}/root_class_closure_shards_registry.h")
	endif()

	add_custom_command(
		OUTPUT "${WAVETRACE_GEN_DIR}/project_reflect_auto.h"
		BYPRODUCTS
			"${WAVETRACE_GEN_DIR}/root_class_closure_reflect_auto.h"
			${WAVETRACE_REFLECT_SHARD_BYPRODUCTS}
		COMMAND ${CMAKE_COMMAND}
			"-DWAVETRACE_REFLECTGEN_EXE=${WAVETRACE_REFLECTGEN_EXE}"
			"-DWAVETRACE_REFLECT_BATCH_DIR=${WAVETRACE_REFLECT_BATCH_DIR}"
			"-DWAVETRACE_REFLECT_TARGET_LIST=${WAVETRACE_REFLECT_TARGET_LIST}"
			"-DWAVETRACE_REFLECT_INPUT_LIST=${WAVETRACE_REFLECT_INPUT_LIST}"
			"-DWAVETRACE_REFLECT_ROOT_CLASS=${WAVETRACE_REFLECT_ROOT_CLASS}"
			"-DWAVETRACE_REFLECT_COMPILE_SHARDS=${WAVETRACE_REFLECT_COMPILE_SHARDS}"
			"-DWAVETRACE_REFLECT_EXTRA_ARGS=${WAVETRACE_REFLECT_EXTRA_ARGS_LIST}"
			"-DWAVETRACE_REFLECT_OUTPUT_DIR=${WAVETRACE_GEN_DIR}"
			"-DWAVETRACE_REFLECT_AGGREGATE_HEADER=project_reflect_auto.h"
			"-DWAVETRACE_REFLECT_LOG_FILE=${WAVETRACE_GEN_DIR}/reflectgen.log"
			"-DWAVETRACE_CONFIG_FILE=${WAVETRACE_CONFIG_FILE}"
			"-DWAVETRACE_REFLECT_CLANG_ARGS_FILE=${WAVETRACE_REFLECT_CLANG_ARGS_FILE}"
			"-DWAVETRACE_REFLECT_SETTINGS_FILE=${WAVETRACE_REFLECT_SETTINGS_FILE}"
			"-DWAVETRACE_REFLECT_VERBOSE=${WAVETRACE_REFLECT_VERBOSE}"
			-P "${WAVETRACE_RUN_REFLECTGEN_SCRIPT}"
		DEPENDS ${WAVETRACE_REFLECT_DEPENDS}
		VERBATIM
	)
	# project_reflect_auto.h is added directly to cmodel below.  Attaching the
	# custom-command output to the real target keeps generation incremental while
	# avoiding a separate wavetrace_generate_reflection project in VS solutions.

	target_include_directories(cmodel PUBLIC
		"${WAVETRACE_ROOT}"
		"${WAVETRACE_GEN_DIR}"
		"${WAVETRACE_ZSTD_INCLUDE_DIR}"
	)
	target_compile_definitions(cmodel PUBLIC
		WAVE_RUNTIME_AVAILABLE=1
		"WAVETRACE_CONFIG_PATH=\"${WAVETRACE_CONFIG_FILE}\""
		NOMINMAX
	)
	if(WAVETRACE_REFLECT_COMPILE_SHARDS GREATER 0)
		target_compile_definitions(cmodel PUBLIC WAVETRACE_COMPILED_SHARDS=1)
	endif()
	if(WIN32)
		add_library(WaveTrace::zstd STATIC IMPORTED)
		set_target_properties(WaveTrace::zstd PROPERTIES
			IMPORTED_LOCATION_DEBUG "${WAVETRACE_ZSTD_LIB_DEBUG}"
			IMPORTED_LOCATION_RELEASE "${WAVETRACE_ZSTD_LIB_RELEASE}"
			IMPORTED_LOCATION_RELWITHDEBINFO "${WAVETRACE_ZSTD_LIB_RELEASE}"
			IMPORTED_LOCATION_MINSIZEREL "${WAVETRACE_ZSTD_LIB_RELEASE}"
			IMPORTED_LOCATION_PROFILE "${WAVETRACE_ZSTD_LIB_RELEASE}"
			INTERFACE_INCLUDE_DIRECTORIES "${WAVETRACE_ZSTD_INCLUDE_DIR}"
		)
		target_link_libraries(cmodel PUBLIC WaveTrace::zstd)
	endif()
	include("${WAVETRACE_ROOT}/cmake/wavetrace_writer_helper.cmake")
	wavetrace_target_needs_writer_helper(cmodel)
	# Keep WaveTrace package headers visible under cmodel's WaveTracer filter.
	# This is IDE organization only; it does not create a separate VS project.
	file(GLOB WAVETRACE_IDE_HEADERS CONFIGURE_DEPENDS "${WAVETRACE_ROOT}/*.h")
	list(SORT WAVETRACE_IDE_HEADERS)
	set(WAVETRACE_GENERATED_HEADERS
		"${WAVETRACE_GEN_DIR}/project_reflect_auto.h"
		"${WAVETRACE_GEN_DIR}/root_class_closure_reflect_auto.h"
	)
	if(WAVETRACE_REFLECT_COMPILE_SHARDS GREATER 0)
		list(APPEND WAVETRACE_GENERATED_HEADERS
			"${WAVETRACE_GEN_DIR}/root_class_closure_shards_registry.h"
			${WAVETRACE_REFLECT_SHARD_HEADERS})
	endif()
	set_source_files_properties(${WAVETRACE_GENERATED_HEADERS} PROPERTIES GENERATED TRUE HEADER_FILE_ONLY TRUE)
	if(WAVETRACE_REFLECT_SHARD_SOURCES)
		set_source_files_properties(${WAVETRACE_REFLECT_SHARD_SOURCES} PROPERTIES GENERATED TRUE)
	endif()
	if(MSVC AND WAVETRACE_REFLECT_SHARD_SOURCES)
		set_source_files_properties(${WAVETRACE_REFLECT_SHARD_SOURCES} PROPERTIES COMPILE_OPTIONS "/cgthreads8")
	endif()
	if(WAVETRACE_IDE_HEADERS)
		set_source_files_properties(${WAVETRACE_IDE_HEADERS} PROPERTIES HEADER_FILE_ONLY TRUE)
		source_group("WaveTracer" FILES ${WAVETRACE_IDE_HEADERS})
	endif()
	source_group("WaveTracer\\Generated" FILES ${WAVETRACE_GENERATED_HEADERS})
	source_group("WaveTracer\\Generated\\CompileShards" FILES ${WAVETRACE_REFLECT_SHARD_SOURCES})
	target_sources(cmodel PRIVATE ${WAVETRACE_IDE_HEADERS} ${WAVETRACE_GENERATED_HEADERS} ${WAVETRACE_REFLECT_SHARD_SOURCES})
endif()
