@echo off
setlocal
cd /d "%~dp0"

echo ============================================================
echo Qt5 one-click v3: download + localize + patch project
echo.
echo Default:
echo   Qt 5.15.2 win64_msvc2019_64
echo   with Debug libraries
echo   clean build folders
echo.
echo Put qt5_oneclick.bat and qt5_oneclick.ps1 in project root.
echo Then double-click this bat.
echo ============================================================
echo.

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0qt5_oneclick.ps1"

set RC=%ERRORLEVEL%
echo.
if "%RC%"=="0" (
    echo [OK] Qt5 one-click finished.
    echo.
    echo Next:
    echo   1. Close Visual Studio.
    echo   2. Reopen the solution.
    echo   3. Build x64.
) else (
    echo [ERROR] Qt5 one-click failed.
)
echo.
pause
exit /b %RC%
