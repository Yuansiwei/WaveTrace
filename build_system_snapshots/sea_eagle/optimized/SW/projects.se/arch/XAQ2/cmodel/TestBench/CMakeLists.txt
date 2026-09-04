cmake_minimum_required(VERSION 3.18)
project(testbench VERSION 1.0.0 LANGUAGES CXX)
set(ALL_SOURCES_TESTBENCH)
set(ALL_HEADERS_TESTBENCH)
set(ALL_INCLUDE_DIRECTORY_TESTBENCH)
set(CMAKE_CXX_STANDARD 14)
set(TESTBENCH_SOURCES_GL_ONLY
    CACHE FILEPATH "CPP FILE"
)
set(TESTBENCH_SOURCES_COMMON
    ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp
    CACHE FILEPATH "CPP FILE"
)

add_subdirectory(l1lstx)
add_subdirectory(PPU)
add_subdirectory(QPPU)
add_subdirectory(DPPU)
add_subdirectory(cluster)
add_subdirectory(cache)
add_subdirectory(UMMA)
add_subdirectory(TAC)
message(STATUS "testbench sources: ${ALL_SOURCES_TESTBENCH}")
if(TESTBENCH_SOURCES_COMMON)
   list(APPEND ALL_SOURCES_TESTBENCH ${TESTBENCH_SOURCES_COMMON})
endif()
if(NOT GPGPU_ONLY AND TESTBENCH_SOURCES_GL_ONLY)
   list(APPEND ALL_SOURCES_TESTBENCH ${TESTBENCH_SOURCES_GL_ONLY})
endif()
if(TESTBENCH_HEADERS_COMMON)
   list(APPEND ALL_HEADERS_TESTBENCH ${TESTBENCH_HEADERS_COMMON})
endif()
if(NOT GPGPU_ONLY AND TESTBENCH_HEADERS_GL_ONLY)
   list(APPEND ALL_HEADERS_TESTBENCH ${TESTBENCH_HEADERS_GL_ONLY})
endif()
list(APPEND ALL_HEADERS_TESTBENCH ${TESTBENCH_HEADERS})
list(APPEND ALL_INCLUDE_DIRECTORY_TESTBENCH ${CMAKE_CURRENT_SOURCE_DIR}/)
message(STATUS "testbench sources: ${ALL_SOURCES_TESTBENCH}")
add_executable(testbench ${ALL_SOURCES_TESTBENCH} ${ALL_HEADERS_TESTBENCH})
if(WIN32 AND MINGW)
	set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-function-sections -fno-data-sections")
endif()
target_include_directories(testbench PRIVATE ${AQARCH}/cmodel/L2L3C/glx/src/reg
											${AQARCH}/cmodel/L2L3C/glx/src/func
											${AQARCH}/cmodel/L2L3C/glx/src
											${AQARCH}/cmodel/L2L3C/glx/src_mmu/l2tlb
											${AQARCH}/cmodel/L2L3C/glx/src_mmu/l1tlb
											${AQARCH}/cmodel/L2L3C/glx/src_mmu/mmu
											${AQARCH}/cmodel/L2L3C/glx/src_mmu
											${AQARCH}/cmodel/L2L3C/glx/vsi_deps
											${AQARCH}/cmodel/L2L3C/glx/fake_ddr
											${AQARCH}/cmodel/L2L3C/glx/import/cm_essence/src/common
											${AQARCH}/cmodel/L2L3C/glx/import/cm_essence/src/utils
											${AQARCH}/cmodel/L2L3C/glx/compress_wrapper/src
											${AQARCH}/cmodel/memory/compression
											${AQARCH}/cmodel
											${AQARCH}/cmodel/inc
											${AQROOT}/arch/XAQ2/cmodel/inc
											${AQROOT}/TOOLS/GPU/perf/systemc-2.3.3/src
											${AQARCH}/cmodel/shaderArch
											${AQROOT}/TOOLS/GcTest/mathlib
											${AQTOOLS}/bin
											${AQARCH}/cmodel/isa/src
											${AQARCH}/cmodel/isa
											${AQTOOLS}/GPU/perf/googletest-1.16.0/googletest/include
											${AQARCH}/cmodel/QPPU
											${AQROOT}/arch/XAQ2/cmodel/PPU/L1LSTX
											${CMAKE_CURRENT_SOURCE_DIR}/)
if(WIN32)
	foreach(source_file ${ALL_SOURCES_TESTBENCH})
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
	foreach(header_file ${ALL_HEADERS_TESTBENCH})
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
endif()
if(WIN32)
	if(MSVC)
		add_compile_options(/Zf)
		set_target_properties(testbench PROPERTIES
			VS_DEBUGGER_COMMAND_ARGUMENTS "--gtest_filter=*"
		)
	# Custom All_Debug flags
	set(CMAKE_CXX_FLAGS " -O1 -DDEBUG /vmg -Zm300 /bigobj /MP4")
	
	# Custom All_Release flags
	set(CMAKE_CXX_FLAGS "-O2 -DNDEBUG /vmg -Zm300 /bigobj /MP4")
	endif()
	if(MINGW)
		set_target_properties(testbench PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
		target_compile_options(testbench PRIVATE -Wa,-mbig-obj)
	endif()

	if(CMODEL_CODE_COVERAGE)
		if(MINGW)
			message(STATUS "==============set codecov===============")
			target_compile_options(cmodel PRIVATE -fprofile-arcs -ftest-coverage)
			target_link_options(cmodel PRIVATE -fprofile-arcs -ftest-coverage)
			target_link_libraries(cmodel PRIVATE gcov)
		endif()
	endif()
endif()
