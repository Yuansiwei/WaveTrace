cmake_minimum_required(VERSION 3.20)

project(isa_test LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 14)

set(ISA_DIR ../../src)
set(MATHLIB_DIR ../../../../../../tools/GcTest/mathlib)


set(ISA_SRC_FILES 
                ${ISA_DIR}/bitmanip.cpp
                ${ISA_DIR}/decoder.cpp
                ${ISA_DIR}/exec_context.cpp
                ${ISA_DIR}/instr_impl.cpp
                ${ISA_DIR}/instr_tc.cpp
                ${ISA_DIR}/op_encodings.cpp
                ${ISA_DIR}/math.cpp
)
message(STATUS " (log) isa source files: ${ISA_SRC_FILES}")

set(MATHLIB_SRC_FILES 
	${MATHLIB_DIR}/vsiBitField.cpp
	${MATHLIB_DIR}/vsiMathFixed.cpp
	${MATHLIB_DIR}/vsiMathFloat.cpp
	${MATHLIB_DIR}/vsiMathMultiType.cpp
	${MATHLIB_DIR}/vsiMathTrans.cpp
)
message(STATUS " (log) mathlib source files: ${MATHLIB_SRC_FILES}")


set_property(GLOBAL PROPERTY USE_FOLDERS ON)

# create the static library.
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR})
message(STATUS " (log) lib file: ${CMAKE_ARCHIVE_OUTPUT_DIRECTORY}")

add_library(mathlib
	STATIC
	${MATHLIB_SRC_FILES}
)
target_include_directories(mathlib
    PUBLIC
	${MATHLIB_DIR}
)

add_library(isa
	STATIC
	${ISA_SRC_FILES}
)
target_include_directories(isa
    PUBLIC
	${ISA_DIR}
)
target_link_libraries(isa
	PUBLIC
	mathlib
)

add_subdirectory(common)
add_subdirectory(mma)
add_subdirectory(bmma)
add_subdirectory(umma)
add_subdirectory(tnsldg)
add_subdirectory(tnsstg)
add_subdirectory(blkldg)
add_subdirectory(blkstg)
add_subdirectory(blkred)

set_target_properties(mma PROPERTIES FOLDER "test")
set_target_properties(bmma PROPERTIES FOLDER "test")
set_target_properties(umma PROPERTIES FOLDER "test")
set_target_properties(tnsldg PROPERTIES FOLDER "test")
set_target_properties(tnsstg PROPERTIES FOLDER "test")
set_target_properties(blkldg PROPERTIES FOLDER "test")
set_target_properties(blkstg PROPERTIES FOLDER "test")
set_target_properties(blkred PROPERTIES FOLDER "test")
