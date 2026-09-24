@echo off
REM Generate Desert.sln for Visual Studio 2022 and open it -- no build.
REM
REM Usage: scripts\Windows\GenerateProjects.bat [--with-tests] [--no-open]
REM   --with-tests  also generate every test suite project into the solution
REM   --no-open     only write Desert.sln, do not start Visual Studio
REM
REM One source of truth for the generation step: this delegates to BuildWindows.bat --gen-only, which
REM finds premake5 (PATH, then vendor\bin) and fails loudly when the Vulkan SDK is missing -- run
REM scripts\Windows\Setup.bat once, from an ADMINISTRATOR console, before the first generation.
setlocal

set "GEN_ARGS=--gen-only"
set "OPEN=1"

:parse_args
if "%~1"=="" goto args_done
if /I "%~1"=="--with-tests" (set "GEN_ARGS=%GEN_ARGS% --with-tests" & shift & goto parse_args)
if /I "%~1"=="--no-open"    (set "OPEN=0" & shift & goto parse_args)
echo [ERROR] Unknown argument: %~1 1>&2
echo         Usage: scripts\Windows\GenerateProjects.bat [--with-tests] [--no-open] 1>&2
exit /b 1
:args_done

call "%~dp0BuildWindows.bat" %GEN_ARGS%
if errorlevel 1 exit /b 1

set "SLN=%~dp0..\..\Desert.sln"
if not exist "%SLN%" (
    echo [ERROR] premake5 reported success but Desert.sln is not in the repository root. 1>&2
    exit /b 1
)
echo --- Solution: %SLN%
if "%OPEN%"=="1" start "" "%SLN%"
exit /b 0
