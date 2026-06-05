@echo off
setlocal
set "ROOT=%~dp0"
set "QTROOT=%ROOT%third_party\Qt\6.5.3\msvc2019_64"
set "PATH=%QTROOT%\bin;%PATH%"
set "QT_PLUGIN_PATH=%QTROOT%\plugins"
start "" "%ROOT%build\x64\Release\QtViewer.exe" %*
