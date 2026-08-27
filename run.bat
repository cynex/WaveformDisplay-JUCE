@echo off
setlocal

cd /d "%~dp0"

set "EXE=build\WaveformDisplay_artefacts\Release\WaveformDisplay.exe"

if not exist "%EXE%" (
    echo Release build not found at "%EXE%".
    echo Run build.bat first to configure and build it.
    exit /b 1
)

rem Any arguments passed to run.bat (e.g. an audio file path) are forwarded
rem to the app, which will load that file on startup.
start "" "%EXE%" %*

endlocal
