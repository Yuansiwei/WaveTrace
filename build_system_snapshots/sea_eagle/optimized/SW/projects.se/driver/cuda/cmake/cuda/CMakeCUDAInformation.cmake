# cmake 3.28.1
message(STATUS "CUDA for VSI Info - CUDA Information")

macro(pushVar var val)
    set(_pop_push_${var}_old    "${${var}}")
    set(${var}                  "${val}")
endmacro()

macro(popVar var)
    set(${var}  "${_pop_push_${var}_old}")
    unset(_pop_push_${var}_old)
endmacro()

# load the system- and compiler specific files
if (WIN32)
    pushVar(CMAKE_C_SIMULATE_ID                 "MSVC")
    if ("x${VSI_CUDA_HOST_ID}" STREQUAL "xClang" OR "x${VSI_CUDA_HOST_ID}" STREQUAL "xGNU")
        pushVar(CMAKE_C_COMPILER_FRONTEND_VARIANT   "GNU")

        include(Platform/Windows-Clang)
        __windows_compiler_clang(CUDA)

        popVar(CMAKE_C_COMPILER_FRONTEND_VARIANT)
    else()
        set(CMAKE_CUDA_STANDARD_COMPUTED_DEFAULT "14")

        macro(MSVC2VCC var)
            string(REPLACE "-Ob0 -Od" "" ${var} ${${var}})
            string(REPLACE "-O2 -Ob2" "-O2" ${var} ${${var}})
            string(REPLACE "-O2 -Ob1" "-gline-tables-only -O2" ${var} ${${var}})
            string(REPLACE "-O1 -Ob1" "" ${var} ${${var}})
            string(REPLACE "/EHsc" "-fcxx-exceptions -fexceptions" ${var} ${${var}}) # not support -fexternc-nounwind ?
            string(REPLACE "/EP" "-E -P" ${var} ${${var}})
            string(REPLACE "/RTC1" "" ${var} ${${var}})
            string(REPLACE "-MDd" " -D_DEBUG -D_DLL -D_MT -Xcompiler -Xclang -Xcompiler --dependent-lib=msvcrtd" ${var} ${${var}})
            string(REPLACE "-MD" " -D_DLL -D_MT -Xcompiler -Xclang -Xcompiler --dependent-lib=msvcrt" ${var} ${${var}})
            string(REPLACE "-Zi" " -g -Xclang -gcodeview" ${var} ${${var}})
            string(REPLACE "/GR-" " -fno-rtti" ${var} ${${var}})
            string(REPLACE "/GR" "" ${var} ${${var}})
        endmacro()

        # MSVC-CUDA
        pushVar(CMAKE_CXX_COMPILER_VERSION  "${CUDA_HOST_COMPILER_NATIVE_VERSION}")
        set(CMAKE_CUDA_SIMULATE_VERSION     "${CMAKE_CXX_COMPILER_VERSION}")

        include(Platform/Windows-NVIDIA-CUDA)

        popVar(CMAKE_CXX_COMPILER_VERSION)

        MSVC2VCC(CMAKE_CUDA_FLAGS_INIT)
        MSVC2VCC(CMAKE_CUDA_FLAGS_DEBUG_INIT)
        MSVC2VCC(CMAKE_CUDA_FLAGS_RELEASE_INIT)
        MSVC2VCC(CMAKE_CUDA_FLAGS_RELWITHDEBINFO_INIT)
        MSVC2VCC(CMAKE_CUDA_FLAGS_MINSIZEREL_INIT)

        set(CMAKE_CUDA_CREATE_STATIC_LIBRARY_IPO    ${CMAKE_CUDA_CREATE_STATIC_LIBRARY})

        # match with include(Platform/Windows-Clang)
        set(CMAKE_CUDA_COMPILE_OPTIONS_MSVC_RUNTIME_LIBRARY_MultiThreaded         -Xclang -flto-visibility-public-std -D_MT -Xclang --dependent-lib=libcmt)
        set(CMAKE_CUDA_COMPILE_OPTIONS_MSVC_RUNTIME_LIBRARY_MultiThreadedDLL      -D_DLL -D_MT -Xcompiler -Xclang -Xcompiler --dependent-lib=msvcrt)
        set(CMAKE_CUDA_COMPILE_OPTIONS_MSVC_RUNTIME_LIBRARY_MultiThreadedDebug    -D_DEBUG -Xclang -flto-visibility-public-std -D_MT -Xclang --dependent-lib=libcmtd)
        set(CMAKE_CUDA_COMPILE_OPTIONS_MSVC_RUNTIME_LIBRARY_MultiThreadedDebugDLL -D_DEBUG -D_DLL -D_MT -Xcompiler -Xclang -Xcompiler --dependent-lib=msvcrtd)
        #set(CMAKE_CUDA_COMPILE_OPTIONS_MSVC_DEBUG_INFORMATION_FORMAT_Embedded        -g -Xclang -gcodeview)
        set(CMAKE_CUDA_COMPILE_OPTIONS_MSVC_DEBUG_INFORMATION_FORMAT_ProgramDatabase  -g -Xclang -gcodeview)
        #set(CMAKE_CUDA_COMPILE_OPTIONS_MSVC_DEBUG_INFORMATION_FORMAT_EditAndContinue -g -Xclang -gcodeview)
    endif()
    set(CMAKE_CUDA_SIMULATE_ID      "MSVC")

    popVar(CMAKE_C_SIMULATE_ID)
elseif (UNIX)
    include(Platform/Linux-GNU)
    __linux_compiler_gnu(CUDA)
    set(CMAKE_CUDA_SIMULATE_ID      "GNU")
endif()

if ("x${VSI_CUDA_HOST_ID}" STREQUAL "xMSVC")
    set(__IMPLICIT_LINKS " ${CMAKE_LIBRARY_PATH_FLAG}\"${VCC_LIB}\" ${CMAKE_LINK_LIBRARY_FLAG}vsiGPGPURT.lib ")
else()
    set(__IMPLICIT_LINKS " ${CMAKE_LIBRARY_PATH_FLAG}\"${VCC_LIB}\" ${CMAKE_LINK_LIBRARY_FLAG}vsiGPGPURT ")
endif()

if (NOT "x${VSI_CUDA_HOST_ID}" STREQUAL "xMSVC")
    string(REPLACE "<CMAKE_CUDA_COMPILER>" "<CMAKE_CUDA_HOST_LINK_LAUNCHER>" CMAKE_CUDA_CREATE_SHARED_LIBRARY "${CMAKE_CUDA_CREATE_SHARED_LIBRARY}")
    string(REPLACE "<CMAKE_CUDA_COMPILER>" "<CMAKE_CUDA_HOST_LINK_LAUNCHER>" CMAKE_CUDA_CREATE_SHARED_MODULE  "${CMAKE_CUDA_CREATE_SHARED_MODULE}")
    string(REPLACE "<CMAKE_CUDA_COMPILER>" "<CMAKE_CUDA_HOST_LINK_LAUNCHER>" CMAKE_CUDA_LINK_EXECUTABLE       "${CMAKE_CUDA_LINK_EXECUTABLE}")

    pushVar(CMAKE_CUDA_CREATE_SHARED_LIBRARY "")
    pushVar(CMAKE_CUDA_CREATE_SHARED_MODULE  "")
    pushVar(CMAKE_CUDA_LINK_EXECUTABLE       "")

    # Load compiler-specific information.
    include(Compiler/${CMAKE_CUDA_COMPILER_ID}-CUDA OPTIONAL)


    popVar(CMAKE_CUDA_CREATE_SHARED_LIBRARY)
    popVar(CMAKE_CUDA_CREATE_SHARED_MODULE )
    popVar(CMAKE_CUDA_LINK_EXECUTABLE      )
else()
    # Load compiler-specific information.
    include(Compiler/NVIDIA-CUDA OPTIONAL)
endif()


# override configuration
list(REMOVE_ITEM CMAKE_CUDA_RUNTIME_LIBRARY_LINK_OPTIONS_STATIC "cudadevrt" "cudart_static")
list(REMOVE_ITEM CMAKE_CUDA_RUNTIME_LIBRARY_LINK_OPTIONS_SHARED "cudadevrt" "cudart")
list(APPEND CMAKE_CUDA_RUNTIME_LIBRARY_LINK_OPTIONS_STATIC "vsiGPGPURT" "vsiGPGPU")
list(APPEND CMAKE_CUDA_RUNTIME_LIBRARY_LINK_OPTIONS_SHARED "vsiGPGPURT" "vsiGPGPU")
set(_CMAKE_COMPILE_AS_CUDA_FLAG "") # -x cuda

set(CMAKE_CUDA_LINKER_SUPPORTS_PDB          ON)
if (UNIX)
    set(CMAKE_CUDA_COMPILE_OPTIONS_PIC      "-Xcompiler=-fPIC")
    set(CMAKE_CUDA_COMPILE_OPTIONS_PIE      "-Xcompiler=-fPIE")
else()
    set(CMAKE_CUDA_COMPILE_OPTIONS_PIC      "")
    set(CMAKE_CUDA_COMPILE_OPTIONS_PIE      "")
endif()

if ((NOT CMAKE_SKIP_BUILD_RPATH) AND (NOT CMAKE_SKIP_RPATH))
    set(CMAKE_BUILD_RPATH "${CMAKE_BUILD_RPATH};${VCC_ROOT}/bin")
endif()

################################
if(UNIX)
  set(CMAKE_CUDA_OUTPUT_EXTENSION .o)
else()
  set(CMAKE_CUDA_OUTPUT_EXTENSION .obj)
endif()

set(CMAKE_INCLUDE_FLAG_CUDA "-I")
set(CMAKE_SHARED_LIBRARY_CREATE_CUDA_FLAGS      " -shared ")
#set(CMAKE_CUDA_SEPARABLE_COMPILATION            ON)

if (NOT CMAKE_SHARED_LIBRARY_RUNTIME_C_FLAG AND UNIX)
    set(CMAKE_SHARED_LIBRARY_RUNTIME_C_FLAG     "-Wl,-rpath,")
endif()
if (NOT CMAKE_SHARED_LIBRARY_RPATH_LINK_C_FLAG AND UNIX)
    set(CMAKE_SHARED_LIBRARY_RPATH_LINK_C_FLAG  "-Wl,-rpath-link,")
endif()

if(NOT CMAKE_SHARED_LIBRARY_RUNTIME_CUDA_FLAG)
  set(CMAKE_SHARED_LIBRARY_RUNTIME_CUDA_FLAG ${CMAKE_SHARED_LIBRARY_RUNTIME_C_FLAG})
endif()

if(NOT CMAKE_SHARED_LIBRARY_RUNTIME_CUDA_FLAG_SEP)
  set(CMAKE_SHARED_LIBRARY_RUNTIME_CUDA_FLAG_SEP ${CMAKE_SHARED_LIBRARY_RUNTIME_C_FLAG_SEP})
endif()

if(NOT CMAKE_SHARED_LIBRARY_RPATH_LINK_CUDA_FLAG)
  set(CMAKE_SHARED_LIBRARY_RPATH_LINK_CUDA_FLAG ${CMAKE_SHARED_LIBRARY_RPATH_LINK_C_FLAG})
endif()

if(NOT DEFINED CMAKE_EXE_EXPORTS_CUDA_FLAG)
  set(CMAKE_EXE_EXPORTS_CUDA_FLAG ${CMAKE_EXE_EXPORTS_C_FLAG})
endif()

if(NOT DEFINED CMAKE_SHARED_LIBRARY_SONAME_CUDA_FLAG)
  set(CMAKE_SHARED_LIBRARY_SONAME_CUDA_FLAG ${CMAKE_SHARED_LIBRARY_SONAME_C_FLAG})
endif()

if(NOT CMAKE_EXECUTABLE_RUNTIME_CUDA_FLAG)
  set(CMAKE_EXECUTABLE_RUNTIME_CUDA_FLAG ${CMAKE_SHARED_LIBRARY_RUNTIME_CUDA_FLAG})
endif()

if(NOT CMAKE_EXECUTABLE_RUNTIME_CUDA_FLAG_SEP)
  set(CMAKE_EXECUTABLE_RUNTIME_CUDA_FLAG_SEP ${CMAKE_SHARED_LIBRARY_RUNTIME_CUDA_FLAG_SEP})
endif()

if(NOT CMAKE_EXECUTABLE_RPATH_LINK_CUDA_FLAG)
  set(CMAKE_EXECUTABLE_RPATH_LINK_CUDA_FLAG ${CMAKE_SHARED_LIBRARY_RPATH_LINK_CUDA_FLAG})
endif()

if(NOT DEFINED CMAKE_SHARED_LIBRARY_LINK_CUDA_WITH_RUNTIME_PATH)
  set(CMAKE_SHARED_LIBRARY_LINK_CUDA_WITH_RUNTIME_PATH ${CMAKE_SHARED_LIBRARY_LINK_C_WITH_RUNTIME_PATH})
endif()


# for most systems a module is the same as a shared library
# so unless the variable CMAKE_MODULE_EXISTS is set just
# copy the values from the LIBRARY variables
if(NOT CMAKE_MODULE_EXISTS)
  set(CMAKE_SHARED_MODULE_CUDA_FLAGS ${CMAKE_SHARED_LIBRARY_CUDA_FLAGS})
  set(CMAKE_SHARED_MODULE_CREATE_CUDA_FLAGS ${CMAKE_SHARED_LIBRARY_CREATE_CUDA_FLAGS})
endif()

if(CMAKE_EXECUTABLE_FORMAT STREQUAL "ELF")
  if(NOT DEFINED CMAKE_CUDA_LINK_WHAT_YOU_USE_FLAG)
    set(CMAKE_CUDA_LINK_WHAT_YOU_USE_FLAG "-Wl,--no-as-needed")
  endif()
  if(NOT DEFINED CMAKE_LINK_WHAT_YOU_USE_CHECK)
    set(CMAKE_LINK_WHAT_YOU_USE_CHECK ldd -u -r)
  endif()
endif()

# add the flags to the cache based
# on the initial values computed in the platform/*.cmake files
# use _INIT variables so that this only happens the first time
# and you can set these flags in the cmake cache
#set(CMAKE_CUDA_FLAGS_INIT "$ENV{CUDAFLAGS} ")
#cmake_initialize_per_config_variable(CMAKE_CUDA_FLAGS "Flags used by the CUDA compiler")


set(CMAKE_CUDA_FLAGS_INIT "${CMAKE_CUDA_FLAGS_INIT} -include stdint.h ")
if (CMAKE_VERBOSE_MAKEFILE)
    set(CMAKE_CUDA_FLAGS_INIT "${CMAKE_CUDA_FLAGS_INIT} -v")
endif()
cmake_initialize_per_config_variable(CMAKE_CUDA_FLAGS "Flags used by the CUDA compiler")

if(CMAKE_CUDA_STANDARD_LIBRARIES_INIT)
  set(CMAKE_CUDA_STANDARD_LIBRARIES "${CMAKE_CUDA_STANDARD_LIBRARIES_INIT}"
    CACHE STRING "Libraries linked by default with all CUDA applications.")
  mark_as_advanced(CMAKE_CUDA_STANDARD_LIBRARIES)
endif()

if(NOT CMAKE_CUDA_COMPILER_LAUNCHER AND DEFINED ENV{CMAKE_CUDA_COMPILER_LAUNCHER})
  set(CMAKE_CUDA_COMPILER_LAUNCHER "$ENV{CMAKE_CUDA_COMPILER_LAUNCHER}"
    CACHE STRING "Compiler launcher for CUDA.")
endif()

include(CMakeCommonLanguageInclude)

# now define the following rules:
# CMAKE_CUDA_CREATE_SHARED_LIBRARY
# CMAKE_CUDA_CREATE_SHARED_MODULE
# CMAKE_CUDA_COMPILE_OBJECT
# CMAKE_CUDA_LINK_EXECUTABLE

# create a shared library
if(NOT CMAKE_CUDA_CREATE_SHARED_LIBRARY)
  set(CMAKE_CUDA_CREATE_SHARED_LIBRARY
      "<CMAKE_CUDA_HOST_LINK_LAUNCHER> -shared <CMAKE_SHARED_LIBRARY_CUDA_FLAGS> <LANGUAGE_COMPILE_FLAGS> <LINK_FLAGS> <CMAKE_SHARED_LIBRARY_CREATE_CUDA_FLAGS> <SONAME_FLAG><TARGET_SONAME> -o <TARGET> <OBJECTS> <LINK_LIBRARIES> ${__IMPLICIT_LINKS}")
else()
  set(CMAKE_CUDA_CREATE_SHARED_LIBRARY "${CMAKE_CUDA_CREATE_SHARED_LIBRARY} ${__IMPLICIT_LINKS}")
endif()

# create a shared module copy the shared library rule by default
if(NOT CMAKE_CUDA_CREATE_SHARED_MODULE)
  set(CMAKE_CUDA_CREATE_SHARED_MODULE "${CMAKE_CUDA_CREATE_SHARED_LIBRARY} ${__IMPLICIT_LINKS}")
else()
  set(CMAKE_CUDA_CREATE_SHARED_MODULE  "${CMAKE_CUDA_CREATE_SHARED_MODULE} ${__IMPLICIT_LINKS}")
endif()

# Create a static archive incrementally for large object file counts.
if(NOT DEFINED CMAKE_CUDA_ARCHIVE_CREATE)
  set(CMAKE_CUDA_ARCHIVE_CREATE "<CMAKE_AR> qc <TARGET> <LINK_FLAGS> <OBJECTS>")
endif()
if(NOT DEFINED CMAKE_CUDA_ARCHIVE_APPEND)
  set(CMAKE_CUDA_ARCHIVE_APPEND "<CMAKE_AR> q <TARGET> <LINK_FLAGS> <OBJECTS>")
endif()
if(NOT DEFINED CMAKE_CUDA_ARCHIVE_FINISH)
  set(CMAKE_CUDA_ARCHIVE_FINISH "<CMAKE_RANLIB> <TARGET>")
endif()

# compile a CUDA file into an object file
if(NOT CMAKE_CUDA_COMPILE_OBJECT)
  set(CMAKE_CUDA_COMPILE_OBJECT
    "<CMAKE_CUDA_COMPILER> -c ${_CMAKE_CUDA_EXTRA_FLAGS} <DEFINES> <INCLUDES> <FLAGS> -o <OBJECT> ${_CMAKE_COMPILE_AS_CUDA_FLAG} <SOURCE>")
endif()

# compile a cu file into an executable
if(NOT CMAKE_CUDA_LINK_EXECUTABLE)
  set(CMAKE_CUDA_LINK_EXECUTABLE
    "<CMAKE_CUDA_HOST_LINK_LAUNCHER> <FLAGS> <CMAKE_CUDA_LINK_FLAGS> <LINK_FLAGS> <OBJECTS> -o <TARGET> <LINK_LIBRARIES> ${__IMPLICIT_LINKS}")
else()
  set(CMAKE_CUDA_LINK_EXECUTABLE       "${CMAKE_CUDA_LINK_EXECUTABLE} ${__IMPLICIT_LINKS}")
endif()


# Add implicit host link directories that contain device libraries
# to the device link line.
set(__IMPLICIT_DLINK_DIRS ${CMAKE_CUDA_IMPLICIT_LINK_DIRECTORIES})
if(__IMPLICIT_DLINK_DIRS)
  list(REMOVE_ITEM __IMPLICIT_DLINK_DIRS ${CMAKE_CUDA_HOST_IMPLICIT_LINK_DIRECTORIES})
endif()
set(__IMPLICIT_DLINK_FLAGS)
foreach(dir ${__IMPLICIT_DLINK_DIRS})
  if(EXISTS "${dir}/libcurand_static.a")
    string(APPEND __IMPLICIT_DLINK_FLAGS " -L\"${dir}\"")
  endif()
endforeach()
unset(__IMPLICIT_DLINK_DIRS)


#These are used when linking relocatable (dc) cuda code
if(NOT CMAKE_CUDA_DEVICE_LINK_LIBRARY)
  set(CMAKE_CUDA_DEVICE_LINK_LIBRARY
    "<CMAKE_CUDA_COMPILER> ${_CMAKE_CUDA_EXTRA_FLAGS} <LANGUAGE_COMPILE_FLAGS> <LINK_FLAGS> ${CMAKE_CUDA_COMPILE_OPTIONS_PIC} ${_CMAKE_CUDA_EXTRA_DEVICE_LINK_FLAGS} -shared -dlink <OBJECTS> -o <TARGET> <LINK_LIBRARIES>${__IMPLICIT_DLINK_FLAGS}")
endif()
if(NOT CMAKE_CUDA_DEVICE_LINK_EXECUTABLE)
  set(CMAKE_CUDA_DEVICE_LINK_EXECUTABLE
    "<CMAKE_CUDA_COMPILER> ${_CMAKE_CUDA_EXTRA_FLAGS} <LANGUAGE_COMPILE_FLAGS> <LINK_FLAGS> ${CMAKE_CUDA_COMPILE_OPTIONS_PIC} ${_CMAKE_CUDA_EXTRA_DEVICE_LINK_FLAGS} -shared -dlink <OBJECTS> -o <TARGET> <LINK_LIBRARIES>${__IMPLICIT_DLINK_FLAGS}")
endif()

# Used when device linking is handled by CMake.
if(NOT CMAKE_CUDA_DEVICE_LINK_COMPILE)
  set(CMAKE_CUDA_DEVICE_LINK_COMPILE "<CMAKE_CUDA_COMPILER> ${_CMAKE_CUDA_EXTRA_FLAGS} <FLAGS> <LINK_FLAGS> -D__CUDA_INCLUDE_COMPILER_INTERNAL_HEADERS__ -D__NV_EXTRA_INITIALIZATION=\"\" -D__NV_EXTRA_FINALIZATION=\"\" -DREGISTERLINKBINARYFILE=\\\"<REGISTER_FILE>\\\" -DFATBINFILE=\\\"<FATBINARY>\\\" ${_CMAKE_COMPILE_AS_CUDA_FLAG} -c \"${CMAKE_CUDA_COMPILER_TOOLKIT_LIBRARY_ROOT}/bin/crt/link.stub\" -o <OBJECT>")
endif()

unset(__IMPLICIT_DLINK_FLAGS)

set(CMAKE_CUDA_INFORMATION_LOADED 1)

