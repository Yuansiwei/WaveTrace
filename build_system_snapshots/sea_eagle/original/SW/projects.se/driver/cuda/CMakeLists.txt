cmake_minimum_required(VERSION 3.28)

project(gpgpu)

option(BUILD_CMODEL             "build cmodel "   ON )
option(BUILD_CMODEL_TEST_BENCH  "build cmodel testbench" OFF)
option(CMODEL_CODE_COVERAGE "cmodel code coverage support" OFF)
option(BUILD_LINUX              "build linux "    OFF)
option(BUILD_VCC                "build vcc "      ON )
option(BUILD_VRTC               "build vrtc "     ON)
option(BUILD_VLINK              "build vlink "    ON )
option(BUILD_SPVCOMPILER        "build spvCompiler " ON )
option(BUILD_VFATBIN            "build vfatbin "  ON )
option(BUILD_TESTS              "build tests "    OFF)
option(BUILD_CUOBJDUMP          "build cuObjdump" OFF)
option(BUILD_VSIDISASM          "build vsidisasm" OFF)
option(BUILD_CUFILT             "build cu++filt"  OFF)
option(BUILD_VSIPRUNE           "build vsiprune"  OFF)
option(BUILD_DRM_KMD            "build drm_kmd"  OFF)
option(ENABLE_TIDY_CHECK        "clang tidy check"        OFF)
option(ENABLE_DUMMY_LIBS        "build dummy cuda libs"    ON)
option(ENABLE_CCACHE            "enable CCACHE to reduce compile time" OFF)
option(ENABLE_DISTCC            "enable distcc to reduce compile time" OFF)
option(ENABLE_CMODEL_DENORM     "enabel cmodel denorm support" OFF)


set(VCC_VERSION  "2"    CACHE   STRING  "Cmake Linker Version")

set(AQROOT ${CMAKE_CURRENT_SOURCE_DIR}/../.. CACHE PATH "AQROOT")
if (NOT AQARCH)
    set(AQARCH ${AQROOT}/arch/XAQ2 CACHE PATH "AQARCH")
endif()
if (NOT AQTOOLS)
    set(AQTOOLS ${AQROOT}/tools CACHE STRING "AQTOOLS")
endif()

set(BUILD_CUDA  "124"    CACHE   STRING  "CUDA SDK Version")
set_property(CACHE BUILD_CUDA PROPERTY STRINGS "90;120;123;124")

if (NOT FIXED_ARCH_TYPE)
    set(FIXED_ARCH_TYPE $ENV{FIXED_ARCH_TYPE})
endif()

# CMODEL            : libEmulator.so + cmodel.a on HOST
# QEMU              : cmodel.a + cmodel wrapper + KMD on HOST
# HOST              : HOST KMD
set(PLATFORM_TYPE  "CMODEL"    CACHE   STRING  "cuda driver platform")
set_property(CACHE PLATFORM_TYPE PROPERTY STRINGS "CMODEL;QEMU;HOST")

# BUILD_CMODEL      cmodel.a
# BUILD_EMULATOR    libemulator.so
# BUILD_LINUX       linux KMD
# BUILD_WINDOWS     window KMD
# BUILD_QEMU        QEMU env
if ("x${PLATFORM_TYPE}" STREQUAL "xCMODEL")
    set(BUILD_CMODEL            ON)
    set(BUILD_EMULATOR          ON)
    set(BUILD_LINUX             OFF)
    set(BUILD_WINDOWS           OFF)
    set(BUILD_QEMU              OFF)
elseif ("x${PLATFORM_TYPE}" STREQUAL "xQEMU")
    if (UNIX)
        set(BUILD_CMODEL        ON)
        set(BUILD_EMULATOR      OFF)
        set(BUILD_LINUX         ON)
        set(BUILD_WINDOWS       OFF)
        set(BUILD_QEMU          ON)
    elseif(WIN32)
        set(BUILD_CMODEL        ON)
        set(BUILD_EMULATOR      OFF)
        set(BUILD_LINUX         OFF)
        set(BUILD_WINDOWS       ON)
        set(BUILD_QEMU          ON)
    else()
        message(FATAL_ERROR "not support platform")
    endif()
elseif ("x${PLATFORM_TYPE}" STREQUAL "xHOST")
    if (UNIX)
        set(BUILD_CMODEL        OFF)
        set(BUILD_EMULATOR      OFF)
        set(BUILD_LINUX         ON)
        set(BUILD_WINDOWS       OFF)
        set(BUILD_QEMU          OFF)
    elseif(WIN32)
        set(BUILD_CMODEL        OFF)
        set(BUILD_EMULATOR      OFF)
        set(BUILD_LINUX         OFF)
        set(BUILD_WINDOWS       ON)
        set(BUILD_QEMU          OFF)
    else()
        message(FATAL_ERROR "not support platform")
    endif()
else()
    message(FATAL_ERROR "unknown build type for cuda driver platform")
endif()


function(check_and_set_var VAR_NAME type)
    if(NOT DEFINED ${VAR_NAME})
        if(DEFINED ENV{${VAR_NAME}})
            set(${VAR_NAME} "$ENV{${VAR_NAME}}")
            cmake_path(CONVERT "${${VAR_NAME}}" TO_CMAKE_PATH_LIST ${VAR_NAME} NORMALIZE)
            message(STATUS "Variable ${VAR_NAME} is not set, using environment variable: $ENV{${VAR_NAME}}")
        else()
            message(FATAL_ERROR "Variable ${VAR_NAME} is not set and no environment variable found.")
        endif()
    else()
        message(STATUS "Set ${VAR_NAME} as environment varibale: ${${VAR_NAME}}")
        cmake_path(CONVERT "${${VAR_NAME}}" TO_CMAKE_PATH_LIST ${VAR_NAME} NORMALIZE)
        set(ENV{${VAR_NAME}} ${${VAR_NAME}})
    endif()
    set(${VAR_NAME} "${${VAR_NAME}}" CACHE ${type} "" FORCE)
endfunction()

check_and_set_var(AQROOT    PATH)
check_and_set_var(AQTOOLS   PATH)
if (NOT FIXED_ARCH_TYPE)
    check_and_set_var(AQARCH    PATH)
    if (BUILD_CMODEL)
        check_and_set_var(GCDEFINE  STRING)
    endif()
endif()

set(CMAKE_CODELITE_USE_TARGETS  ON)
set(CMAKE_CXX_STANDARD          17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD            11)
set(CMAKE_C_STANDARD_REQUIRED   ON)
set(CMAKE_INSTALL_PREFIX        "${CMAKE_BINARY_DIR}/install")

set(HAL_TARGET_NAME             gpgpuHAL)
set(DRV_TARGET_NAME             vsiGPGPU)
set(RT_TARGET_NAME              vsiGPGPURT)
set(COMPILER_TARGET_NAME        compiler)

# github.com/ccache/ccache/wiki/MS-Visual-Studio
macro(VisualStudio_CCACHE ccache)
    file(COPY_FILE ${ccache} ${CMAKE_BINARY_DIR}/cl.exe ONLY_IF_DIFFERENT)

    # By default Visual Studio generators will use /Zi which is not compatible
    # with ccache, so tell Visual Studio to use /Z7 instead.
    message(STATUS "Setting MSVC debug information format to 'Embedded'")
    set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<$<CONFIG:Debug,RelWithDebInfo>:Embedded>")

    set(CMAKE_VS_GLOBALS
      "CLToolExe=cl.exe"
      "CLToolPath=${CMAKE_BINARY_DIR}"
      "UseMultiToolTask=true"
      "DebugInformationFormat=OldStyle"
    )

endmacro()

if (ENABLE_CCACHE)
    find_program(hasCCACHE ccache)
    if (NOT hasCCACHE)
        set(ENABLE_CCACHE   OFF)
        message(WARNING "not found ccache binary, ccache is disabled")
    else()
        set(ccache_exe ${hasCCACHE})
        if (MSVC)
            VisualStudio_CCACHE(${ccache_exe})
        else()
            set(CMAKE_C_COMPILER_LAUNCHER   ${ccache_exe})
            set(CMAKE_CXX_COMPILER_LAUNCHER ${ccache_exe})
        endif()
    endif()
endif()

if (ENABLE_DISTCC)
    find_program(hasDISTCC distcc)
    if (NOT hasDISTCC)
        set(ENABLE_DISTCC   OFF)
        message(WARNING "not found distcc program, distcc is disabled")
    else()
        set(distcc_exe ${hasDISTCC})
        set(CMAKE_C_COMPILER_LAUNCHER   ${distcc_exe})
        set(CMAKE_CXX_COMPILER_LAUNCHER ${distcc_exe})
    endif()
endif()

if (ENABLE_TIDY_CHECK)
    find_program(ctidy NAMES clang-tidy)
    if (ctidy)
        set(CMAKE_CXX_CLANG_TIDY    "clang-tidy;-checks=*")
        set(CMAKE_C_CLANG_TIDY      "clang-tidy;-checks=*")
    else()
        message(STATUS "not found clang-tidy!, disabled ENABLE_TIDY_CHECK")
        set(ENABLE_TIDY_CHECK OFF)
    endif()
endif()

macro(installTarget name isSDK)
    get_target_property(_imported ${name} IMPORTED)
    if (_imported)
        get_target_property(_lib ${name} IMPORTED_IMPLIB)
        get_target_property(_so  ${name} IMPORTED_LOCATION)
        if (_lib AND isSDK)
            install(FILES ${_lib} DESTINATION lib)
        endif()
        if (_so)
            install(FILES ${_so} DESTINATION bin)
        endif()
        unset(_lib)
        unset(_so)
    else()
        install(TARGETS ${name}
            RUNTIME DESTINATION bin
            LIBRARY DESTINATION lib
            ARCHIVE DESTINATION lib
        )
    endif()
endmacro()

set(CUDA90_COMPATIBLE_VERSION_MAJOR        9)
set(CUDA90_COMPATIBLE_VERSION_MINOR        0)
set(CUDA90_COMPATIBLE_VERSION_PATCH        176)
set(CUDA90_COMPATIBLE_VERSION_EXTRA        )
set(CUDA90_COMPATIBLE_VERSION              ${CUDA90_COMPATIBLE_VERSION_MAJOR}.${CUDA90_COMPATIBLE_VERSION_MINOR}.${CUDA90_COMPATIBLE_VERSION_PATCH})
set(CUDA90_HDR                             include-9.0.176)
set(CUDA90_COMPATIBLE_ARCHITECTURES        70)

set(CUDA120_COMPATIBLE_VERSION_MAJOR       12)
set(CUDA120_COMPATIBLE_VERSION_MINOR       0)
set(CUDA120_COMPATIBLE_VERSION_PATCH       0)
set(CUDA120_COMPATIBLE_VERSION_EXTRA       527.41)
set(CUDA120_COMPATIBLE_VERSION             ${CUDA120_COMPATIBLE_VERSION_MAJOR}.${CUDA120_COMPATIBLE_VERSION_MINOR}.${CUDA120_COMPATIBLE_VERSION_PATCH})
set(CUDA120_HDR                            include-12.0.0_527.41)
set(CUDA120_COMPATIBLE_ARCHITECTURES       90)

set(CUDA123_COMPATIBLE_VERSION_MAJOR       12)
set(CUDA123_COMPATIBLE_VERSION_MINOR       3)
set(CUDA123_COMPATIBLE_VERSION_PATCH       1)
set(CUDA123_COMPATIBLE_VERSION_EXTRA       546.12)
set(CUDA123_COMPATIBLE_VERSION             ${CUDA123_COMPATIBLE_VERSION_MAJOR}.${CUDA123_COMPATIBLE_VERSION_MINOR}.${CUDA123_COMPATIBLE_VERSION_PATCH})
set(CUDA123_HDR                            include-12.3.1_546.12)
set(CUDA123_COMPATIBLE_ARCHITECTURES       90)

set(CUDA124_COMPATIBLE_VERSION_MAJOR       12)
set(CUDA124_COMPATIBLE_VERSION_MINOR       4)
set(CUDA124_COMPATIBLE_VERSION_PATCH       0)
set(CUDA124_COMPATIBLE_VERSION_EXTRA       551.61)
set(CUDA124_COMPATIBLE_VERSION             ${CUDA124_COMPATIBLE_VERSION_MAJOR}.${CUDA124_COMPATIBLE_VERSION_MINOR}.${CUDA124_COMPATIBLE_VERSION_PATCH})
set(CUDA124_HDR                            include-12.4.0_551.61)
set(CUDA124_COMPATIBLE_ARCHITECTURES       90)

set(CUDA_COMPATIBLE_VERSION_MAJOR      ${CUDA${BUILD_CUDA}_COMPATIBLE_VERSION_MAJOR})
set(CUDA_COMPATIBLE_VERSION_MINOR      ${CUDA${BUILD_CUDA}_COMPATIBLE_VERSION_MINOR})
set(CUDA_COMPATIBLE_VERSION_PATCH      ${CUDA${BUILD_CUDA}_COMPATIBLE_VERSION_PATCH})
set(CUDA_COMPATIBLE_VERSION_EXTRA      ${CUDA${BUILD_CUDA}_COMPATIBLE_VERSION_EXTRA})
set(CUDA_COMPATIBLE_VERSION            ${CUDA${BUILD_CUDA}_COMPATIBLE_VERSION}      )
set(CUDA_HDR                           ${CUDA${BUILD_CUDA}_HDR}                     )
set(CUDA_COMPATIBLE_ARCHITECTURES      ${CUDA${BUILD_CUDA}_COMPATIBLE_ARCHITECTURES})


set(COMPUTE_ROOT        ${CMAKE_SOURCE_DIR})
if (NOT COMPUTE_SDK_DIR)
    set(COMPUTE_SDK_DIR ${AQROOT}/build/sdk)
endif()
set(VCC_ROOT            ${COMPUTE_SDK_DIR})
set(COMPUTE_PUBLIC_HDR  ${COMPUTE_SDK_DIR}/include)
set(COMPUTE_PRIVATE_HDR ${COMPUTE_ROOT}/sdk/inc)
set(CUDA_PRIVATE_HDR    ${COMPUTE_ROOT}/external/${CUDA124_HDR})

set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS   ON)

set(OS_DEP_LIB  )
if (MSVC)
    set(BUILD_TYPES ${CMAKE_BUILD_TYPE} ${CMAKE_CONFIGURATION_TYPES})
    list(REMOVE_DUPLICATES BUILD_TYPES)


    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY      ${COMPUTE_SDK_DIR}/lib)
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY      ${COMPUTE_SDK_DIR}/lib)
    set(CMAKE_PDB_OUTPUT_DIRECTORY          ${COMPUTE_SDK_DIR}/bin)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY      ${COMPUTE_SDK_DIR}/bin)

    foreach(config IN LISTS BUILD_TYPES)
        string(TOUPPER ${config} config)
        set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${config}      ${COMPUTE_SDK_DIR}/lib)
        set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${config}      ${COMPUTE_SDK_DIR}/lib)
        set(CMAKE_PDB_OUTPUT_DIRECTORY_${config}          ${COMPUTE_SDK_DIR}/bin)
        set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${config}      ${COMPUTE_SDK_DIR}/bin)
    endforeach()
else ()
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY      ${COMPUTE_SDK_DIR}/drivers)
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY      ${COMPUTE_SDK_DIR}/drivers)
    set(CMAKE_PDB_OUTPUT_DIRECTORY          ${COMPUTE_SDK_DIR}/drivers)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY      ${COMPUTE_SDK_DIR}/drivers)
    if (MINGW)
        set(OS_DEP_LIB ${OS_DEP_LIB} pthread rt)
    else()
        set(OS_DEP_LIB ${OS_DEP_LIB} dl pthread rt)
    endif()
endif()

if ("${CMAKE_SYSTEM_NAME}" STREQUAL "Linux")
    execute_process(
        COMMAND mkdir -p ${COMPUTE_SDK_DIR}/drivers
    )
    execute_process(
        COMMAND ln -s drivers bin
        COMMAND ln -s drivers lib
        WORKING_DIRECTORY ${COMPUTE_SDK_DIR}
    )
endif()

######################################################################
# global configuration
if (MSVC)
    add_compile_options("/MP")
endif()

######################################################################
# start to generate project
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake/reggen")
include(RegGen)
include(config.cmake OPTIONAL)

if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/cmake/cmodel)
    list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake/cmodel")
    include(GCDefine)
    include(CModel)
	if(BUILD_CMODEL_TEST_BENCH)
		include(TestBench)
	endif()
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake/hal")
include(build_options)

if (UNIX AND DLOPEN_SECURITY_WAR)
    add_definitions(-DDLOPEN_SECURITY_WAR=1)
endif()
add_definitions(-DCUDA_COMPATIBLE_VERSION=${CUDA_COMPATIBLE_VERSION})
add_definitions(-DCUDA_COMPATIBLE_VERSION_MAJOR=${CUDA_COMPATIBLE_VERSION_MAJOR})
add_definitions(-DCUDA_COMPATIBLE_VERSION_MINOR=${CUDA_COMPATIBLE_VERSION_MINOR})
add_definitions(-DCUDA_COMPATIBLE_VERSION_PATCH=${CUDA_COMPATIBLE_VERSION_PATCH})
add_definitions(-DDISABLE_BUILD_ISSUE=1 -DgcdENABLE_UVM=0)
add_definitions(-D_NOT_USE_HAL_USER)

if (CMAKE_BUILD_TYPE STREQUAL Debug)
    add_definitions(-DDEBUG)
    add_definitions(-D_DEBUG)
endif()

macro(add_shared_resource tgt map def)
    if (WIN32)
        target_sources(${tgt} PRIVATE ${def})
    elseif (UNIX)
        target_sources(${tgt} PRIVATE ${map})
    endif()
endmacro()

add_subdirectory(../../compiler/cuda compiler)

add_subdirectory(hal)
add_subdirectory(driver)
add_subdirectory(runtime)
add_subdirectory(dummy)

if (BUILD_TESTS)
    if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test)
        enable_testing()
        add_subdirectory(test)
    else()
        message(STATUS "no test directory, skipped!")
        set(BUILD_TESTS OFF)
    endif()
endif()

file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/external/${CUDA_HDR}/ DESTINATION ${COMPUTE_PUBLIC_HDR}/
    FILES_MATCHING PATTERN *.h
)
file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/external/${CUDA_HDR}/ DESTINATION ${COMPUTE_PUBLIC_HDR}/
    FILES_MATCHING PATTERN *.hpp
)
file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/cmake/cuda DESTINATION ${COMPUTE_SDK_DIR}/cmake)

if (WIN32)
	file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/cmake/extras DESTINATION ${COMPUTE_SDK_DIR})
endif()

if (NOT "${BUILD_CUDA}" STREQUAL "90")
    # Copy files without extension, such as "nv/target"
    file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/external/${CUDA_HDR}/ DESTINATION ${COMPUTE_PUBLIC_HDR}/
        FILES_MATCHING REGEX "[a-zA-Z0-9]"
    )
    file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/external/hip DESTINATION ${COMPUTE_PUBLIC_HDR})
endif()
configure_file(${COMPUTE_SDK_DIR}/cmake/cuda/vsiCUDAVersion.cmake ${COMPUTE_SDK_DIR}/cmake/cuda/vsiCUDAVersion.cmake @ONLY)

file(COPY ${COMPUTE_PRIVATE_HDR}/cuda_vsi_extension.h DESTINATION ${COMPUTE_PUBLIC_HDR}/)

#if (MINGW AND WIN32)
    #file(COPY ${COMPUTE_SDK_DIR}/bin/libEmulator.dll
     #   DESTINATION ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}
    #)
#endif()

RegGen()
GCDefine()

if (BUILD_CMODEL)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION OFF)
    import_emulator()
	if(BUILD_CMODEL_TEST_BENCH)
        import_TestBench()
	endif()
endif()


install(DIRECTORY ${COMPUTE_SDK_DIR}/include    DESTINATION .)
install(DIRECTORY ${COMPUTE_SDK_DIR}/cmake      DESTINATION .)

#add_subdirectory(ccl)


message(STATUS "=========================")
message(STATUS "INFO Sumary:")
message(STATUS "AQROOT                  : ${AQROOT}")
message(STATUS "AQARCH                  : ${AQARCH}")
message(STATUS "COMPUTE PUBLIC HEADER   : ${COMPUTE_PUBLIC_HDR}")
message(STATUS "COMPUTE PRIVATE HEADER  : ${COMPUTE_PRIVATE_HDR}")
message(STATUS "platform type           : ${CMAKE_SYSTEM_NAME} - ${PLATFORM_TYPE}")
message(STATUS "build vcc               : ${BUILD_VCC}")
message(STATUS "build vrtc              : ${BUILD_VRTC}")
message(STATUS "build vfatbin           : ${BUILD_VFATBIN}")
message(STATUS "build vlink             : ${BUILD_VLINK}")
message(STATUS "build spvCompiler       : ${BUILD_SPVCOMPILER}")
message(STATUS "build tests             : ${BUILD_TESTS}")
message(STATUS "build cuObjdump         : ${BUILD_CUOBJDUMP}")
message(STATUS "build vsidisasm         : ${BUILD_VSIDISASM}")
message(STATUS "build cu++filt          : ${BUILD_CUFILT}")
message(STATUS "build vsiprune          : ${BUILD_VSIPRUNE}")
message(STATUS "build drm_kmd           : ${BUILD_DRM_KMD}")
message(STATUS "CUDA_HDR                : ${CUDA_HDR}")
message(STATUS "CUDA SDK VERSION        : ${CUDA_COMPATIBLE_VERSION}")
message(STATUS "dummy CUDA libs         : ${ENABLE_DUMMY_LIBS}")
message(STATUS "enable clang tidy check : ${ENABLE_TIDY_CHECK}")
message(STATUS "HAL name                : ${HAL_TARGET_NAME}")
message(STATUS "driver name             : ${DRV_TARGET_NAME}")
message(STATUS "runtime name            : ${RT_TARGET_NAME}")
message(STATUS "compiler name           : ${COMPILER_TARGET_NAME}")
message(STATUS "cmodel denorm support   : ${ENABLE_CMODEL_DENORM}")
message(STATUS "=========================")
