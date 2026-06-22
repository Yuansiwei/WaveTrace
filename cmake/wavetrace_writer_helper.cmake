include_guard(GLOBAL)

function(wavetrace_add_writer_helper)
	if(TARGET wavetrace_writer_monitor)
		return()
	endif()
	if(NOT DEFINED WAVETRACE_ROOT OR NOT EXISTS "${WAVETRACE_ROOT}/wvz4_writer_monitor_main.cpp")
		message(FATAL_ERROR "wavetrace_add_writer_helper requires WAVETRACE_ROOT to point at the WaveTrace package root")
	endif()

	add_executable(wavetrace_writer_monitor
		"${WAVETRACE_ROOT}/wvz4_writer_monitor_main.cpp"
		"${WAVETRACE_ROOT}/wvz4_writer_typed.h")
	add_executable(WaveTrace::writer_helper ALIAS wavetrace_writer_monitor)

	set_target_properties(wavetrace_writer_monitor PROPERTIES
		OUTPUT_NAME "wvz4_writer_monitor")
	target_compile_features(wavetrace_writer_monitor PRIVATE cxx_std_14)
	target_include_directories(wavetrace_writer_monitor PRIVATE
		"${WAVETRACE_ROOT}")
	target_compile_definitions(wavetrace_writer_monitor PRIVATE
		_CRT_SECURE_NO_WARNINGS
		NOMINMAX)

	if(DEFINED WAVETRACE_ZSTD_ROOT AND EXISTS "${WAVETRACE_ZSTD_ROOT}/lib/zstd.h")
		target_include_directories(wavetrace_writer_monitor PRIVATE
			"${WAVETRACE_ZSTD_ROOT}/lib")
	endif()
	if(TARGET WaveTrace::zstd)
		target_link_libraries(wavetrace_writer_monitor PRIVATE WaveTrace::zstd)
	else()
		target_compile_definitions(wavetrace_writer_monitor PRIVATE WVZ4_NO_ZSTD)
	endif()
endfunction()

function(wavetrace_target_needs_writer_helper target_name)
	if(NOT TARGET "${target_name}")
		message(FATAL_ERROR "wavetrace_target_needs_writer_helper target does not exist: ${target_name}")
	endif()
	wavetrace_add_writer_helper()
	add_dependencies("${target_name}" wavetrace_writer_monitor)

	get_target_property(_wavetrace_target_type "${target_name}" TYPE)
	if(_wavetrace_target_type STREQUAL "EXECUTABLE")
		add_custom_command(TARGET "${target_name}" POST_BUILD
			COMMAND ${CMAKE_COMMAND} -E copy_if_different
				"$<TARGET_FILE:wavetrace_writer_monitor>"
				"$<TARGET_FILE_DIR:${target_name}>/$<TARGET_FILE_NAME:wavetrace_writer_monitor>"
			VERBATIM)
	endif()
endfunction()
