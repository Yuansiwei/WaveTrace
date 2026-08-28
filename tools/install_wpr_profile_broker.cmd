@echo off
setlocal

fltmc >nul 2>&1
if errorlevel 1 (
    powershell.exe -NoProfile -ExecutionPolicy Bypass -Command ^
      "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b
)

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0install_wpr_profile_broker.ps1"
set "install_exit=%ERRORLEVEL%"
echo.
if "%install_exit%"=="0" (
    echo WaveTrace WPR profiler permission broker installed successfully.
) else (
    echo Installation failed with exit code %install_exit%.
)
pause
exit /b %install_exit%
