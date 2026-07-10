set(debugger_SRC
    vDebugger.cpp vDebugger.h
    argsParse.cpp argsParse.h
    vDebugServer.cpp vDebugServer.h
    vvxDebugServer.cpp vvxDebugServer.h    
    vclDebugServer.cpp vclDebugServer.h
    vcuDebugServer.cpp vcuDebugServer.h
    DebugBackend.cpp DebugBackend.h
    vDebuggerUser.cpp vDebuggerUser.h
    vDebugCommonUtils.cpp vDebugCommonUtils.h
    vDebugDataUtils.cpp vDebugDataUtils.h
    vDebugTransformer.cpp vDebugTransformer.h
    vmiImpl.cpp vmiImpl.h
    vcliImpl.cpp vcliImpl.h
    vDebugCompiler.cpp vDebugCompiler.h
    vDebugBreakpoint.cpp vDebugBreakpoint.h
    )

#if (WIN32)
#    set(debugger_SRC ${debugger_SRC} dbg.def)
#else()
#    set(debugger_SRC ${debugger_SRC} dbg.map)
#endif()

add_library(${DEBUGGER_TARGET_NAME} SHARED ${debugger_SRC})

target_include_directories(${DEBUGGER_TARGET_NAME}
    PRIVATE ${AQROOT}/hal/inc
            ${AQROOT}/hal
            ${AQROOT}/compiler/libVSC/include  
            ${AQARCH}/cmodel/inc
            ${AQROOT}/hal/user
            ${AQROOT}/hal/os/emulator/user
            ${AQROOT}/debugger
    INTERFACE ${AQROOT}/debugger
)
target_link_libraries(${DEBUGGER_TARGET_NAME} PRIVATE Emulator vsc m pthread dl rt stdc++)
target_compile_definitions(${DEBUGGER_TARGET_NAME} PRIVATE -DVDEBUG_SERVER_2=1)
target_compile_definitions(${DEBUGGER_TARGET_NAME} PRIVATE -DLINUX)
target_link_directories(${DEBUGGER_TARGET_NAME} PUBLIC ${CMAKE_ARCHIVE_OUTPUT_DIRECTORY})
#if (BUILD_LINUX)
    target_compile_definitions(${DEBUGGER_TARGET_NAME} PRIVATE -DLINUX)
#endif()


installTarget(${DEBUGGER_TARGET_NAME} ON)
