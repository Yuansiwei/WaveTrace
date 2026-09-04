md compute-build
cd compute-build
set base=%~dp0
set base=%base:\=/%

::rem gpu-sw-cd-04
set PATH=C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Tools\MSVC\14.29.30133\bin\Hostx64\x64;D:\GPU_3D\cn8185\work\cuda\cmake-3.28.1-windows-x86_64\bin;%PATH%

cmake -DGCDEFINE=gc9600L_0096 -DCMAKE_SYSTEM_VERSION=10.0.19041.0 -DLLVM_ROOT=D:/GPU_3D/public/llvm-17.0.4 ..
cmake --build . -j 20
cd ..
