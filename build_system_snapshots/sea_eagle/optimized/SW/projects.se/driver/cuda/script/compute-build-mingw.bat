md compute-build-mingw
cd compute-build-mingw
set base=%~dp0
set base=%base:\=/%


::rem gpu-sw-cd-04
set PATH=C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Tools\MSVC\14.29.30133\bin\Hostx64\x64;D:\GPU_3D\public\tools\x86_64-13.2.0-release-win32-seh-msvcrt-rt_v11-rev1\mingw64\bin;D:\GPU_3D\cn8185\work\cuda\cmake-3.28.1-windows-x86_64\bin;%PATH%

cmake -G "CodeLite - MinGW Makefiles" -DCMAKE_BUILD_TYPE=Debug -DGCDEFINE=gc9600L_0096 ..
cmake --build . -j 20
cd ..
