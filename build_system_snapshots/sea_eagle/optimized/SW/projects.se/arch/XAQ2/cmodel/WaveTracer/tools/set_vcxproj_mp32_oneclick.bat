@echo off
setlocal

set "SCRIPT_DIR=%~dp0"
set "PS_SCRIPT=%SCRIPT_DIR%set_vcxproj_mp32.ps1"

if not exist "%PS_SCRIPT%" (
    echo Missing script: "%PS_SCRIPT%"
    echo Keep this .bat next to set_vcxproj_mp32.ps1.
    pause
    exit /b 1
)

if "%~1"=="" (
    set "TARGET_ROOT=%CD%"
) else (
    set "TARGET_ROOT=%~1"
)

echo WaveTrace /MP32 updater
echo Target root: "%TARGET_ROOT%"
echo.

powershell -NoProfile -ExecutionPolicy Bypass -File "%PS_SCRIPT%" -Root "%TARGET_ROOT%" -Threads 32 -Backup
set "ERR=%ERRORLEVEL%"

echo.
if not "%ERR%"=="0" (
    echo Failed with exit code %ERR%.
) else (
    echo Done. Backups were written as *.vcxproj.bak.
)
pause
exit /b %ERR%
