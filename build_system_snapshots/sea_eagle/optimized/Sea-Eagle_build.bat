@echo off
setlocal EnableExtensions
title Sea-Eagle vec1_cl_build

if not defined WORKDIR set "WORKDIR=D:\Users\cn1842\workspace\cn1842_SeaEagle0"
if not defined CMAKE_EXE set "CMAKE_EXE=D:\Users\cn1842\cmake-3.28.1-windows-x86_64\bin\cmake.exe"
if not defined BUILD_CONFIG set "BUILD_CONFIG=Release"
if not defined BUILD_PARALLEL set "BUILD_PARALLEL=32"
if not defined GENERATE_ONLY set "GENERATE_ONLY=1"

set "AQROOT=%WORKDIR%\SW\projects.se"
set "AQARCH=%AQROOT%\arch\XAQ2"
set "AQTOOLS=%WORKDIR%\TOOLS"
set "BUILD_DIR=%WORKDIR%\vec1_cl_build"

echo [INFO] WORKDIR=%WORKDIR%
echo [INFO] BUILD_CONFIG=%BUILD_CONFIG%
echo [INFO] BUILD_PARALLEL=%BUILD_PARALLEL%
echo [INFO] GENERATE_ONLY=%GENERATE_ONLY%
echo [INFO] CMAKE_EXE=%CMAKE_EXE%
echo.

if not exist "%CMAKE_EXE%" (
  echo [ERROR] CMake executable not found: "%CMAKE_EXE%"
  goto :Fail
)

call :RemoveLink "%AQROOT%\tools" || goto :Fail
call :RemoveLink "%AQROOT%\arch" || goto :Fail
call :RemoveLink "%AQROOT%\driver\cuda\mathlib" || goto :Fail

call :MakeLink "%AQROOT%\tools" "%WORKDIR%\TOOLS" || goto :Fail
call :MakeLink "%AQROOT%\arch" "%WORKDIR%\HW\projects.se\arch" || goto :Fail
call :MakeLink "%AQROOT%\driver\cuda\mathlib" "%WORKDIR%\TOOLS\GcTest\mathlib" || goto :Fail

set PATH=C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Tools\MSVC\14.29.30037\lib\x64;%PATH%

echo [INFO] Configuring CMake...
"%CMAKE_EXE%" -S "%AQROOT%\driver\cuda" -B "%BUILD_DIR%" ^
  -G "Visual Studio 16 2019" -A x64 -T host=x64 ^
  -DGCDEFINE=cc10200L_0066 ^
  -DCMAKE_SYSTEM_VERSION=10.0.19041.0 ^
  -DCMAKE_CONFIGURATION_TYPES="Debug;Release;RelWithDebInfo;MinSizeRel;Profile" ^
  -DVEC1_MSVC_MP_COUNT=%BUILD_PARALLEL% ^
  -DVEC1_RELEASE_DEBUG_INFO=ON
if errorlevel 1 goto :Fail

if /I "%GENERATE_ONLY%"=="1" (
  echo.
  echo [OK] Generated solution in %BUILD_DIR%
  echo [INFO] Open solution: %BUILD_DIR%\gpgpu.sln
  echo [INFO] To build from this bat later, run:
  echo        set GENERATE_ONLY=0
  echo        set BUILD_CONFIG=Release
  echo        Sea-Eagle_build.bat
  set "RESULT=0"
  goto :Done
)

echo.
echo [INFO] Building %BUILD_CONFIG%...
"%CMAKE_EXE%" --build "%BUILD_DIR%" --config "%BUILD_CONFIG%" --parallel %BUILD_PARALLEL%
if errorlevel 1 goto :Fail

echo.
echo [OK] Built %BUILD_CONFIG% in %BUILD_DIR%
set "RESULT=0"
goto :Done

:Fail
set "RESULT=%errorlevel%"
if "%RESULT%"=="0" set "RESULT=1"
echo.
echo [ERROR] Build failed. Exit code: %RESULT%
echo [ERROR] Window is kept open so the message can be copied.
goto :Done

:Done
echo.
pause
endlocal & exit /b %RESULT%

:RemoveLink
if not exist "%~1" exit /b 0
fsutil reparsepoint query "%~1" >nul 2>nul
if errorlevel 1 (
  echo [ERROR] Refusing to remove non-link path: "%~1"
  exit /b 1
)
rmdir "%~1"
exit /b %errorlevel%

:MakeLink
if not exist "%~2" (
  echo [ERROR] Missing link target: "%~2"
  exit /b 1
)
mklink /d "%~1" "%~2"
exit /b %errorlevel%
