@echo off
setlocal EnableDelayedExpansion
REM Configure (cmake --preset) and build Desert Engine on Windows with MSVC and Ninja.
REM
REM Usage:
REM   scripts\Windows\BuildWindows.bat [Debug^|Release^|Shipping] [--unity] [--gen-only] [--no-analyze]
REM                                     [--no-tests] [--launcher ^<exe^>] [--project ^<Target^>]...
REM
REM Examples:
REM   scripts\Windows\BuildWindows.bat                  Debug build, static analysis on
REM   scripts\Windows\BuildWindows.bat Release          Release build
REM   scripts\Windows\BuildWindows.bat Shipping         what a player gets: no capture, no profiler, no counters
REM   scripts\Windows\BuildWindows.bat Release --unity  the CI speed build ^(preset windows-msvc-ninja-unity^)
REM
REM The test suites are configured unless --no-tests is passed ^(CMakePresets.json `base`: DESERT_WITH_TESTS=ON^).
REM DESERT_WITH_TESTS is passed on EVERY configure, ON or OFF, so one --no-tests run cannot stick to the
REM build tree's cache and silently drop the suites from the next run.
REM --launcher ^<exe^> sets DESERT_COMPILER_LAUNCHER ^(CMAKE_^<LANG^>_COMPILER_LAUNCHER, CI's ccache^); without it
REM CMakeLists.txt looks for sccache/ccache on PATH on the first configure.
REM --project ^<Target^> builds that CMake target instead of everything; repeatable ^(BuildMacOS.sh's --project=^).
REM The MSVC environment comes from vcvars64.bat of the Visual Studio instance vswhere reports when
REM cl.exe is not already on PATH, so a plain cmd works as well as a Developer Command Prompt.
REM   scripts\Windows\BuildWindows.bat Debug --no-analyze   skip clang-tidy, and SAY so
REM
REM THE NAME USED TO BE A LIE, AND FOUR SCRIPTS SHARED THE CONFUSION. BuildWindows.bat once only
REM generated a solution and built nothing — while CompileWindows.bat was the one
REM that compiled, BuildWindowsWithTests.bat was the same generation with one flag, and
REM BuildWindows_CMake.bat pointed a CMake generator at a directory with no CMakeLists.txt in it.
REM Nothing in those names said which of them a person should run. This script now does what
REM scripts/MacOS/BuildMacOS.sh does, with the same arguments, and the other three are gone.
REM
REM There is also no PAUSE here. Two of the deleted scripts ended in one, which is fine for a
REM double-click and hangs for ever when another script or CI calls it.

cd /d "%~dp0..\.."
set "ROOT=%CD%"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"

set "CONFIG=Debug"
set "PRESET=windows-msvc-ninja"
set "GEN_ONLY=0"
set "ANALYZE=1"
set "WITH_TESTS=ON"
set "LAUNCHER_ARG="
set "TARGETS="

REM WHERE THE ANALYSER ATTACHES. One path, named once. Its exit codes ARE its interface — 0 clean,
REM 1 findings, 2 the gate could not run — so this script reads its status and never its output: a
REM gate that cannot run prints nothing, which is byte-identical to a gate that found nothing.
REM
REM SPELLED THROUGH %ROOT% AND NOT %~dp0..\, deliberately: Desert/Tests/Tools/BuildScriptContract
REM checks that every "scripts/..." path written anywhere under scripts/ resolves to a real file, and
REM it finds those paths by their text. A path assembled out of %~dp0 and ".." is invisible to that
REM check — it would be a reference no gate can see, which is the thing the gate exists to prevent.
set "ANALYZER=%ROOT%\scripts\CI\CheckTidy.bat"

:parse_args
if "%~1"=="" goto args_done
if /I "%~1"=="Debug"        (set "CONFIG=Debug"          & shift & goto parse_args)
if /I "%~1"=="Release"      (set "CONFIG=Release"        & shift & goto parse_args)
REM SHIPPING WAS REJECTED HERE, ON THE PLATFORM THE GAME SHIPS FOR. The configuration arrived with its
REM macOS wrapper taught the word (scripts/MacOS/BuildMacOS.sh) and this one not, so
REM `BuildWindows.bat Shipping` answered "[ERROR] Unknown argument: Shipping" -- while the packager's
REM own "Runtime binary not found" message is what sends people here to build that configuration.
REM Two dead ends in a row is how a configuration stops being used at all.
if /I "%~1"=="Shipping"     (set "CONFIG=Shipping"       & shift & goto parse_args)
if /I "%~1"=="--unity"      (set "PRESET=windows-msvc-ninja-unity" & shift & goto parse_args)
if /I "%~1"=="--gen-only"   (set "GEN_ONLY=1"            & shift & goto parse_args)
if /I "%~1"=="--no-analyze" (set "ANALYZE=0"             & shift & goto parse_args)
if /I "%~1"=="--no-tests"   (set "WITH_TESTS=OFF"        & shift & goto parse_args)
REM A VALUE IS THE NEXT ARGUMENT, NOT `--flag=value`: cmd splits arguments on `=` as well as on spaces, so
REM `--launcher=C:\x\ccache.exe` reaches %%1 as `--launcher` and %%2 as the path unless the caller quotes it.
if /I "%~1"=="--launcher" (
    if "%~2"=="" (echo [ERROR] --launcher needs a path 1>&2 & exit /b 1)
    set "LAUNCHER_ARG=-DDESERT_COMPILER_LAUNCHER=%~2"
    shift & shift & goto parse_args
)
if /I "%~1"=="--project" (
    if "%~2"=="" (echo [ERROR] --project needs a target name 1>&2 & exit /b 1)
    set "TARGETS=!TARGETS! %~2"
    shift & shift & goto parse_args
)
echo [ERROR] Unknown argument: %~1 1>&2
echo         Usage: scripts\Windows\BuildWindows.bat [Debug^|Release^|Shipping] [--unity] [--gen-only] [--no-analyze] [--no-tests] [--launcher ^<exe^>] [--project ^<Target^>]... 1>&2
exit /b 1
:args_done

REM The x64 MSVC environment: kept when cl.exe is already on PATH ^(a Developer Command Prompt, CI's
REM msvc-dev-cmd^), otherwise vcvars64.bat of the newest Visual Studio with the C++ toolset. Checked by
REM EXIT CODE, never by scraping the output.
where cl >NUL 2>&1
if errorlevel 1 (
    set "VCVARS="
    if exist "%VSWHERE%" (
        for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find VC\Auxiliary\Build\vcvars64.bat 2^>NUL`) do set "VCVARS=%%i"
    )
    if not defined VCVARS (
        echo [ERROR] vcvars64.bat not found. Run scripts\Windows\Setup.bat, which installs the MSVC 1>&2
        echo         v143 toolset, or open an x64 Developer Command Prompt. 1>&2
        exit /b 1
    )
    call "!VCVARS!" >NUL
    if errorlevel 1 (
        echo [ERROR] "!VCVARS!" failed. 1>&2
        exit /b 1
    )
)
REM CMake and Ninja: PATH first ^(a Developer Command Prompt, CI's msvc-dev-cmd, a standalone install^),
REM then the copies Visual Studio bundles with its "C++ CMake tools for Windows" component, searched in
REM EVERY instance vswhere knows ^(any edition, Build Tools and previews included, newest first^).
REM vcvars64.bat puts them on PATH only for its OWN instance, and the instance that has the C++ toolset
REM is not necessarily the one that has the CMake component.
set "VS_CMAKE_REL=Common7\IDE\CommonExtensions\Microsoft\CMake"
where cmake >NUL 2>&1
if errorlevel 1 call :find_vs_tool "CMake\bin\cmake.exe"
where ninja >NUL 2>&1
if errorlevel 1 call :find_vs_tool "Ninja\ninja.exe"
for %%t in (cmake ninja) do (
    where %%t >NUL 2>&1
    if errorlevel 1 (
        echo [ERROR] %%t not found: not on PATH and in no Visual Studio instance. Install the Visual Studio 1>&2
        echo         component "C++ CMake tools for Windows" ^(Microsoft.VisualStudio.Component.VC.CMake.Project: 1>&2
        echo         CMake 3.28+ and Ninja^); scripts\Windows\Setup.bat installs it. 1>&2
        exit /b 1
    )
)

REM NO GenVersion.bat CALL HERE, AND THE ABSENCE IS THE FIX. The build identity is generated by a
REM custom command of the Common target, which runs on every build. Refreshing it here as well would put
REM the refresh in two places, and the one that runs first would hide a hook that had stopped
REM working: a missing header is a compile error, a stale one is a lie.

echo --- Configuring ^(cmake --preset %PRESET% -DDESERT_WITH_TESTS=%WITH_TESTS% %LAUNCHER_ARG%^)
if defined LAUNCHER_ARG (
    cmake --preset %PRESET% -DDESERT_WITH_TESTS=%WITH_TESTS% "!LAUNCHER_ARG!"
) else (
    cmake --preset %PRESET% -DDESERT_WITH_TESTS=%WITH_TESTS%
)
if errorlevel 1 (
    echo [ERROR] cmake --preset %PRESET% failed. 1>&2
    exit /b 1
)

if "%GEN_ONLY%"=="1" (
    echo === build\cmake\%PRESET% configured ===
    exit /b 0
)

REM Build presets are ^<configure preset^>-^<lowercase config^> ^(CMakePresets.json^).
set "BUILD_PRESET=%PRESET%-debug"
if /I "%CONFIG%"=="Release"  set "BUILD_PRESET=%PRESET%-release"
if /I "%CONFIG%"=="Shipping" set "BUILD_PRESET=%PRESET%-shipping"

echo --- Building ^(%BUILD_PRESET%^)
echo     Tip: close Editor.exe first — a running one makes the link fail with LNK1168.
set "TARGET_ARG="
if defined TARGETS set "TARGET_ARG=--target%TARGETS%"
cmake --build --preset %BUILD_PRESET% %TARGET_ARG%
if errorlevel 1 (
    echo [ERROR] build failed ^(%CONFIG%^). 1>&2
    exit /b 1
)

echo.
echo === Build complete: build\Bin\%CONFIG% ===

REM ---------------------------------------------------------------------------
REM Static analysis.
REM
REM THE ONLY OUTCOME THAT MAY BE SILENT IS THE ONE WHERE IT RAN. Every other path prints a line saying
REM the analyser did not run and why, because a check skipped without saying so is worse than one that
REM was never added: the build looks exactly as it does when the code is clean, so the absence is
REM invisible for as long as nobody goes looking. This platform has already paid for that shape once —
REM cmd ate the digit in `set ERROR=0>>` and the Windows test runner could not report a failure at all
REM before 2026-08-15, while every run looked green.
REM
REM NOT UNDER CI: a gate belongs in its own job, where its verdict is separable from the build's,
REM exactly as the clang-format gate already is. Windows Debug measured 73-93 minutes against a
REM 150-minute ceiling and has been cancelled by its own timeout twice; a cancelled job reports as
REM `cancelled`, which looks like nothing being wrong.
REM ---------------------------------------------------------------------------
if "%ANALYZE%"=="0" (
    echo !!! static analysis SKIPPED: --no-analyze was passed. Nothing was checked by clang-tidy.
    endlocal
    exit /b 0
)
if defined CI (
    echo !!! static analysis SKIPPED: running under CI, where the clang-tidy gate is its own job.
    endlocal
    exit /b 0
)
if not exist "%ANALYZER%" (
    REM NOT IMPLEMENTED ON THIS PLATFORM is a THIRD state, and it is not the same as COULD NOT RUN.
    REM
    REM "Could not run" is transient — a missing binary, a wrong version, a broken database — and it
    REM must refuse, because it is indistinguishable from "ran and found nothing" by output alone.
    REM This is not that. The Windows entry point has never been written. The MSVC compile database
    REM it needs now exists ^(build\cmake\windows-msvc-ninja\compile_commands.json, filtered to one
    REM configuration by scripts\CI\GenCompileCommands.sh^); the entry point and clang-tidy's MSVC
    REM driver mode are what is owed. Refusing until somebody writes it does not produce analysis — it produces a platform whose build always fails, which
    REM is how "--no-analyze" becomes the command everybody types and the gate dies unnoticed.
    REM
    REM So it says so on every build, loudly, and proceeds. What keeps this from becoming a permanent
    REM silent skip is not the exit code: it is the row in BuildScriptContract's kPendingReferences,
    REM which names the owing task and REDDENS the moment scripts\CI\CheckTidy.bat appears.
    echo. 1>&2
    echo !!! static analysis NOT AVAILABLE ON WINDOWS YET: "%ANALYZER%" does not exist. 1>&2
    echo     Nothing was checked by clang-tidy. This is a known gap, not a failure of your build: 1>&2
    echo     the Windows entry point of the gate is its own task. 1>&2
    echo     On macOS the same build DOES analyse, so a change that only builds here is unchecked. 1>&2
    echo. 1>&2
    goto after_analysis
)

echo --- Static analysis ^(clang-tidy, changed lines^)
call "%ANALYZER%"
set "ANALYZER_RC=%ERRORLEVEL%"
if "%ANALYZER_RC%"=="0" (
    echo === clang-tidy: clean ===
    endlocal
    exit /b 0
)
if "%ANALYZER_RC%"=="1" (
    echo clang-tidy reported findings in the lines you changed ^(see above^). 1>&2
    endlocal
    exit /b 1
)
REM Exit 2 and anything unexpected are the same thing here: the gate produced no verdict, and
REM "no verdict" must never be reported as "clean".
echo clang-tidy COULD NOT RUN ^(exit %ANALYZER_RC%^) — an environment failure, not a clean analysis. 1>&2
echo   The binaries in build\Bin\%CONFIG% are still valid. 1>&2
endlocal
exit /b 1

:after_analysis
REM Reached ONLY by the not-implemented-on-this-platform branch above. The build succeeded and was
REM not analysed, and the line that says so has already been printed to stderr.
endlocal
exit /b 0

:find_vs_tool
REM %~1: the tool under an instance's bundled CMake directory. The newest instance that has it wins and
REM its folder is prepended to PATH ^(same environment: a `call :label` has no setlocal of its own^).
REM The -find path is UNQUOTED on purpose: it has no spaces, and a second quoted pair inside for /f's
REM backquotes makes cmd strip the outer quotes, so "vswhere.exe" stops being a command.
set "_VS_TOOL="
if not exist "%VSWHERE%" exit /b 0
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -all -prerelease -products * -sort -find %VS_CMAKE_REL%\%~1 2^>NUL`) do (
    if not defined _VS_TOOL set "_VS_TOOL=%%i"
)
if defined _VS_TOOL for %%d in ("!_VS_TOOL!") do set "PATH=%%~dpd;!PATH!"
set "_VS_TOOL="
exit /b 0
