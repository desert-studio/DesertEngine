@echo off
setlocal
REM Launch the Desert Runtime (standalone player) for a project.
REM
REM Usage: scripts\Windows\RunRuntime.bat [Debug^|Release] [--project <.deproj>] [--scene <.desce>]
REM        (with no --project, falls back to the built-in sandbox project)

cd /d "%~dp0..\.."

set "CONFIG=%~1"
if "%CONFIG%"=="" set "CONFIG=Debug"
if not "%~1"=="" shift

set "RUNTIME=%CD%\build\Bin\%CONFIG%\Runtime.exe"

if not exist "%RUNTIME%" (
    echo %RUNTIME% not found — build first: scripts\Windows\BuildWindows.bat %CONFIG% 1>&2
    exit /b 1
)

REM Engine resources (shaders/fonts) resolve relative to the working directory — same tree the editor uses.
cd Editor

if "%~1"=="" (
    "%RUNTIME%" --project Desert.deproj
) else (
    "%RUNTIME%" %*
)
exit /b %ERRORLEVEL%
