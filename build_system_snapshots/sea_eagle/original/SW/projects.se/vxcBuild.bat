@echo off
@if "%3"=="" goto :usage
@goto :__main__

:usage
@setlocal
@echo.
@echo usage:
@echo ^ ^ ^ ^ %0 AQROOT buildConfig GPU_CONFIG [clean]
@echo. 
@echo ^ ^AQROOT: vivante driver root path"
@echo    buildConfig: win32^|x64"
@echo.
@echo.
@endlocal
@goto :end

:init
    echo ^=^= check GPU config file ...
    set GPU_CONFIG_FILE=%AQROOT%\compiler\vclcompiler\viv_gpu.config
    if "%GPU_CONFIG%"=="default" set GPU_CONFIG_FILE=%AQROOT%\compiler\vclcompiler\viv_gpu.config
	if not "%GPU_CONFIG%"=="default" set GPU_CONFIG_FILE=%AQROOT%\compiler\vclcompiler\viv_gpu_%GPU_CONFIG%.config
@goto :end


:build_offline_compiler
@setlocal
    echo build offline compiler ....
	set AQARCH=%AQROOT%\arch\XAQ2
	set VIVANTE_SDK_DIR=%AQROOT%\build\sdk.compiler.%BUILD_CONFIG%
	set PATH=%VIVANTE_SDK_DIR%\bin;%PATH%
	set VSBUILDCONFIG="vivante_release_no_kernel_static^|Win32"
	if "%BUILD_CONFIG%" == "x64" set VSBUILDCONFIG="vivante_release_no_kernel_static^|X64"
	set REG_VSBUILDCONFIG="All_Release^|Win32"
	if "%BUILD_CONFIG%" == "x64" set REG_VSBUILDCONFIG="All_Release^|X64"
	cd /d %AQROOT%
	sed.exe -i "s/;NO_KERNEL/;gcdDEBUG_OUTPUT_STDOUT;NO_KERNEL/g" %AQROOT%\hal\os\emulator\user\os.user.2012.vcxproj
	if "%5"=="" (
	"C:\Program Files (x86)\Microsoft Visual Studio 11.0\Common7\IDE\devenv" VIV_Drivers_2012.sln /Clean %VSBUILDCONFIG%
	"C:\Program Files (x86)\Microsoft Visual Studio 11.0\Common7\IDE\devenv" VIV_Drivers_2012.sln /Build %REG_VSBUILDCONFIG% /Project "GCCORE registers"
	"C:\Program Files (x86)\Microsoft Visual Studio 11.0\Common7\IDE\devenv" VIV_Drivers_2012.sln /Build %VSBUILDCONFIG% /Project libVSC
	rem "C:\Program Files (x86)\Microsoft Visual Studio 11.0\Common7\IDE\devenv" VIV_Drivers_2012.sln /Build %VSBUILDCONFIG% /Project glslentry
	"C:\Program Files (x86)\Microsoft Visual Studio 11.0\Common7\IDE\devenv" VIV_Drivers_2012.sln /Build %VSBUILDCONFIG% /Project llvm
	"C:\Program Files (x86)\Microsoft Visual Studio 11.0\Common7\IDE\devenv" VIV_Drivers_2012.sln /Build %VSBUILDCONFIG% /Project clcentry
	rem "C:\Program Files (x86)\Microsoft Visual Studio 11.0\Common7\IDE\devenv" VIV_Drivers_2012.sln /Build %VSBUILDCONFIG% /Project vCompiler
	"C:\Program Files (x86)\Microsoft Visual Studio 11.0\Common7\IDE\devenv" VIV_Drivers_2012.sln /Build %VSBUILDCONFIG% /Project vcCompiler
	)else (
	"C:\Program Files (x86)\Microsoft Visual Studio 11.0\Common7\IDE\devenv" VIV_Drivers_2012.sln 
	)
	if "%BUILD_CONFIG%"=="win32" copy /y %AQROOT%\output\vivante_release_no_kernel_static\vcCompiler.exe %AQROOT%
	if "%BUILD_CONFIG%"=="x64" copy /y %AQROOT%\x64\vivante_release_no_kernel_static\vcCompiler.exe %AQROOT%
	rem if "%BUILD_CONFIG%"=="win32" copy /y %AQROOT%\output\vivante_release_no_kernel_static\vCompiler.exe %AQROOT%
	rem if "%BUILD_CONFIG%"=="x64" copy /y %AQROOT%\x64\vivante_release_no_kernel_static\vCompiler.exe %AQROOT%
@endlocal
@goto :end

:check_vcCompiler
@setlocal
    echo ^=^= check vcCompiler tool ...
    if not exist %AQROOT%\vcCompiler.exe (
        echo ^=^= not found vcCompiler
        echo ^=^= build vcCompiler ...
        cd /d %AQROOT%
        call :build_offline_compiler
    )
@endlocal
@goto :end


:cleanup
@setlocal
    cd /d %AQROOT%
	del /s /q vcCompiler.exe
    cd /d %AQROOT%\driver\khronos\libOpenVX\driver\src
    del /s /q /f *.gcPGM *.vxgcSL *.vx vxc_*.h *.vx.log 1>nul
@endlocal
@goto :end

:exact_vx_shader
@setlocal
    echo ^=^= exact VXC shader files ...
    if not exist %AQROOT%\sdk\include (
       cd /d %AQROOT%\sdk
	   mklink /j include inc
	)

    cd /d %AQROOT%\driver\khronos\libOpenVX\driver\src
    if not exist .\vxc_binaries\%GPU_CONFIG% mkdir .\vxc_binaries\%GPU_CONFIG%
    del /s /q /f *.gcPGM *.vxgcSL *.vx vxc_*.h .\vxc_binaries\%GPU_CONFIG%\*.h 1>nul
    echo "python %AQROOT%\tools\bin\ExactVXC.py -i %AQROOT%\driver\khronos\libOpenVX\driver\src\gc_vx_layer.c"
    python %AQROOT%\tools\bin\ExactVXC.py -i gc_vx_layer.c
	cd /d %AQROOT%\driver\khronos\libOpenVX\driver\src
	dir /b *.vx > vxFiles.txt
	type vxFiles.txt
endlocal
goto :end

:convert_vxc_shader
setlocal
    echo ^=^= generating .\vxc_binaries\%GPU_CONFIG%\vxc_binaries.h ...
	cd /d %AQROOT%\driver\khronos\libOpenVX\driver\src
	@for /f "delims=." %%a in (vxFiles.txt) do (
        echo %AQROOT%\vcCompiler.exe -f %GPU_CONFIG_FILE% -O0 -allkernel %%a.vx
        %AQROOT%\vcCompiler -f%GPU_CONFIG_FILE% -O0 -allkernel %%a.vx -v > %%a.vx.log
		if exist %%a_all.gcPGM (
            echo python %AQROOT%\tools\bin\ConvertPGMToH.py -i %%a_all.gcPGM -o .\vxc_binaries\%GPU_CONFIG%\vxc_bin_%%a.h
            %VIVANTE_PROJECT_ROOT%\Python\python.exe %AQROOT%\tools\bin\ConvertPGMToH.py -i %%a_all.gcPGM -o .\vxc_binaries\%GPU_CONFIG%\vxc_bin_%%a.h
		) else (
		    type %%a.vx.log
		)
    )

	cd /d %AQROOT%\driver\khronos\libOpenVX\driver\src
    echo #ifndef __VXC_BINARIES_H__ > .\vxc_binaries\%GPU_CONFIG%\vxc_binaries.h
    echo #define __VXC_BINARIES_H__ >> .\vxc_binaries\%GPU_CONFIG%\vxc_binaries.h
    @for /f "delims=." %%a in (vxFiles.txt) do (
        echo #include "vxc_bin_%%a.h" >> .\vxc_binaries\%GPU_CONFIG%\vxc_binaries.h
    )

    echo #endif >> .\vxc_binaries\%GPU_CONFIG%\vxc_binaries.h
    echo.
	echo.

    cd %AQROOT%\driver\khronos\libOpenVX\driver\src
    del /s /q /f *.gcPGM *.vxgcSL *.vx vxc_*.h *.vx.log 1>nul
:end_convert
@endlocal
@goto :end

:__main__
@setlocal
	set AQROOT=%1
	set BUILD_CONFIG=%2
	set GPU_CONFIG=%3
	set MORE=%4
	set GPU_CONFIG_FILE=%AQROOT%\compiler\vclcompiler\viv_gpu.config

	set PATH=%AQROOT%\tools\bin\python;%PATH%
	if not exist %AQROOT%\VIV_Drivers_2012.sln (
		echo.
		echo ERROR: not found %AQROOT%\VIV_Drivers_2012.sln
		echo.
		goto :end_main
	)

	call :init
	if not exist %GPU_CONFIG_FILE% (
		echo.
		echo ERROR: not found GPU config file: %GPU_CONFIG_FILE%
                echo You can get GPU config file from here: //SW/Rel5x/configs/*.config
		echo.
		goto :end_main
	)
	echo ^=^= found GPU config file: %GPU_CONFIG_FILE%
	echo.

	call :check_vcCompiler

	if not exist %AQROOT%\vcCompiler.exe (
	    echo.
	    echo ERROR: not found vcCompiler tool: %AQROOT%\vcCompiler.exe
		echo.
		goto :end_main
    )

	call :exact_vx_shader

	set VIVANTE_SDK_DIR=%AQROOT%\sdk
	call :convert_vxc_shader
@:end_main
@endlocal
@goto :end
:end

