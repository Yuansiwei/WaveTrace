cmake_minimum_required(VERSION 3.20)

project(math LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(MATHLIB_DIR ${PROJECT_SOURCE_DIR}/../third_party/mathlib)

set(MATHLIB_CXX_FILES
    ${MATHLIB_DIR}/mathlib/vsiBitField.cpp
    ${MATHLIB_DIR}/mathlib/vsiMathFixed.cpp
    ${MATHLIB_DIR}/mathlib/vsiMathFloat.cpp
    ${MATHLIB_DIR}/mathlib/vsiMathMultiType.cpp
    ${MATHLIB_DIR}/mathlib/vsiMathTrans.cpp
)

set(ISA_DIR ${PROJECT_SOURCE_DIR}/../isa)

add_library(${PROJECT_NAME} STATIC
    ${ISA_DIR}/src/math.cpp
    ${MATHLIB_CXX_FILES}
)

target_include_directories(${PROJECT_NAME} PUBLIC
    ${MATHLIB_DIR}/mathlib
    ${ISA_DIR}/src
)

target_link_libraries(${PROJECT_NAME} PRIVATE
)

target_compile_definitions(${PROJECT_NAME} PRIVATE
)

if (NOT MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE
        -Wall
        -Wextra
        -Wno-unused-variable
        -Wno-unused-but-set-variable
        -Wno-unused-parameter
        -Wno-sign-compare
    )
endif()

# executable example

add_executable(main
    ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp
)

target_include_directories(main PRIVATE
    ${MATHLIB_DIR}/mathlib
    ${ISA_DIR}/src
)

target_link_libraries(main
    math
)
