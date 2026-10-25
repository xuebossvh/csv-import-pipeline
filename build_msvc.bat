@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat"
cd C:\Users\wps\.wpscomate\agent\workspace\csv-import-pipeline
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 2>&1
cmake --build build --config Release -j 2>&1