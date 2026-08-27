@echo off
setlocal

cd /d "%~dp0"

echo === Configuring WaveformDisplay (CMake) ===
cmake -B build -S . -DCMAKE_BUILD_TYPE=Debug
if errorlevel 1 (
    echo CMake configuration failed.
    exit /b 1
)

echo === Building WaveformDisplay (Debug) ===
cmake --build build --config Debug
if errorlevel 1 (
    echo Build failed.
    exit /b 1
)

echo === Build succeeded ===
echo Executable should be under build\WaveformDisplay_artefacts\Debug\
endlocal
