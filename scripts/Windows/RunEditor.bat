@echo off
setlocal
REM Launch the Desert Editor built by BuildWindows.bat.
REM
REM Usage: scripts\Windows\RunEditor.bat [Debug^|Release] [editor args...]
REM        (everything after the config is forwarded to the Editor, e.g. --project <path>)

cd /d "%~dp0..\.."

set "CONFIG=%~1"
if "%CONFIG%"=="" set "CONFIG=Debug"
if not "%~1"=="" shift

set "EDITOR=%CD%\build\Bin\%CONFIG%\Editor.exe"

if not exist "%EDITOR%" (
    echo %EDITOR% not found — build first: scripts\Windows\BuildWindows.bat %CONFIG% 1>&2
    exit /b 1
)

REM Where this engine lives, so the Editor can record itself in %USERPROFILE%\.desertengine\engines.json
REM — the file the launcher reads to find an engine at all. Set BEFORE the `cd`, because after it
REM %CD% is the Editor folder.
set "DESERT_ROOT=%CD%"

REM The engine resolves Resources/... relative to the working directory.
cd Editor

REM The editor REQUIRES a project (--project <.deproj>); picking projects is the Project Hub's job.
REM With no extra args, fall back to the built-in sandbox project.
REM
REM %* is NOT affected by `shift` in cmd — it always expands to the ORIGINAL argument list, so
REM forwarding with %* handed the Editor the configuration name ("Debug --project X") as a stray
REM positional argument. Rebuild the forwarded tail by hand instead.
set "ARGS="
:collect
if "%~1"=="" goto run
set ARGS=%ARGS% %1
shift
goto collect
:run
if not defined ARGS (
    "%EDITOR%" --project Desert.deproj
) else (
    "%EDITOR%"%ARGS%
)
exit /b %ERRORLEVEL%
