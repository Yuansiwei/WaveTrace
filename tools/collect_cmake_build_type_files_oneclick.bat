@echo off
setlocal
title WaveTrace CMake/VS Collector

echo ===============================================
echo WaveTrace CMake/VS build source collector
echo ===============================================
echo.

set "SCRIPT=%~dp0collect_cmake_build_type_files.ps1"
if not exist "%SCRIPT%" (
  echo Missing script: "%SCRIPT%"
  pause
  exit /b 1
)

if "%~1"=="" (
  set "TARGET_ROOT=%CD%"
) else (
  set "TARGET_ROOT=%~1"
)

echo Target root:
echo   "%TARGET_ROOT%"
echo.
echo Running... this may take a while on a large workspace.
echo The report txt will be opened automatically when finished.
echo.

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT%" -Root "%TARGET_ROOT%" -OpenOutput
set "ERR=%ERRORLEVEL%"
if not "%ERR%"=="0" (
  echo collect failed, error=%ERR%
  pause
  exit /b %ERR%
)
echo.
echo Done.
pause
exit /b 0
