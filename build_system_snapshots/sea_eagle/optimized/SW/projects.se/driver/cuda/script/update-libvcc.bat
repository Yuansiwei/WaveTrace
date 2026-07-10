

set AQ_ROOT=
set COMPUTE_ROOT=

::copy libVCC.so
copy %COMPUTE_ROOT%\compiler\libVCCL\bin\win64\debug\libVCCL.dll %AQ_ROOT%\build\sdk\bin

::copy runtime header
xcopy %COMPUTE_ROOT%\compiler\libVCCL\runtime %AQ_ROOT%\build\sdk\include\runtime /E /I /Y
