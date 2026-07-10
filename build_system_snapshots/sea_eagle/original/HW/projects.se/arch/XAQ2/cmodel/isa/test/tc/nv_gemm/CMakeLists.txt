cmake_minimum_required(VERSION 3.18)

project(nv_gemm LANGUAGES CXX CUDA)



set(CMAKE_CXX_STANDARD 14)

set(CMAKE_CUDA_STANDARD 14)



# set the target arch

if(SM_120a)

    # set(CMAKE_CUDA_FLAGS "${CMAKE_CUDA_FLAGS} -gencode arch=compute_120a,code=sm_120a")

    set(CMAKE_CUDA_ARCHITECTURES 120a)

    message(STATUS " (log) sm_120a")

else()

    # set(CMAKE_CUDA_FLAGS "${CMAKE_CUDA_FLAGS} -gencode arch=compute_89,code=sm_89")

    set(CMAKE_CUDA_ARCHITECTURES 89)

    message(STATUS " (log) sm_89")

endif()



# look for the cuda toolkit

find_package(CUDAToolkit REQUIRED)

include(CheckLanguage)

if( NOT CMAKE_CUDA_COMPILER)

    message(FATAL_ERROR "CUDA not found!")

endif()



set(COMMON_DIR ../common)

set(CUDA_KER_DIR ../cuda_ker)

set(CUDA_UTIL_DIR ../cuda_util)

set(INC_DIR

    ${COMMON_DIR}

    ${CUDA_KER_DIR}

    ${CUDA_UTIL_DIR}

)

message(STATUS " (log) project path: ${PROJECT_SOURCE_DIR}")

message(STATUS " (log) inc path: ${INC_DIR}")



include_directories(${INC_DIR})



set(CUDA_SRC_FILES

    ${CUDA_UTIL_DIR}/nv_tc_util.cu

    ${CUDA_KER_DIR}/ker_mma.cu

	./matmul.cu

)

message(STATUS " (log) cuda src files: ${SRC_FILES}")



# set main file

set(MAIN_FILE main.cpp)

message(STATUS " (log) main file: ${MAIN_FILE}")



set(EXECUTABLE_OUTPUT_PATH ${PROJECT_BINARY_DIR})

message(STATUS " (log) bin path: ${EXECUTABLE_OUTPUT_PATH}")



set(EXE_NAME test)



add_executable(${EXE_NAME}

    ${MAIN_FILE}

    ${CUDA_SRC_FILES})



target_include_directories(${EXE_NAME} PRIVATE

    ${INC_DIR}

    ${CMAKE_CUDA_TOOLKIT_INCLUDE_DIRECTORIES}

)



target_link_libraries(${EXE_NAME} PRIVATE

    CUDA::cudart

    CUDA::cublasLt

)



if(SM_120a)

    target_compile_definitions(test PRIVATE SM_120a=${SM_120a})

endif()

if(ENABLE_fPRINT_VERIFY)

    target_compile_definitions(test PRIVATE ENABLE_fPRINT_VERIFY=${ENABLE_fPRINT_VERIFY})

endif()



# if(SM_120a)

#     target_compile_options(test PRIVATE

#     $<$<AND:$<COMPILE_LANGUAGE:CUDA>,$<CONFIG:Release>>:

#         -O3                # Release mode, enable the maximum optimization.

#         -gencode=arch=compute_120a,code=sm_120a

#         -lineinfo

#         --expt-relaxed-constexpr

#     >

#     $<$<AND:$<COMPILE_LANGUAGE:CUDA>,$<CONFIG:Debug>>:

#         -O0                # Debug mode, disable the optimization.

#         -g                 # generate the debug info.

#         -gencode=arch=compute_120a,code=sm_120a

#         -lineinfo

#         --expt-relaxed-constexpr

#     >

#     )

# else()

#     target_compile_options(test PRIVATE

#     $<$<AND:$<COMPILE_LANGUAGE:CUDA>,$<CONFIG:Release>>:

#         -O3                # Release mode, enable the maximum optimization.

#         -gencode=arch=compute_89,code=sm_89

#         -lineinfo

#         --expt-relaxed-constexpr

#     >

#     $<$<AND:$<COMPILE_LANGUAGE:CUDA>,$<CONFIG:Debug>>:

#         -O0                # Debug mode, disable the optimization.

#         -g                 # generate the debug info.

#         -gencode=arch=compute_89,code=sm_89

#         -lineinfo

#         --expt-relaxed-constexpr

#     >

#     )

# endif()

