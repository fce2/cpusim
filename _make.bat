@echo off
setlocal EnableExtensions
rem Reset PATH + clear VS accumulator vars to avoid "line too long" in vcvars64
set "PATH=C:\Windows\System32;C:\Windows;C:\Windows\System32\Wbem;C:\Program Files (x86)\Microsoft Visual Studio\Installer;C:\cygwin64\bin;C:\Users\g\claude\c64\oscar64\bin;C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.2\bin"
set "INCLUDE="
set "LIB="
set "LIBPATH="
call "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 echo WARNING: CUDA/MSVC not available, CUDA builds will fail
make -j 10 %*
set "MAINRC=%errorlevel%"
rem CUDA c-variants (6502T/main001c..3c) are built by nvcc, independent of the g++ `all` above.
rem Run them whenever nvcc is present + no explicit target arg, even if a sibling subdir (e.g. 6502M) failed.
if not "%~1"=="" goto :skipcuda
where nvcc.exe >nul 2>nul
if errorlevel 1 (echo WARNING: nvcc not found, CUDA c-variants skipped & goto :skipcuda)
make -C 6502T cuda
:skipcuda
endlocal & exit /b %MAINRC%