set(TARGET_NAME     gpgpuHal)

set(hal_SRC
    chip.cpp chip.h
    legacy_hal.h legacy_compiler.h
    kmd.h os.h
    hal.cpp hal.h
    types.h options.h
    compiler.cpp compiler.h
    common.cpp os_common.h os_atomic.c
    tensor.cpp tensor.h
    utils.cpp utils.h
    dump.cpp dump.h
    debug.cpp debug.h
    printf.cpp
    mem_profiler.h mem_profiler.cpp
    queue_sched.cpp queue_sched.h
    hal_error.h user_os.cpp
    cache.cpp cache.h
    profiler.cpp profiler.h
)

set(legacy_SRC
    ${AQROOT}/hal/inc/gc_hal_enum.h
    ${AQROOT}/hal/inc/shared/gc_hal_enum_shared.h
    ${AQROOT}/hal/inc/shared/gc_hal_types_shared.h
    #${AQROOT}/hal/inc/shared/gc_hal_profiler_shared.h
    ${AQROOT}/hal/inc/shared/gc_hal_driver_shared.h
    ${AQROOT}/hal/inc/shared/gc_hal_base_shared.h
    ${AQROOT}/hal/user/arch/gc_hal_user_hardware_shared.h
    ${AQROOT}/hal/inc/gc_feature_database.h
)
if (BUILD_DRM_KMD)
    set(legacy_SRC ${legacy_SRC}
        ${AQROOT}/drm_kmd/vsigpu/galcore_calc_ioctl.h
        ${AQROOT}/drm_kmd/vsigpu/include/uapi/drm/vsigpu_drm.h
    )
endif()
source_group(legacy FILES ${legacy_SRC})

set(hal_SRC ${hal_SRC} ${legacy_SRC})
set(hal_PRI_INC )

if (BUILD_EMULATOR)
    if (WIN32)
        set(hal_SRC ${hal_SRC} os_windows.cpp kmd_cmodel.cpp)
        set(hal_PRI_INC ${AQROOT}/hal/os/emulator/user)
    elseif (UNIX)
        set(hal_SRC ${hal_SRC} os_linux.cpp kmd_cmodel.cpp)
        set(hal_PRI_INC ${AQROOT}/hal/os/linuxemulator/user)
    else()
        message(FATAL_ERROR "unknown HAL config")
    endif()
elseif (BUILD_WINDOWS)
    set(hal_SRC ${hal_SRC} os_windows.cpp kmd_wddm.cpp)
    set(hal_PRI_INC ${AQROOT}/driver/windows/wddm/vivante
                    ${AQROOT}/driver/windows/wddm/viv_cmodel
                    ${AQROOT}/hal/os/emulator/user
                    ${AQROOT}/sdk/inc)
elseif (BUILD_LINUX)
    if (BUILD_DRM_KMD)
        set(hal_SRC ${hal_SRC} os_linux.cpp kmd_drm.cpp)
    else()
        set(hal_SRC ${hal_SRC} os_linux.cpp kmd_linux.cpp)
    endif()
    set(hal_PRI_INC ${AQROOT}/hal/os/linux/user)
else()
    message(FATAL_ERROR "unknown HAL config")
endif()

if (BUILD_DEBUGGER)
    set(hal_SRC ${hal_SRC} debugger.cpp debugger.h)
endif()

add_library(${HAL_TARGET_NAME} STATIC ${hal_SRC})

target_include_directories(${HAL_TARGET_NAME}
    PRIVATE ${AQROOT}/hal/inc
            ${AQROOT}/hal/user/
            ${AQROOT}/hal/user/arch/
            ${AQROOT}/compiler/libVSC/include/
            ${AQROOT}/compiler/libSPIRV/spvconverter/
            ${AQARCH}/cmodel/inc
            ${hal_PRI_INC}
            ${COMPUTE_PRIVATE_HDR}
            ${AQROOT}/hal/inc/shared
    INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}
	PUBILIC ${AQROOT}/compiler/cuda/VCC2/VCC_Core/include/
			${AQROOT}/hal/inc/shared/
)
if (BUILD_DRM_KMD)
    target_include_directories(${HAL_TARGET_NAME}
        PRIVATE ${AQROOT}/drm_kmd/vsigpu/include/uapi/drm
                ${AQROOT}/drm_kmd/vsigpu
    )
endif()

if (BUILD_DEBUGGER)
target_include_directories(${HAL_TARGET_NAME} PRIVATE ${AQROOT}/debugger)
target_compile_definitions(${HAL_TARGET_NAME} PRIVATE -DVDEBUG_SERVER_2=1)
endif()

target_link_libraries(${HAL_TARGET_NAME} PUBLIC ${COMPILER_TARGET_NAME})

target_link_libraries(${HAL_TARGET_NAME} PRIVATE
	vcc-core
)

target_compile_definitions(${HAL_TARGET_NAME} PUBLIC -DHAL_OS_INTERFACE_ONLY)

target_compile_definitions(${HAL_TARGET_NAME} PUBLIC ${BUILD_DRIVER_OPTIONS})

if (BUILD_EMULATOR)
    target_compile_definitions(${HAL_TARGET_NAME} PRIVATE -DEMULATOR)
endif()

if (OS_DEP_LIB)
    target_link_libraries(${HAL_TARGET_NAME} PUBLIC ${OS_DEP_LIB})
endif()
