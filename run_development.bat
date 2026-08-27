@echo off
setlocal

cd /d "%~dp0"

set "EXE=build\WaveformDisplay_artefacts\Debug\WaveformDisplay.exe"

if not exist "%EXE%" (
    echo Debug build not found at "%EXE%".
    echo Run build_development.bat first to configure and build it.
    exit /b 1
)

rem Any arguments passed to run_development.bat (e.g. an audio file path)
rem are forwarded to the app, which will load that file on startup.
start "" "%EXE%" %*

endlocal
