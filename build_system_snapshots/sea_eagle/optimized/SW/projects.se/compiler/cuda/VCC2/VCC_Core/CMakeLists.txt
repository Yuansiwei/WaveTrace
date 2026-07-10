cmake_minimum_required(VERSION 3.15)
set(TARGET_NAME vcc-core)
project(${TARGET_NAME} VERSION 1.0 LANGUAGES CXX)

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

include_directories(${AQROOT}/compiler/cuda/VCC2/VCC_Core/include
	${AQROOT}/driver/cuda/sdk/inc
	${AQROOT}/hal/inc/shared)

set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON)

add_library(${TARGET_NAME} SHARED
  src/drvi/vcc_decoder.cpp
  include/drvi/vcc_core.h
  include/drvi/VivanteGPUKEP.h
)

target_include_directories(${TARGET_NAME} PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E  copy ${AQROOT}/compiler/cuda/VCC2/sdk/VivanteGPUKernelLaunch.dll ${AQROOT}/build/sdk/bin/VivanteGPUKernelLaunch.dll
	COMMAND ${CMAKE_COMMAND} -E  copy ${AQROOT}/compiler/cuda/VCC2/sdk/VivanteGPUKernelLaunch.pdb ${AQROOT}/build/sdk/bin/VivanteGPUKernelLaunch.pdb
	COMMENT "Copy DLL files"
	VERBATIM
)
