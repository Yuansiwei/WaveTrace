


macro(import_TestBench)
    #set(CMAKE_POSITION_INDEPENDENT_CODE ON)
    #set(CMAKE_POSITION_INDEPENDENT_CODE ON)
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
    endif()
    set(incs ${incs} ${AQROOT}/hal/os/linuxemulator/emulator ${AQROOT}/hal/os/linuxemulator/inc)
    add_subdirectory(${AQARCH}/cmodel/TestBench ${CMAKE_BINARY_DIR}/cmodeltestbench)
    #google test
    add_subdirectory(${AQTOOLS}/GPU/perf/googletest-1.16.0 ${CMAKE_BINARY_DIR}/googletestBuild)
    target_link_libraries(testbench PRIVATE gtest)
    target_link_libraries(testbench PRIVATE gtest_main)
    target_link_libraries(testbench PRIVATE cmodel)
    if(WIN32 AND MINGW)
        target_link_libraries(testbench PRIVATE winpthread)
        target_link_directories(testbench PRIVATE C:/Program Files/mingw64/bin)
    endif()
    # KMD, libEmulator
    target_include_directories(testbench PRIVATE ${incs})

    #target_link_options(testbench PRIVATE -flto)
    set_target_properties(testbench PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endmacro()


