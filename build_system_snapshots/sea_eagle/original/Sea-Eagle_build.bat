set WORKDIR=D:\Users\cn1842\workspace\cn1842_SeaEagle0

set AQROOT=%WORKDIR%\SW\projects.se
set AQARCH=%WORKDIR%\SW\projects.se\arch\XAQ2
set AQTOOLS=%WORKDIR%\TOOLS
rmdir	/s /q  %AQROOT%\tools
rmdir	/s /q  %AQROOT%\arch
rmdir	/s /q  %AQROOT%\driver\cuda\mathlib

mklink /d %AQROOT%\tools									%WORKDIR%\TOOLS
mklink /d %AQROOT%\arch									    %WORKDIR%\HW\projects.se\arch
mklink /d %AQROOT%\driver\cuda\mathlib						%WORKDIR%\TOOLS\GcTest\mathlib

md %WORKDIR%\vec1_cl_build
cd %WORKDIR%\vec1_cl_build
set base=%~dp0
set base=%base:\=/%

set PATH=C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Tools\MSVC\14.29.30037\lib\x64;%PATH%

D:\Users\cn1842\cmake-3.28.1-windows-x86_64\bin\cmake.exe -DGCDEFINE=cc10200L_0066 -DCMAKE_SYSTEM_VERSION=10.0.19041.0 %AQROOT%\driver\cuda -DCMAKE_BUILD_TYPE=Release

cd ..
pause
