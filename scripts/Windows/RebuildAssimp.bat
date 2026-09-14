@echo off
setlocal EnableDelayedExpansion
REM Rebuilds assimp — the ONE third-party dependency this repository commits as an opaque prebuilt
REM binary — from its pinned source.
REM
REM   scripts\Windows\RebuildAssimp.bat [Debug^|Release^|both]
REM
REM WHEN YOU RUN IT: to upgrade assimp, to audit what the committed .lib/.dll actually contain, or on
REM a toolchain the prebuilt ones are incompatible with. NOT as part of setting a machine up and NOT
REM as part of a build — Setup.bat does not call this, because the committed binaries work. That is
REM why it is no longer called BuildDependencies.bat: a name with "Dependencies" in it reads like a
REM setup step, and this is the opposite of one.
REM
REM   assimp   Editor\ThirdParty\assimp\bin\{Debug,Release}  — no source in the tree at all
REM
REM Everything else (ImGui, Jolt, Lua, Optick, MeshOptimizer, GLFW, yaml-cpp, reflect-cpp) is a
REM premake project built by the normal solution. reflect-cpp was a second target here until
REM 2026-09-11 and it produced a file nothing read: this script's own comment claimed
REM "bin/<config>/reflectcpp.lib, the exact path Desert/Dependencies.lua links", and
REM Desert/Dependencies.lua has linked the premake project `ReflectCpp` on every platform since the
REM Release job started dying on LNK1181. The sources are compiled by
REM BuildScripts/ThirdParty/ReflectCpp.lua, one project, all platforms; there is no .lib to rebuild.
REM
REM CRT: built /MD (dynamic) to match BuildScripts/PlatformWindows.lua. Mixing a static CRT into the
REM link is exactly the LNK2038 wall the node editor hit — and it is also why the packaged drop
REM carries the CRT DLLs app-local (see scripts\Windows\Package.bat).

cd /d "%~dp0..\.."
set "ROOT=%CD%"

set "CONFIGS=%~1"
if "%CONFIGS%"=="" set "CONFIGS=both"
if /I "%CONFIGS%"=="both" set "CONFIGS=Debug Release"

REM Pinned: an unpinned dependency turns "rebuild the deps" into an unplanned upgrade.
set "ASSIMP_TAG=v5.4.3"

where cmake >NUL 2>&1
if errorlevel 1 (
    echo [ERROR] cmake not found in PATH. Install it or open a Developer Command Prompt.
    exit /b 1
)

call :build_assimp || exit /b 1
goto :done

REM ---------------------------------------------------------------------------
REM assimp — no source in the tree, so fetch the pinned tag next to the build.
REM Produces Editor/ThirdParty/assimp/bin/<config>/assimp-vc142-mt[d].{lib,dll} + include/.
REM assimp is the ONE dependency that ships as a DLL; everything else links statically.
REM ---------------------------------------------------------------------------
:build_assimp
set "ASSIMP_DST=%ROOT%\Editor\ThirdParty\assimp"
set "ASSIMP_SRC=%ROOT%\ThirdParty\.assimp-src"

echo === Building assimp %ASSIMP_TAG% ===
if not exist "%ASSIMP_SRC%\CMakeLists.txt" (
    if exist "%ASSIMP_SRC%" rmdir /S /Q "%ASSIMP_SRC%"
    git clone --branch %ASSIMP_TAG% --depth 1 https://github.com/assimp/assimp.git "%ASSIMP_SRC%" || exit /b 1
) else (
    echo --- source present ^(delete ThirdParty\.assimp-src to re-fetch^)
)

for %%C in (%CONFIGS%) do (
    echo --- assimp %%C
    REM Importers only: the engine reads models and never writes them, and the exporters roughly
    REM double the build. Tests/tools/samples off for the same reason.
    cmake -S "%ASSIMP_SRC%" -B "%ASSIMP_SRC%\build-%%C" -A x64 ^
        -DCMAKE_POLICY_DEFAULT_CMP0091=NEW ^
        -DCMAKE_MSVC_RUNTIME_LIBRARY="MultiThreaded$<$<CONFIG:Debug>:Debug>DLL" ^
        -DBUILD_SHARED_LIBS=ON ^
        -DASSIMP_BUILD_ASSIMP_TOOLS=OFF ^
        -DASSIMP_BUILD_TESTS=OFF ^
        -DASSIMP_BUILD_SAMPLES=OFF ^
        -DASSIMP_INSTALL=OFF ^
        -DASSIMP_NO_EXPORT=ON ^
        -DASSIMP_WARNINGS_AS_ERRORS=OFF >NUL || exit /b 1
    cmake --build "%ASSIMP_SRC%\build-%%C" --config %%C || exit /b 1

    if not exist "%ASSIMP_DST%\bin\%%C" mkdir "%ASSIMP_DST%\bin\%%C"
    for %%F in ("%ASSIMP_SRC%\build-%%C\bin\%%C\assimp-*.dll") do copy /Y "%%F" "%ASSIMP_DST%\bin\%%C\" >NUL
    for %%F in ("%ASSIMP_SRC%\build-%%C\lib\%%C\assimp-*.lib") do copy /Y "%%F" "%ASSIMP_DST%\bin\%%C\" >NUL
    echo     -^> Editor\ThirdParty\assimp\bin\%%C\
)

REM Headers: the public include tree plus the generated config.h, which only exists after configure.
echo --- assimp headers
xcopy /E /I /Y /Q "%ASSIMP_SRC%\include\assimp" "%ASSIMP_DST%\include\assimp" >NUL || exit /b 1
for %%C in (%CONFIGS%) do (
    if exist "%ASSIMP_SRC%\build-%%C\include\assimp\config.h" (
        copy /Y "%ASSIMP_SRC%\build-%%C\include\assimp\config.h" "%ASSIMP_DST%\include\assimp\config.h" >NUL
    )
)
echo     -^> Editor\ThirdParty\assimp\include\assimp
exit /b 0

:done
echo.
echo === assimp rebuilt ===
echo NOTE: the resulting binaries are COMMITTED to the repo. Review the diff before committing —
echo       a dependency upgrade should be a deliberate, separate change.
endlocal
