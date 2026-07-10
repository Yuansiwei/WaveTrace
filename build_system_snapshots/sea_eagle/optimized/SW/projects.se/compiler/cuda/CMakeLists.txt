
set(hal_os ${AQROOT}/hal/os/)
set(hal_inc  ${AQROOT}/hal/inc ${AQROOT}/hal/user ${AQROOT}/hal/user/arch)
#set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON)

if (BUILD_EMULATOR)
    if (WIN32)
        file(GLOB os_files ${hal_os}/emulator/user/gc_hal_user_os*)
        set(hal_inc ${hal_inc} ${hal_os}/emulator/user/ ${hal_os}/emulator/inc)
        set(hal_files ${hal_os}/emulator/user/gc_hal_user_debug.c
            ${hal_os}/emulator/user/gc_hal_user_math.c
        )
        set(hal_dep Ws2_32)
    else()
        file(GLOB os_files ${hal_os}/linuxemulator/user/gc_hal_user_os*)
        set(hal_inc ${hal_inc} ${hal_os}/linuxemulator/user/ ${hal_os}/linuxemulator/inc ${hal_os}/linuxemulator/emulator/ ${AQROOT}/hal/kernel/  ${AQROOT}/hal/kernel/arch)
        set(hal_files ${hal_os}/linuxemulator/user/gc_hal_user_debug.c
            ${hal_os}/linuxemulator/user/gc_hal_user_math.c
            ${hal_os}/linuxemulator/user/gc_hal_user_atomic.c
        )
        set(hal_dep pthread)
    endif()
elseif(BUILD_WINDOWS)
    file(GLOB os_files ${hal_os}/emulator/user/gc_hal_user_os*)
    set(hal_inc ${hal_inc} ${hal_os}/emulator/user/ ${hal_os}/emulator/inc)
    set(hal_files ${hal_os}/emulator/user/gc_hal_user_debug.c
        ${hal_os}/emulator/user/gc_hal_user_math.c
    )
    set(hal_dep Ws2_32)
elseif(BUILD_LINUX)
    file(GLOB os_files ${hal_os}/linux/user/gc_hal_user_os*)
    set(hal_inc ${hal_inc} ${hal_os}/linux/user/ ${hal_os}/linux/kernel/ ${AQROOT}/hal/kernel/ ${AQROOT}/hal/kernel/arch)
    set(hal_files ${hal_os}/linux/user/gc_hal_user_debug.c
        ${hal_os}/linux/user/gc_hal_user_math.c
    )
    set(hal_dep pthread)
else()
    message(FATAL_ERROR "unknown platform")
endif()
set(vsc_inc ${AQROOT}/compiler/libVSC/include)

if (FIXED_ARCH_TYPE)
    set(compiler_DIR ${AQROOT}/compiler)

    macro(copy_import_target tgt)
        get_target_property(_lib ${tgt} IMPORTED_IMPLIB)
        get_target_property(_loc  ${tgt} IMPORTED_LOCATION)

        if (EXISTS ${_lib})
            file(COPY ${_lib} DESTINATION ${CMAKE_ARCHIVE_OUTPUT_DIRECTORY})
        endif()
        if (_loc)
            file(COPY ${_loc} DESTINATION ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})
        endif()

        unset(_lib)
        unset(_loc)
    endmacro()

    add_library(${COMPILER_TARGET_NAME} SHARED IMPORTED GLOBAL)
    set_target_properties(${COMPILER_TARGET_NAME} PROPERTIES
        IMPORTED_IMPLIB   ${CMAKE_CURRENT_SOURCE_DIR}/${FIXED_ARCH_TYPE}/${CMAKE_SHARED_LIBRARY_PREFIX}${COMPILER_TARGET_NAME}${CMAKE_LINK_LIBRARY_SUFFIX}
        IMPORTED_LOCATION ${CMAKE_CURRENT_SOURCE_DIR}/${FIXED_ARCH_TYPE}/${CMAKE_SHARED_LIBRARY_PREFIX}${COMPILER_TARGET_NAME}${CMAKE_SHARED_LIBRARY_SUFFIX}
    )

    target_include_directories(${COMPILER_TARGET_NAME} INTERFACE
        ${hal_inc}
        ${vsc_inc}
        ${compiler_DIR}/libCLC/inc ${compiler_DIR}/libCLC/compiler ${compiler_DIR}/libCLC/preprocessor ${compiler_DIR}/libCLC/lib
        ${compiler_DIR}/libSPIRV/spvconverter
        ${AQARCH}/cmodel/inc
    )
    installTarget(${COMPILER_TARGET_NAME} OFF)
    copy_import_target(${COMPILER_TARGET_NAME})

    macro(import_compiler_executable name)
        set(_exe ${CMAKE_CURRENT_SOURCE_DIR}/${FIXED_ARCH_TYPE}/${name}${CMAKE_EXECUTABLE_SUFFIX})
        if (EXISTS ${_exe})
            add_executable(${name} IMPORTED GLOBAL)
            set_target_properties(${name} PROPERTIES
                IMPORTED_LOCATION ${_exe}
            )
            installTarget(${name} OFF)
            copy_import_target(${name})
        else()
            message(STATUS "skip none exist executable ${_exe}")
        endif()
        unset(_exe)
    endmacro()

    macro(import_compiler_library name)
        set(_loc ${CMAKE_CURRENT_SOURCE_DIR}/${FIXED_ARCH_TYPE}/${CMAKE_SHARED_LIBRARY_PREFIX}${name}${CMAKE_SHARED_LIBRARY_SUFFIX})
        set(_lib ${CMAKE_CURRENT_SOURCE_DIR}/${FIXED_ARCH_TYPE}/${CMAKE_SHARED_LIBRARY_PREFIX}${name}${CMAKE_LINK_LIBRARY_SUFFIX})

        add_library(${name} SHARED IMPORTED GLOBAL)
        set_target_properties(${name} PROPERTIES
            IMPORTED_IMPLIB   ${_lib}
            IMPORTED_LOCATION ${_loc}
        )
        installTarget(${name} OFF)
        copy_import_target(${name})
    endmacro()

    # VIV:TODO: For nvrtc library, we need to refine the code after link the llvm (vcc2) to driver.
    import_compiler_library(nvrtc)
    message("import_compiler_library(nvrtc)!!!!!!!")

    file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/vcc-lib/runtime DESTINATION ${COMPUTE_SDK_DIR}/include/)
    file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/nvrtc/include/nvrtc.h DESTINATION ${COMPUTE_SDK_DIR}/include/)
    file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/${FIXED_ARCH_TYPE}/clang DESTINATION ${COMPUTE_SDK_DIR}/lib/)
    install(DIRECTORY ${COMPUTE_SDK_DIR}/lib/clang      DESTINATION lib/)
else()
    message("add_subdirectory(nvrtc)!!!!!!!")
    add_subdirectory(vsc)

    if (WIN32)
        set(res compiler.def)
    else()
        set(res compiler.map)
    endif()

    add_library(${COMPILER_TARGET_NAME} SHARED dummy.cpp ${res})
    target_link_libraries(${COMPILER_TARGET_NAME} PRIVATE "$<LINK_LIBRARY:WHOLE_ARCHIVE,vsc>")
    if (NOT BUILD_LINUX)
        target_compile_definitions(${COMPILER_TARGET_NAME} PRIVATE -DEMULATOR)
    endif()
    installTarget(${COMPILER_TARGET_NAME} OFF)

	add_subdirectory(VCC2)

if(BUILD_VRTC)
    add_subdirectory(nvrtc)
endif()

endif()
