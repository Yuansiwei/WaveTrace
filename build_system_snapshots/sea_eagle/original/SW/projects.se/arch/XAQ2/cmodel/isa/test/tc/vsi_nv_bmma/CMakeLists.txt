
cmake_minimum_required(VERSION 3.16)

project(VsiNvBMma LANGUAGES CUDA CXX)

set(CMAKE_CXX_STANDARD 14)

# set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(SM_120a)
    set(CMAKE_CUDA_FLAGS "${CMAKE_CUDA_FLAGS} -gencode arch=compute_120a,code=sm_120a")
    #set(CMAKE_CUDA_ARCHITECTURES 120a)
    message(STATUS " (log) sm_120a")
else()
    set(CMAKE_CUDA_FLAGS "${CMAKE_CUDA_FLAGS} -gencode arch=compute_89,code=sm_89")
    #set(CMAKE_CUDA_ARCHITECTURES 89)
    message(STATUS " (log) sm_89")
endif()
include(CheckLanguage)
if( NOT CMAKE_CUDA_COMPILER)
    message(FATAL_ERROR "CUDA not found!")
endif()


add_definitions(-DINIT_MEMORY_METHOD=1)

set(COMMON_DIR ../common)
set(ISA_DIR ../../../src)
set(MATHLIB_DIR ../../../third_party/mathlib/mathlib)
set(CUDA_UTIL_DIR ../cuda_util)
set(CUDA_KER_DIR ../cuda_ker)

set(INC_DIR ${COMMON_DIR} ${ISA_DIR} ${MATHLIB_DIR} ${CUDA_UTIL_DIR} ${CUDA_KER_DIR})

message(STATUS " (log) project path: ${PROJECT_SOURCE_DIR}")
message(STATUS " (log) include path: ${INC_DIR}")

include_directories(${INC_DIR})

set(COMMON_SRC_FILES 
    ${COMMON_DIR}/vsi_tc_util.cpp
    ${CUDA_UTIL_DIR}/nv_tc_util.cu
    ${CUDA_KER_DIR}/ker_mma.cu
)
message(STATUS " (log) common source files: ${COMMON_SRC_FILES}")

set(ISA_SRC_FILES 
    ${ISA_DIR}/bitmanip.cpp
    ${ISA_DIR}/decoder.cpp
    ${ISA_DIR}/exec_context.cpp
    ${ISA_DIR}/instr_impl.cpp
    ${ISA_DIR}/instr_tc.cpp
    ${ISA_DIR}/op_encodings.cpp
    ${ISA_DIR}/serialize.cpp
)
message(STATUS " (log) isa source files: ${ISA_SRC_FILES}")

set(MATHLIB_SRC_FILES 
    ${MATHLIB_DIR}/vsiBitField.cpp
    ${MATHLIB_DIR}/vsiMathFixed.cpp
    ${MATHLIB_DIR}/vsiMathFloat.cpp
    ${MATHLIB_DIR}/vsiMathMultiType.cpp
	${MATHLIB_DIR}/vsiMathTrans.cpp
)
message(STATUS " (log) mathlib source files: ${MATHLIB_SRC_FILES}")


# create the static library.
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR})
message(STATUS " (log) lib file: ${CMAKE_ARCHIVE_OUTPUT_DIRECTORY}")
add_library(isa STATIC ${ISA_SRC_FILES})
add_library(mathlib STATIC ${MATHLIB_SRC_FILES})


# set main file
set(MAIN_FILE verify_bmma_nv.cu)
message(STATUS " (log) main file: ${MAIN_FILE}")

set(EXECUTABLE_OUTPUT_PATH ${PROJECT_BINARY_DIR})
message(STATUS " (log) bin path: ${EXECUTABLE_OUTPUT_PATH}")

link_directories(${CMAKE_ARCHIVE_OUTPUT_DIRECTORY})
link_libraries(isa mathlib)
add_executable(test ${MAIN_FILE} ${COMMON_SRC_FILES})

if(SM_120a)
    target_compile_definitions(test PRIVATE SM_120a=${SM_120a})
endif()
if(ENABLE_fPRINT_VERIFY)
    target_compile_definitions(test PRIVATE ENABLE_fPRINT_VERIFY=${ENABLE_fPRINT_VERIFY})
endif()
if(ENABLE_fPRINT_VSI)
    target_compile_definitions(test PRIVATE ENABLE_fPRINT_VSI=${ENABLE_fPRINT_VSI})
endif()


# if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
#     if(CMAKE_CXX_COMPILER_ID MATCHES "MSVC")
#         target_compile_options(test PRIVATE
#             /W4
#         )
#     elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
#         target_compile_options(test PRIVATE
#             -Wall
#             -Wextra
#             -Wpedantic
#             -Werror
#             -Wshadow
#             -Wconversion
#         )
#     endif()
# else()
#     if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
#         target_compile_options(test PRIVATE
#             -Wall
#             -Wextra
#             -Wpedantic
#             -Werror
#             -Wshadow
#             -Wconversion
#             -Wsign-conversion
#             -Wold-style-cast
#         )
#     endif()
# endif()

