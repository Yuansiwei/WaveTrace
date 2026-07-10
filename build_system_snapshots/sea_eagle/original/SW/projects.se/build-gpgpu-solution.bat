md gpgpu
cd gpgpu
set base=%~dp0
set base=%base:\=/%

set PATH=C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Tools\MSVC\14.29.30133\bin\Hostx64\x64;%PATH%
cmake -DGCDEFINE=cc10200L_0066 -DCMAKE_BUILD_TYPE=Debug -DCMAKE_SYSTEM_VERSION=10.0.19041.0 ..\driver\cuda
cd ..
