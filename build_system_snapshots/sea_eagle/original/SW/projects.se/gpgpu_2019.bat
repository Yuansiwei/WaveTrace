setlocal
set ARCH=cc10200L_0066
set AQROOT=%cd%
set BUILDSOURCE=%AQROOT%
set AQARCH=%AQROOT%\arch\XAQ2
set AQVGARCH=%AQROOT%\arch\GC350
set VIVANTE_SDK_DIR=%AQROOT%\build\sdk
set CARCHDIR=%AQROOT%\vipArchPerfMdl_trunk
set PATH=%VIVANTE_SDK_DIR%\bin;%PATH%
"C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\Common7\IDE\devenv" gpgpu\gpgpu.sln
endlocal
