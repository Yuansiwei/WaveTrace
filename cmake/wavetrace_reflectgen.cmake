include_guard(GLOBAL)

function(wavetrace_add_reflectgen)
	if(TARGET wavetrace_reflectgen)
		return()
	endif()
	if(NOT DEFINED WAVETRACE_ROOT OR NOT EXISTS "${WAVETRACE_ROOT}/ReflectGen.cpp")
		message(FATAL_ERROR "wavetrace_add_reflectgen requires WAVETRACE_ROOT to point at the WaveTrace package root")
	endif()

	add_executable(wavetrace_reflectgen
		"${WAVETRACE_ROOT}/ReflectGen.cpp")
	add_executable(WaveTrace::reflectgen ALIAS wavetrace_reflectgen)

	set_target_properties(wavetrace_reflectgen PROPERTIES
		OUTPUT_NAME "ReflectGen")
	target_compile_features(wavetrace_reflectgen PRIVATE cxx_std_14)
	target_include_directories(wavetrace_reflectgen PRIVATE
		"${WAVETRACE_ROOT}")
	target_compile_definitions(wavetrace_reflectgen PRIVATE
		_CRT_SECURE_NO_WARNINGS
		NOMINMAX)

	if(WIN32)
		set(_wavetrace_llvm_root "${WAVETRACE_ROOT}/third_party/llvm/llvm-local")
		if(NOT EXISTS "${_wavetrace_llvm_root}/include/clang-c/Index.h")
			message(FATAL_ERROR "WaveTrace local LLVM include directory was not found: ${_wavetrace_llvm_root}/include")
		endif()
		if(NOT EXISTS "${_wavetrace_llvm_root}/lib/libclang.lib")
			message(FATAL_ERROR "WaveTrace local libclang import library was not found: ${_wavetrace_llvm_root}/lib/libclang.lib")
		endif()
		if(NOT EXISTS "${_wavetrace_llvm_root}/bin/libclang.dll")
			message(FATAL_ERROR "WaveTrace local libclang runtime was not found: ${_wavetrace_llvm_root}/bin/libclang.dll")
		endif()
		target_include_directories(wavetrace_reflectgen PRIVATE
			"${_wavetrace_llvm_root}/include")
		target_link_directories(wavetrace_reflectgen PRIVATE
			"${_wavetrace_llvm_root}/lib")
		target_link_libraries(wavetrace_reflectgen PRIVATE libclang.lib)
		add_custom_command(TARGET wavetrace_reflectgen POST_BUILD
			COMMAND ${CMAKE_COMMAND} -E copy_if_different
				"${_wavetrace_llvm_root}/bin/libclang.dll"
				"$<TARGET_FILE_DIR:wavetrace_reflectgen>/libclang.dll"
			VERBATIM)
	else()
		find_path(WAVETRACE_LIBCLANG_INCLUDE_DIR clang-c/Index.h)
		find_library(WAVETRACE_LIBCLANG_LIBRARY clang)
		if(NOT WAVETRACE_LIBCLANG_INCLUDE_DIR OR NOT WAVETRACE_LIBCLANG_LIBRARY)
			message(FATAL_ERROR "libclang was not found. Set WAVETRACE_LIBCLANG_INCLUDE_DIR and WAVETRACE_LIBCLANG_LIBRARY.")
		endif()
		target_include_directories(wavetrace_reflectgen PRIVATE
			"${WAVETRACE_LIBCLANG_INCLUDE_DIR}")
		target_link_libraries(wavetrace_reflectgen PRIVATE
			"${WAVETRACE_LIBCLANG_LIBRARY}")
	endif()
endfunction()
