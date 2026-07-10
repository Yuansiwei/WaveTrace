:: Update "C:\Users\XYZ\P4WS" path to your local directory path before running the script.



set AQROOT=C:\Users\XYZ\P4WS\SW\Rel7x\dev7.0.x\projects.dev

set AQARCH=C:\Users\XYZ\P4WS\HW\projects.v730\arch\XAQ2



set VIVANTE_SDK_DIR=%AQROOT%\build

set path=%VIVANTE_SDK_DIR%\bin;%path%



START "" "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\Common7\IDE\devenv.exe" %AQROOT%\VIV_Drivers.sln



TIMEOUT 3



START "" "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\Common7\IDE\devenv.exe" %AQROOT%\..\..\..\..\TEST\SW\sdk\samples\vdk\es20\tutorial1\tutorial1.2019.sln

