set(RT_SRC
    api.cpp api.h
    drv.cpp drv.h
    device.cpp device.h
    ../hal/os.h
    #../driver/type_converter.cpp ../driver/type_converter.h
    ../driver/dispatch_table.inc ../driver/drv_types.h
    api_types.h
    rt_common.h rt_types.h
    api_runtime.cpp runtime_log.cpp
    utils.h utils.cpp
    runtime_dispatch_table.inc
    device_property.inc drv2rt_error.inc
)

function(export_table)
    include(${CMAKE_CURRENT_SOURCE_DIR}/exports/hip.cmake)
    include(${CMAKE_CURRENT_SOURCE_DIR}/exports/cuda_common.cmake)
    include(${CMAKE_CURRENT_SOURCE_DIR}/exports/cuda900.cmake)
    include(${CMAKE_CURRENT_SOURCE_DIR}/exports/cuda901.cmake)
    include(${CMAKE_CURRENT_SOURCE_DIR}/exports/cuda902.cmake)
    include(${CMAKE_CURRENT_SOURCE_DIR}/exports/cuda1000.cmake)
    include(${CMAKE_CURRENT_SOURCE_DIR}/exports/cuda1001.cmake)
    include(${CMAKE_CURRENT_SOURCE_DIR}/exports/cuda1002.cmake)
    include(${CMAKE_CURRENT_SOURCE_DIR}/exports/cuda1100.cmake)
    include(${CMAKE_CURRENT_SOURCE_DIR}/exports/cuda1200.cmake)
    include(${CMAKE_CURRENT_SOURCE_DIR}/exports/cuda1203.cmake)
    include(${CMAKE_CURRENT_SOURCE_DIR}/exports/cuda1204.cmake)

    #if (ENABLE_EXTERIMENTAL)
    #    list(APPEND table ${HIP_EXPORTS})
    #endif()
    list(APPEND table ${CUDA_EXPORTS})

    math(EXPR version "${CUDA_COMPATIBLE_VERSION_MAJOR} * 100 + ${CUDA_COMPATIBLE_VERSION_MINOR}")
    list(APPEND table ${CUDA${version}_EXPORTS})

    if (WIN32)
        list(APPEND table ${CUDA${version}_WIN_EXPORTS})
    elseif (UNIX)
        list(APPEND table ${CUDA${version}_LINUX_EXPORTS})
    else()
        message(FATAL_ERROR "unknown platform")
    endif()

    list(REMOVE_DUPLICATES table)
    if (WIN32)
        set(str
[=[
; generate by cmake script
EXPORTS
]=])
        foreach(v IN LISTS table)
            set(str "${str}\t${v}\n")
        endforeach()
        set(name "exports.def")
    elseif (UNIX)
        set(str
[=[
/* generate by cmake script */
LOCAL:
    *;

GLOBAL:
]=])
        foreach(v IN LISTS table)
            set(str "${str}\t${v};\n")
        endforeach()
        set(name "exports.map")
    else()
        message(FATAL_ERROR "unknown platform")
    endif()
    file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/${name} "${str}")
endfunction()

set(RT_SRC ${RT_SRC} api_cuda.cpp)
#if (ENABLE_EXTERIMENTAL)
    set(RT_SRC ${RT_SRC} api_hip.cpp)
    #add_definitions(-DgcdREMOVE_FROM_FINAL_RELEASE=1)
#endif()

export_table()
if (WIN32)
    set(RT_SRC ${RT_SRC} ${CMAKE_CURRENT_BINARY_DIR}/exports.def ../hal/os_windows.cpp)
elseif (UNIX)
    set(RT_SRC ${RT_SRC} ${CMAKE_CURRENT_BINARY_DIR}/exports.map ../hal/os_linux.cpp)
endif()

include_directories(${AQROOT}/compiler/cuda/VCC2/VCC_Core/include
	${AQROOT}/driver/cuda/sdk/inc
	${AQROOT}/hal/inc/shared)

add_library(${RT_TARGET_NAME} SHARED ${RT_SRC})

target_link_libraries(${RT_TARGET_NAME} PRIVATE
  vcc-core
)
target_include_directories(${RT_TARGET_NAME}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../driver
            ${CMAKE_CURRENT_SOURCE_DIR}/../hal
            ${COMPUTE_PRIVATE_HDR}
    PUBLIC  ${COMPUTE_PUBLIC_HDR}
)
#target_link_libraries(${RT_TARGET_NAME} PRIVATE ${HAL_TARGET_NAME} ${DRV_TARGET_NAME})
target_compile_definitions(${RT_TARGET_NAME} PRIVATE -DVSI_CUDA_RUNTIME=1)

if (OS_DEP_LIB)
    target_link_libraries(${RT_TARGET_NAME} PUBLIC ${OS_DEP_LIB})
endif()

installTarget(${RT_TARGET_NAME} ON)
