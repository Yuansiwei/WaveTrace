@echo off

D:\GPUCOMPILER\cn9543\SW\Compiler\SeaEagle\LLVM-Master\project\Debug\bin\llc.exe %1

if %errorlevel% neq 0 (
	exit /b 0
) else (
	exit /b 1
)
