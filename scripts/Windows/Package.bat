@echo off
setlocal EnableDelayedExpansion
REM THE ENGINE DROP — the downloadable build of the TOOLS, not of a game.
REM Output: dist\DesertEngine-<config>\ — CI archives this directory as an artifact (ci.yml).
REM
REM   scripts\Windows\Package.bat [Release^|Debug]
REM
REM WHAT THIS IS AND WHAT IT IS NOT (П5). A GAME is packaged by the editor's own PackageGame() and by
REM nothing else: it needs an OPEN PROJECT, which this script does not have and CI does not have
REM either, and its product is the player binary plus one archive carrying the project's content and
REM its descriptor. This script's product is the EDITOR plus the tools plus a project to open.
REM
REM WHAT TRAVELS, AND WHY IT IS NOT "Editor\Resources". Until 2026-09-11 line 59 was an unfiltered
REM `robocopy Editor\Resources /E`, i.e. 133 MB whole, of which 129 MB is the sandbox project's
REM authored content — 50 MB of baked cloud volumes, 38 MB of meshes and a 37 MB
REM cinematic_menu_loop.gif that NO file in this repository names — none of it reachable from the
REM scene the drop opens, and macOS .DS_Store files riding along into a WINDOWS artifact. Worse,
REM the descriptor that would have made any of it reachable, Editor\Desert.deproj, was not copied at
REM all: the packaged editor had 129 MB of assets and no project able to see them.
REM
REM Now three things travel and each has a rule rather than a list:
REM   1. the ENGINE resource trees — Shaders, Fonts, Icons. Whole, because the engine's own services
REM      SCAN them (Engine/Runtime/Services/ServiceScanRoots.hpp) rather than naming files.
REM   2. the sandbox descriptor, Desert.deproj.
REM   3. the CLOSURE of its DefaultScene, from Tools\AssetClosure, which walks the editor's own asset
REM      reference graph. Not a list in this file: content moves every week, and a stale list fails in
REM      the direction where the package still starts and shows an empty world.
REM
REM WHY Content.dpak AND Content.manifest ARE NOT WRITTEN HERE. Measured on the macOS twin of this
REM script, 2026-09-08; the reasoning is about call sites rather than about the shell. `VFS::MountPak`
REM has exactly ONE non-test call site in this repository — Runtime/Source/PackagedContent.cpp — so
REM the editor and the tools in this directory never mount an archive at all, and the pak's only
REM possible reader was the Runtime sitting beside it. That reader mounted its 144 MB and then
REM refused, because a pak of Editor\Resources carries no project descriptor.

cd /d "%~dp0..\.."
set "ROOT=%CD%"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"

set "CONFIG=%~1"
if "%CONFIG%"=="" set "CONFIG=Release"

set "BIN=%ROOT%\build\Bin\%CONFIG%"
set "OUT=%ROOT%\dist\DesertEngine-%CONFIG%"
set "PROJECT=%ROOT%\Editor\Desert.deproj"

if not exist "%BIN%\Runtime.exe" (
    echo Package.bat: no %CONFIG% binaries in %BIN% — build first 1>&2
    exit /b 1
)
if not exist "%BIN%\AssetClosure.exe" (
    echo Package.bat: %BIN%\AssetClosure.exe is missing. 1>&2
    echo   It is what decides which assets travel. Without it this script could only guess, and a 1>&2
    echo   guess that is short by one file produces a package that starts and shows nothing. 1>&2
    echo   Build it: scripts\Windows\BuildWindows.bat %CONFIG% 1>&2
    exit /b 1
)
if not exist "%BIN%\AssetRegistryTool.exe" (
    echo Package.bat: %BIN%\AssetRegistryTool.exe is missing. 1>&2
    echo   The drop needs its OWN asset registry ^(see the cook at the end of this script^); without 1>&2
    echo   one the packaged editor starts, finds zero shaders and dies before its first frame. 1>&2
    echo   Build it: scripts\Windows\BuildWindows.bat %CONFIG% 1>&2
    exit /b 1
)
if not exist "%PROJECT%" (
    echo Package.bat: %PROJECT% is missing — the drop has no project to open 1>&2
    exit /b 1
)

if exist "%OUT%" rmdir /S /Q "%OUT%"
mkdir "%OUT%" || exit /b 1

for %%E in (Editor Runtime PakTool DShaderTool) do (
    if exist "%BIN%\%%E.exe" copy /Y "%BIN%\%%E.exe" "%OUT%\" >NUL
)

REM Any DLL the build produced, copied beside the exes. NOTHING THIRD-PARTY SHIPS AS A DLL ANY MORE —
REM this line used to say assimp was the one that did, and since D40 assimp is a pinned submodule
REM compiled into the exe. The loop stays because it is a statement about the BUILD OUTPUT rather than
REM about assimp: if some future dependency ever ships one, the drop should carry it. It matches nothing
REM today, and Desert/Tests/Editor/AssimpBoundary asserts that no step names an assimp runtime.
for %%D in ("%BIN%\*.dll") do copy /Y "%%D" "%OUT%\" >NUL 2>&1

REM ---------------------------------------------------------------------------
REM The C runtime, app-local.
REM
REM THE PACKAGE DID NOT START ON A CLEAN MACHINE, and nothing in it said so. Everything here is built
REM /MD (BuildScripts/PlatformWindows.lua), so Editor.exe and Runtime.exe import msvcp140.dll and
REM vcruntime140*.dll — and the drop is a ZIP, so there is nobody to run an installer for the person
REM who unpacks it. Windows answers a missing CRT with a dialog naming a DLL, which reads as a broken
REM build.
REM
REM THE THREE OPTIONS AND WHY THIS ONE. (a) app-local: copy the CRT DLLs beside the exe. Microsoft
REM documents exactly this alongside central deployment — the files are shipped for it in
REM VC\Redist\MSVC\<ver>\x64\Microsoft.VC143.CRT and are covered by the Visual Studio distributable
REM code terms. (b) require the Visual C++ Redistributable and check for it: a check cannot fix
REM anything from inside a ZIP, so it turns a dialog into a different dialog. (c) build /MT: NOT
REM ATTEMPTED HERE, and the reason that used to be written here is now FALSE and has been replaced
REM rather than deleted. It said assimp ARRIVED as a DLL, so a static CRT in the exe plus a dynamic
REM one in assimp would be two heaps (the LNK2038 wall). Since D40 assimp is compiled from a pinned
REM submodule INTO the exe, so that particular obstacle is gone. What has not been done is the work:
REM /MT has to agree across every dependency in the link, and shaderc, the Vulkan loader and the
REM prebuilt gtest are not audited for it. Leaving the old sentence would have been a comment
REM promising a guarantee the tree no longer gives; leaving no sentence would have invited someone to
REM read the silence as "nobody tried". This is the third state: measured, named, and not done.
REM
REM So (a), and it REFUSES rather than shipping a package that cannot start.
REM ---------------------------------------------------------------------------
set "VSINSTALL="
if exist "%VSWHERE%" (
    for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2^>NUL`) do set "VSINSTALL=%%i"
)
set "CRTDIR="
if defined VSINSTALL (
    for /d %%v in ("!VSINSTALL!\VC\Redist\MSVC\*") do (
        if exist "%%v\x64\Microsoft.VC143.CRT\msvcp140.dll" set "CRTDIR=%%v\x64\Microsoft.VC143.CRT"
    )
)
if not defined CRTDIR (
    echo Package.bat: could not find the redistributable VC143 CRT. 1>&2
    echo   Looked under "!VSINSTALL!\VC\Redist\MSVC\*\x64\Microsoft.VC143.CRT". 1>&2
    echo   Without it the package will not start on a machine that has no Visual C++ Redistributable, 1>&2
    echo   and it would not say why — so this refuses instead of shipping one that cannot run. 1>&2
    echo   Add it in the Visual Studio Installer: 1>&2
    echo       Microsoft.VisualStudio.Component.VC.Redist.14.Latest 1>&2
    echo   scripts\Windows\Setup.bat requests that component, so a machine set up by it already has one. 1>&2
    exit /b 1
)
echo Package.bat: CRT from !CRTDIR!
copy /Y "!CRTDIR!\*.dll" "%OUT%\" >NUL || exit /b 1
REM The two the linker actually names. Checked by NAME, because "copy succeeded" only says the
REM directory had something in it.
for %%R in (msvcp140.dll vcruntime140.dll vcruntime140_1.dll) do (
    if not exist "%OUT%\%%R" (
        echo Package.bat: %%R did not reach the package 1>&2
        exit /b 1
    )
)

REM ---------------------------------------------------------------------------
REM Engine resource trees, whole. /XF and /XD keep platform junk out; the check after the copies is
REM what makes that verifiable rather than hoped for.
REM
REM MEASURED ON THE macOS SIDE, where the junk comes from: the owner's working tree holds six
REM .DS_Store files under Editor/Resources, two of them inside the three trees copied here, and the
REM unfiltered copy carried them. A FRESH CHECKOUT holds zero — Finder is what creates them — so CI
REM never saw one, and looking there says the hazard does not exist. Windows contributes Thumbs.db and
REM desktop.ini the same way.
REM ---------------------------------------------------------------------------
for %%T in (Shaders Fonts Icons Splash) do (
    if not exist "%ROOT%\Editor\Resources\%%T" (
        echo Package.bat: engine resource tree Editor\Resources\%%T is missing 1>&2
        exit /b 1
    )
    robocopy "%ROOT%\Editor\Resources\%%T" "%OUT%\Resources\%%T" /E /NFL /NDL /NJH /NJS /NP ^
        /XF .DS_Store Thumbs.db desktop.ini /XD __MACOSX .git >NUL
    REM robocopy uses exit codes 0-7 for success; anything >= 8 is a real failure.
    if !ERRORLEVEL! GEQ 8 (
        echo Package.bat: copying Resources\%%T failed 1>&2
        exit /b 1
    )
)

REM The descriptor, verbatim: the drop's Resources\Assets sits exactly where the dev tree's does, so
REM nothing about it needs rebasing (a GAME's does — see PackagedDescriptor()).
copy /Y "%PROJECT%" "%OUT%\" >NUL || exit /b 1

REM ---------------------------------------------------------------------------
REM The scene's closure. The tool's EXIT CODE is the check: its output is a file list, and an empty
REM one would be indistinguishable from "this scene needs nothing", so it refuses rather than
REM printing none.
REM ---------------------------------------------------------------------------
set "CLOSURE=%TEMP%\desert-closure-%RANDOM%.txt"
"%BIN%\AssetClosure.exe" "%PROJECT%" --out "!CLOSURE!"
if errorlevel 1 (
    echo Package.bat: AssetClosure failed — refusing to guess which assets travel 1>&2
    if exist "!CLOSURE!" del /Q "!CLOSURE!"
    exit /b 1
)

set "ASSETS_SRC=%ROOT%\Editor\Resources\Assets"
set "ASSETS_DST=%OUT%\Resources\Assets"
set "COPIED=0"
for /f "usebackq delims=" %%L in ("!CLOSURE!") do (
    set "REL=%%L"
    set "REL=!REL:/=\!"
    if not exist "!ASSETS_SRC!\!REL!" (
        echo Package.bat: AssetClosure named %%L, which is not a file under !ASSETS_SRC! 1>&2
        del /Q "!CLOSURE!"
        exit /b 1
    )
    for %%F in ("!ASSETS_DST!\!REL!") do if not exist "%%~dpF" mkdir "%%~dpF"
    copy /Y "!ASSETS_SRC!\!REL!" "!ASSETS_DST!\!REL!" >NUL || exit /b 1
    set /a COPIED+=1
)
del /Q "!CLOSURE!"

if "!COPIED!"=="0" (
    echo Package.bat: the closure was empty — refusing to ship a project with no content 1>&2
    exit /b 1
)

REM NEGATIVE CONTROL, and it is here because the positive one cannot see this class at all: a package
REM whose file COUNT is right can still be carrying another operating system's junk.
set "JUNK=0"
for /f "delims=" %%J in ('dir /s /b "%OUT%\.DS_Store" "%OUT%\Thumbs.db" 2^>NUL') do (
    echo Package.bat: platform junk reached the package: %%J 1>&2
    set "JUNK=1"
)
if "!JUNK!"=="1" exit /b 1

REM ---------------------------------------------------------------------------
REM THE DROP'S OWN ASSET REGISTRY, AND WITHOUT IT THE DROP DOES NOT START.
REM
REM MEASURED 2026-09-22 on the macOS twin's output, once the editor stopped demanding --project and
REM could get far enough to fail for this instead: the packaged editor opened its project, reported
REM "[ContentRegistry] 0 row(s)", then "[AssetPreloader] 0 shader program(s) ready", then "Could not
REM find the shader: StaticMeshPBR", and aborted before its first frame. The reasoning is about the
REM boot order rather than about the shell, so it holds here identically.
REM
REM WHY A COOK AND NOT A COPY OF Editor\Cooked\AssetRegistry.dreg. Since GAP_ANALYSIS T2.4 neither
REM host walks the content roots at boot — both READ the registry — so a project with no registry
REM has no content. The dev tree's registry is not the drop's: this package carries the CLOSURE of
REM one scene, not the ~1500 files the repository tracks, so a copied registry would name content
REM the drop does not have. The registry has to describe THIS directory, which is what --disk asks.
REM
REM Run from %OUT% because engine resource roots resolve against the WORKING DIRECTORY and are never
REM remapped by a project — the tool refuses rather than cooking a registry with no shaders in it,
REM and that refusal is this step's check.
REM ---------------------------------------------------------------------------
if not exist "%OUT%\Resources\Splash\Splash.tex" (
    echo Package.bat: the drop has no Resources\Splash\Splash.tex — its splash would open without its picture 1>&2
    exit /b 1
)
pushd "%OUT%"
"%BIN%\AssetRegistryTool.exe" cook "Desert.deproj" --disk
popd
if not exist "%OUT%\Cooked\AssetRegistry.dreg" (
    echo Package.bat: the drop has no Cooked\AssetRegistry.dreg — its editor would start with zero 1>&2
    echo   shaders and abort before the first frame 1>&2
    exit /b 1
)

echo Package.bat: packaged -^> %OUT%
echo   engine resources: Shaders + Fonts + Icons + Splash ^(its picture is committed there^)
echo   project assets:   !COPIED! files, the closure of Desert.deproj's DefaultScene
endlocal
exit /b 0
