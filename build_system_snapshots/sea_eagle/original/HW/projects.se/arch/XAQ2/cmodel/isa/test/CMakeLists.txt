cmake_minimum_required(VERSION 3.20)

project(isa_test LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 14)

set(ISA_DIR  ${PROJECT_SOURCE_DIR}/isa)
set(ISA_SOURCE_DIR ${ISA_DIR}/src)

set(ISA_CXX_FILES
    ${ISA_SOURCE_DIR}/bitmanip.cpp
    ${ISA_SOURCE_DIR}/decoder.cpp
    ${ISA_SOURCE_DIR}/exec_context.cpp
    ${ISA_SOURCE_DIR}/instr_impl.cpp
    ${ISA_SOURCE_DIR}/instr_tc.cpp
    ${ISA_SOURCE_DIR}/math.cpp
    ${ISA_SOURCE_DIR}/op_encodings.cpp
)

add_library(isa STATIC
    ${ISA_CXX_FILES}
)

# third_party libraries
set(GTEST_SRC_HOME ${PROJECT_SOURCE_DIR}/third_party/googletest-1.16.0)
set(MATHLIB_SRC_HOME ${PROJECT_SOURCE_DIR}/third_party/mathlib)

add_subdirectory(${MATHLIB_SRC_HOME})

target_include_directories(isa PUBLIC
    ${ISA_SOURCE_DIR}
)

target_link_libraries(isa
    PUBLIC mathlib
)

target_compile_definitions(isa PRIVATE
)

target_compile_options(isa PRIVATE
)

# clang-tidy command
#set(CXX_CLANG_TIDY_COMMAND
#    clang-tidy;
#    -p ${PROJECT_BINARY_DIR};
#    --config-file=${PROJECT_SOURCE_DIR}/.clang-tidy;
#)

# enable clang-tidy
#set_target_properties(${PROJECT_NAME} PROPERTIES
#    CXX_CLANG_TIDY "${CXX_CLANG_TIDY_COMMAND}"
#)

# enable test
enable_testing()

add_subdirectory(${GTEST_SRC_HOME})

add_subdirectory(common)
add_subdirectory(instr)
