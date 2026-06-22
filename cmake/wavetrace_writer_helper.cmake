include_guard(GLOBAL)

function(wavetrace_add_writer_helper)
	if(TARGET wavetrace_writer_monitor)
		return()
	endif()
	if(NOT DEFINED WAVETRACE_ROOT)
		message(FATAL_ERROR "wavetrace_add_writer_helper requires WAVETRACE_ROOT to point at the WaveTrace package root")
	endif()

	if(NOT DEFINED WAVETRACE_WRITER_HELPER_EXE)
		set(WAVETRACE_WRITER_HELPER_EXE "${WAVETRACE_ROOT}/tools/bin/wvz4_writer_monitor.exe" CACHE FILEPATH "Prebuilt WaveTrace writer helper executable")
	endif()
	if(NOT EXISTS "${WAVETRACE_WRITER_HELPER_EXE}")
		message(FATAL_ERROR "WaveTrace prebuilt writer helper executable was not found: ${WAVETRACE_WRITER_HELPER_EXE}")
	endif()

	add_executable(wavetrace_writer_monitor IMPORTED GLOBAL)
	set_target_properties(wavetrace_writer_monitor PROPERTIES
		IMPORTED_LOCATION "${WAVETRACE_WRITER_HELPER_EXE}")
	add_executable(WaveTrace::writer_helper ALIAS wavetrace_writer_monitor)
endfunction()

function(wavetrace_target_needs_writer_helper target_name)
	if(NOT TARGET "${target_name}")
		message(FATAL_ERROR "wavetrace_target_needs_writer_helper target does not exist: ${target_name}")
	endif()
	wavetrace_add_writer_helper()

	get_target_property(_wavetrace_target_type "${target_name}" TYPE)
	if(_wavetrace_target_type STREQUAL "EXECUTABLE")
		add_custom_command(TARGET "${target_name}" POST_BUILD
			COMMAND ${CMAKE_COMMAND} -E copy_if_different
				"$<TARGET_FILE:wavetrace_writer_monitor>"
				"$<TARGET_FILE_DIR:${target_name}>/$<TARGET_FILE_NAME:wavetrace_writer_monitor>"
			VERBATIM)
	endif()
endfunction()
