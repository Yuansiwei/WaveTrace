include_guard(GLOBAL)

function(wavetrace_add_reflectgen)
	if(TARGET wavetrace_reflectgen)
		return()
	endif()
	if(NOT DEFINED WAVETRACE_ROOT)
		message(FATAL_ERROR "wavetrace_add_reflectgen requires WAVETRACE_ROOT to point at the WaveTrace package root")
	endif()

	if(NOT DEFINED WAVETRACE_REFLECTGEN_EXE)
		set(WAVETRACE_REFLECTGEN_EXE "${WAVETRACE_ROOT}/tools/bin/wavetrace_reflectgen.exe" CACHE FILEPATH "Prebuilt WaveTrace ReflectGen executable")
	endif()
	if(NOT EXISTS "${WAVETRACE_REFLECTGEN_EXE}")
		message(FATAL_ERROR "WaveTrace prebuilt ReflectGen executable was not found: ${WAVETRACE_REFLECTGEN_EXE}")
	endif()

	add_executable(wavetrace_reflectgen IMPORTED GLOBAL)
	set_target_properties(wavetrace_reflectgen PROPERTIES
		IMPORTED_LOCATION "${WAVETRACE_REFLECTGEN_EXE}")
	add_executable(WaveTrace::reflectgen ALIAS wavetrace_reflectgen)
endfunction()
