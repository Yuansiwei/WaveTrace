@echo off
setlocal EnableExtensions
title build-gpgpu-solution

if not defined BUILD_CONFIG set "BUILD_CONFIG=Debug"
if not defined BUILD_PARALLEL set "BUILD_PARALLEL=32"
if not defined GENERATE_ONLY set "GENERATE_ONLY=1"

set "BASE=%~dp0"
set "SOURCE_DIR=%BASE%driver\cuda"
set "BUILD_DIR=%BASE%gpgpu"

set PATH=C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Tools\MSVC\14.29.30133\bin\Hostx64\x64;%PATH%

echo [INFO] SOURCE_DIR=%SOURCE_DIR%
echo [INFO] BUILD_DIR=%BUILD_DIR%
echo [INFO] BUILD_CONFIG=%BUILD_CONFIG%
echo [INFO] BUILD_PARALLEL=%BUILD_PARALLEL%
echo [INFO] GENERATE_ONLY=%GENERATE_ONLY%
echo.

cmake -S "%SOURCE_DIR%" -B "%BUILD_DIR%" ^
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
  echo        set BUILD_CONFIG=Debug
  echo        build-gpgpu-solution.bat
  set "RESULT=0"
  goto :Done
)

cmake --build "%BUILD_DIR%" --config "%BUILD_CONFIG%" --parallel %BUILD_PARALLEL%
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
