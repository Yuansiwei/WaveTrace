@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0wpr_profile.ps1" %*
exit /b %ERRORLEVEL%
