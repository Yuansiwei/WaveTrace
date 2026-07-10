macro(filterSource root vcxproj var)
    file(GLOB_RECURSE allfiles RELATIVE ${root} ${root}/*.cpp ${root}/*.c ${root}/*.cxx)
    string(TOUPPER "${allfiles}" allfiles_upper)
    string(TOUPPER "${vcxproj}"  vcxproj)

    #message(STATUS "files : ${vcxproj}")
    #message(STATUS "filesU: ${allfiles_upper}")
    #message(STATUS "root  : ${root}")
    foreach(f IN LISTS vcxproj)
        list(FIND allfiles_upper ${f} idx)

        #message(STATUS "idx:${idx} - ${f}")
        if (NOT "${idx}" STREQUAL "-1")
            list(GET allfiles ${idx}  f)
            #message(STATUS "select: ${f}")
            set(${var} ${${var}} ${root}/${f})
        endif()
    endforeach()
endmacro()

macro(filterHeader root vcxproj var)
    file(GLOB_RECURSE allfiles RELATIVE ${root} ${root}/*.h ${root}/*.hpp ${root}/*.hxx)
    string(TOUPPER "${allfiles}" allfiles_upper)
    string(TOUPPER "${vcxproj}"  vcxproj)

    #message(STATUS "files : ${vcxproj}")
    #message(STATUS "filesU: ${allfiles_upper}")
    #message(STATUS "root  : ${root}")
    foreach(f IN LISTS vcxproj)
        list(FIND allfiles_upper ${f} idx)

        #message(STATUS "idx:${idx} - ${f}")
        if (NOT "${idx}" STREQUAL "-1")
            list(GET allfiles ${idx}  f)
            #message(STATUS "select: ${f}")
            set(${var} ${${var}} ${root}/${f})
        endif()
    endforeach()
endmacro()


macro (GetFilesFromvcxproj var filename)
    # source
    file(STRINGS ${filename} vcxproj REGEX "<ClCompile Include=")
    string(REPLACE "<ClCompile Include=" "" vcxproj ${vcxproj})
    string(REPLACE " " "" vcxproj ${vcxproj})
    string(REPLACE "\t" "" vcxproj ${vcxproj})
    string(REGEX REPLACE "/>" "" vcxproj ${vcxproj})
    string(REGEX REPLACE ">" "" vcxproj ${vcxproj})
    string(REPLACE "\"\"" "@" vcxproj ${vcxproj})
    string(REPLACE "\"" "" vcxproj ${vcxproj})
    string(REPLACE "\\" "/" vcxproj ${vcxproj})
    string(REPLACE "@" ";" vcxproj ${vcxproj})
    #list(REMOVE_ITEM vcxproj "$(AQROOT)/tools/RenderTarget/Client/RTClient.cpp")
    #message(STATUS "${vcxproj}")

    get_filename_component(root ${filename} DIRECTORY ABSOLUTE)
    filterSource("${root}" "${vcxproj}" ${var})

    # header
    file(STRINGS ${filename} vcxproj REGEX "<ClInclude Include=")
    if (vcxproj)
        string(REPLACE "<ClInclude Include=" "" vcxproj ${vcxproj})
        string(REPLACE " " "" vcxproj ${vcxproj})
        string(REPLACE "\t" "" vcxproj ${vcxproj})
        string(REGEX REPLACE "[ ]*/>" "" vcxproj ${vcxproj})
        string(REPLACE "\\" "/" vcxproj ${vcxproj})
        string(REPLACE "\"\"" "@" vcxproj ${vcxproj})
        string(REPLACE "\"" "" vcxproj ${vcxproj})
        string(REPLACE "@" ";" vcxproj ${vcxproj})
        #list(REMOVE_ITEM vcxproj "$(AQROOT)/tools/RenderTarget/Client/RTClient.hpp")
        #message(STATUS "${vcxproj}")

        filterHeader("${root}" "${vcxproj}" ${var})
    endif()

    #message(STATUS "cmodel files: ${${var}}")
    source_group(TREE ${root} FILES ${${var}})
endmacro()

function(add_cmodel)
    GetFilesFromvcxproj(src ${AQARCH}/cmodel/cmodel.vcxproj)

    if (WIN32)
        cmake_path(CONVERT "${AQTOOLS}" TO_CMAKE_PATH_LIST AQTOOLS NORMALIZE)
        #set(src ${src}
        #    ${AQTOOLS}/RenderTarget/Client/RTClient.cpp
        #    ${AQTOOLS}/RenderTarget/Client/RTClient.hpp
        #)
    endif()

    add_library(cmodel STATIC ${src})
endfunction()

macro(import_emulator_src)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
    cmake_path(CONVERT "${AQROOT}" TO_CMAKE_PATH_LIST AQROOT NORMALIZE)
    cmake_path(CONVERT "${AQARCH}" TO_CMAKE_PATH_LIST AQARCH NORMALIZE)
    cmake_path(CONVERT "${AQTOOLS}" TO_CMAKE_PATH_LIST AQTOOLS NORMALIZE)

    set(incs ${AQROOT}/hal/inc
        ${AQROOT}/hal/kernel ${AQROOT}/hal/kernel/arch
        ${AQTOOLS}/GPU/perf/systemc-2.3.3/src
    )
    if (WIN32)
        set(incs ${incs} ${AQROOT}/hal/os/emulator/emulator ${AQROOT}/hal/os/emulator/inc)
    else()
        set(incs ${incs} ${AQROOT}/hal/os/linuxemulator/emulator ${AQROOT}/hal/os/linuxemulator/inc)
    endif()

    # cmodel, cmodel.a
    if (BUILD_CMODEL)
		if(UNIX)
			if (FIXED_ARCH_TYPE)
				add_library(cmodel STATIC IMPORTED GLOBAL)
				set_target_properties(cmodel PROPERTIES
					IMPORTED_IMPLIB   ${CMAKE_CURRENT_SOURCE_DIR}/${FIXED_ARCH_TYPE}/${CMAKE_SHARED_LIBRARY_PREFIX}cmodel${CMAKE_LINK_LIBRARY_SUFFIX}
					IMPORTED_LOCATION ${CMAKE_CURRENT_SOURCE_DIR}/${FIXED_ARCH_TYPE}/${CMAKE_SHARED_LIBRARY_PREFIX}cmodel${CMAKE_SHARED_LIBRARY_SUFFIX}
				)
			else()
				add_subdirectory(${AQARCH}/cmodel ${CMAKE_BINARY_DIR}/cmodelLib)
			    if (BUILD_QEMU AND EXISTS ${AQTOOLS}/qemu_cmodel/cmodel_wrapper)
					file(GLOB_RECURSE src ${AQTOOLS}/qemu_cmodel/cmodel_wrapper/wrapper.cpp)
					file(GLOB_RECURSE hdr ${AQTOOLS}/qemu_cmodel/cmodel_wrapper/wrapper.h)
					cmake_path(GET hdr PARENT_PATH hdrPath)
	
					add_library(wrapcmodel SHARED ${src} ${hdr})
                    set_target_properties(wrapcmodel PROPERTIES
                        CXX_STANDARD 14
                        CXX_STANDARD_REQUIRED ON
                    )
					target_include_directories(wrapcmodel PRIVATE ${hdrPath})
					target_compile_options(wrapcmodel PUBLIC -DVIV_CMODEL_ARCH_VITALITY)
					target_link_libraries(wrapcmodel PRIVATE cmodel)
				endif()
			endif()
		else()
			if (FIXED_ARCH_TYPE)
				add_library(cmodel STATIC IMPORTED GLOBAL)
				set_target_properties(cmodel PROPERTIES
					IMPORTED_IMPLIB   ${CMAKE_CURRENT_SOURCE_DIR}/${FIXED_ARCH_TYPE}/${CMAKE_SHARED_LIBRARY_PREFIX}cmodel${CMAKE_LINK_LIBRARY_SUFFIX}
					IMPORTED_LOCATION ${CMAKE_CURRENT_SOURCE_DIR}/${FIXED_ARCH_TYPE}/${CMAKE_SHARED_LIBRARY_PREFIX}cmodel${CMAKE_SHARED_LIBRARY_SUFFIX}
				)
	
				#target_include_directories(cmodel INTERFACE
				#    ${hal_inc}
				#    ${vsc_inc}
				#    ${compiler_DIR}/libCLC/inc ${compiler_DIR}/libCLC/compiler ${compiler_DIR}/libCLC/preprocessor ${compiler_DIR}/libCLC/lib
				#    ${compiler_DIR}/libSPIRV/spvconverter
				#    ${AQARCH}/cmodel/inc
				#)
			else()
				add_subdirectory(${AQARCH}/cmodel ${CMAKE_BINARY_DIR}/cmodelLib)
				if (BUILD_QEMU AND EXISTS ${AQTOOLS}/qemu_cmodel/cmodel_wrapper)
					file(GLOB_RECURSE src ${AQTOOLS}/qemu_cmodel/cmodel_wrapper/wrapper.cpp)
					file(GLOB_RECURSE hdr ${AQTOOLS}/qemu_cmodel/cmodel_wrapper/wrapper.h)
					cmake_path(GET hdr PARENT_PATH hdrPath)
	
					add_library(wrapcmodel SHARED ${src} ${hdr})
					target_include_directories(wrapcmodel PRIVATE ${hdrPath})
					target_compile_options(wrapcmodel PUBLIC -DVIV_CMODEL_ARCH_VITALITY)
					target_link_libraries(wrapcmodel PRIVATE cmodel)
				endif()
			endif()
		endif()
    endif()
    # KMD, libEmulator
    if (BUILD_EMULATOR)
        set(CMAKE_CXX_STANDARD 14)
        set(CMAKE_CXX_STANDARD_REQUIRED ON)
        set(kernel_src
            ${AQROOT}/hal/kernel/gc_hal_kernel.c
            ${AQROOT}/hal/kernel/gc_hal_kernel_command.c
            ${AQROOT}/hal/kernel/gc_hal_kernel_db.c
            ${AQROOT}/hal/kernel/gc_hal_kernel_event.c
            ${AQROOT}/hal/kernel/gc_hal_kernel_heap.c
            ${AQROOT}/hal/kernel/gc_hal_kernel_gmmu.c
            ${AQROOT}/hal/kernel/gc_hal_kernel_preemption.c
            ${AQROOT}/hal/kernel/gc_hal_kernel_video_memory.c
            ${AQROOT}/hal/inc/gc_hal_options.h
            ${AQROOT}/hal/inc/gc_hal_types.h
            ${AQROOT}/hal/inc/gc_hal_version.h
            ${AQROOT}/hal/inc/gc_hal.h
            ${AQROOT}/hal/inc/gc_hal_driver.h
            ${AQROOT}/hal/inc/gc_hal_enum.h
            ${AQROOT}/hal/kernel/gc_hal_kernel.h
            ${AQROOT}/hal/kernel/gc_hal_kernel_gmmu.h
            ${AQROOT}/hal/kernel/gc_hal_kernel_types.h
            ${AQROOT}/hal/kernel/gc_hal_kernel_precomp.h
        )
        set(arch_src
            ${AQROOT}/hal/kernel/arch/gc_hal_kernel_context.c
            ${AQROOT}/hal/kernel/arch/gc_hal_kernel_hardware.c
            ${AQROOT}/hal/kernel/arch/gc_hal_kernel_hardware_ce.c
            ${AQROOT}/hal/kernel/arch/gc_hal_kernel_hardware_func.c
            ${AQROOT}/hal/kernel/arch/gc_hal_kernel_recorder.c
            ${AQROOT}/hal/kernel/arch/gc_hal_kernel_context.h
            ${AQROOT}/hal/kernel/arch/gc_hal_kernel_hardware.h
        )
        set(emulator_src
            ${AQROOT}/hal/os/emulator/emulator/gc_hal_kernel_atomic.cpp
            ${AQROOT}/hal/os/emulator/emulator/gc_hal_kernel_cache.cpp
            ${AQROOT}/hal/os/emulator/emulator/gc_hal_kernel_cmodel.cpp
            ${AQROOT}/hal/os/emulator/emulator/gc_hal_kernel_cmodel_v2.cpp
            ${AQROOT}/hal/os/emulator/emulator/gc_hal_kernel_debug.c
            ${AQROOT}/hal/os/emulator/emulator/gc_hal_kernel_emulator.cpp
            ${AQROOT}/hal/os/emulator/emulator/gc_hal_kernel_io.cpp
            ${AQROOT}/hal/os/emulator/emulator/gc_hal_kernel_kernel.cpp
            ${AQROOT}/hal/os/emulator/emulator/gc_hal_kernel_memory.cpp
            ${AQROOT}/hal/os/emulator/emulator/gc_hal_kernel_os.cpp
            ${AQROOT}/hal/os/emulator/emulator/gc_hal_kernel_page_heap.cpp
            ${AQROOT}/hal/os/emulator/emulator/gc_hal_kernel_power.cpp
            ${AQROOT}/hal/os/emulator/emulator/gc_hal_kernel_sync.cpp
            ${AQROOT}/hal/os/emulator/emulator/gc_hal_kernel_time.cpp
            ${AQROOT}/hal/os/emulator/emulator/gc_hal_kernel_user.cpp
            ${AQROOT}/hal/os/emulator/emulator/gc_hal_kernel_emulator.h
            ${AQROOT}/hal/os/emulator/emulator/gc_hal_kernel_emulator_precomp.h
            ${AQROOT}/hal/os/emulator/emulator/gc_hal_kernel_os.h
            ${AQROOT}/hal/os/emulator/inc/gc_hal_emulator.h
            ${AQROOT}/hal/os/emulator/inc/gc_hal_kernel_debug.h
        )
        set(linuxemulator_src
            ${AQROOT}/hal/os/linuxemulator/emulator/gc_hal_kernel_atomic.cpp
            ${AQROOT}/hal/os/linuxemulator/emulator/gc_hal_kernel_broadcast.cpp
            ${AQROOT}/hal/os/linuxemulator/emulator/gc_hal_kernel_cache.cpp
            ${AQROOT}/hal/os/linuxemulator/emulator/gc_hal_kernel_cmodel.cpp
            ${AQROOT}/hal/os/linuxemulator/emulator/gc_hal_kernel_debug.cpp
            ${AQROOT}/hal/os/linuxemulator/emulator/gc_hal_kernel_debug.h
            ${AQROOT}/hal/os/linuxemulator/emulator/gc_hal_kernel_emulator.cpp
            ${AQROOT}/hal/os/linuxemulator/emulator/gc_hal_kernel_emulator.h
            ${AQROOT}/hal/os/linuxemulator/emulator/gc_hal_kernel_emulator_precomp.h
            ${AQROOT}/hal/os/linuxemulator/emulator/gc_hal_kernel_io.cpp
            ${AQROOT}/hal/os/linuxemulator/emulator/gc_hal_kernel_kernel.cpp
            ${AQROOT}/hal/os/linuxemulator/emulator/gc_hal_kernel_memory.cpp
            ${AQROOT}/hal/os/linuxemulator/emulator/gc_hal_kernel_mutex.h
            ${AQROOT}/hal/os/linuxemulator/emulator/gc_hal_kernel_os.cpp
            ${AQROOT}/hal/os/linuxemulator/emulator/gc_hal_kernel_os.h
            ${AQROOT}/hal/os/linuxemulator/emulator/gc_hal_kernel_page_heap.cpp
            ${AQROOT}/hal/os/linuxemulator/emulator/gc_hal_kernel_sync.cpp
            ${AQROOT}/hal/os/linuxemulator/emulator/gc_hal_kernel_time.cpp
            ${AQROOT}/hal/os/linuxemulator/emulator/gc_hal_kernel_user.cpp
        )
        if (WIN32)
            set(emulator    libEmulator)
            add_library(${emulator} SHARED
                ${kernel_src}
                ${arch_src}
                ${emulator_src}
            )
            source_group(kernel FILES ${kernel_src})
            source_group(arch FILES ${arch_src})
            source_group(emulator FILES ${emulator_src})
            target_compile_definitions(${emulator} PRIVATE -DEMULATOR -D_USRDLL -DHAL_EXPORTS)
            target_compile_options(${emulator} PRIVATE "/vmg")
        else()
            set(emulator    Emulator)
            add_library(${emulator} SHARED
                ${kernel_src}
                ${arch_src}
                ${linuxemulator_src}
            )
            source_group(kernel FILES ${kernel_src})
            source_group(arch FILES ${arch_src})
            source_group(emulator FILES ${emulator_src})
            target_compile_definitions(${emulator} PRIVATE -DLINUXEMULATOR -DLINUX -DEMULATOR)
            target_compile_options(${emulator} PRIVATE  -fpermissive)
        endif()
        set_target_properties(${emulator} PROPERTIES CXX_STANDARD 14 CXX_STANDARD_REQUIRED ON)
        target_include_directories(${emulator} PRIVATE ${incs})
        target_link_libraries(${emulator} PRIVATE cmodel)
        target_compile_definitions(${emulator} PRIVATE ${BUILD_DRIVER_OPTIONS})
    endif()
endmacro()

macro(import_emulator_bin)
    file(COPY ${AQROOT}/hal/${FIXED_ARCH_TYPE}/${CMAKE_SHARED_LIBRARY_PREFIX}Emulator${CMAKE_SHARED_LIBRARY_SUFFIX} DESTINATION ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})
    file(COPY ${AQROOT}/hal/${FIXED_ARCH_TYPE}/ DESTINATION ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/
        FILES_MATCHING PATTERN ${CMAKE_SHARED_LIBRARY_PREFIX}wrap_*${CMAKE_SHARED_LIBRARY_SUFFIX}
    )
endmacro()

macro(import_emulator)
    if (FIXED_ARCH_TYPE)
        import_emulator_bin()
    else()
        import_emulator_src()
    endif()
endmacro()

