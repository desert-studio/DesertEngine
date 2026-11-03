@echo off
setlocal EnableDelayedExpansion
REM One-time environment setup for building Desert Engine on Windows.
REM
REM   scripts\Windows\Setup.bat [--no-install] [--help]
REM
REM Installs every build dependency this repository actually needs, fetches the third-party pieces
REM that are not submodules, and checks the CMake + Ninja the build configures with. Safe to re-run.
REM It configures nothing itself: scripts\Windows\BuildWindows.bat runs `cmake --preset` on every build.
REM
REM WHY THIS SCRIPT IS AS LONG AS ITS macOS TWIN NOW. It used to be 134 lines against Setup.sh's 352,
REM and the difference was not economy: three things the build cannot start without were simply not
REM here, and the script exited 0 without them. It warned about a missing Vulkan SDK and carried on;
REM it never looked for a compiler at all; and it aborted on the first failing step, which is the
REM exact defect Д5 fixed on the macOS side (one unhappy submodule took every later step down with
REM it, and the symptom read as "the worktree is broken"). The list of what a build needs is not a
REM matter of opinion — .github/workflows/ci.yml installs it, because a runner starts from nothing.
REM This script installs the same set.
REM
REM Steps are INDEPENDENT and none of them aborts the run. Each failure is recorded by name and the
REM summary at the end lists all of them; the exit code is non-zero if anything failed or if any path
REM the build reads is still missing.

cd /d "%~dp0..\.."
set "ROOT=%CD%"

REM %ProgramFiles(x86)% must be expanded outside any parenthesised block: the ")" in the name closes
REM the block at parse time and cmd reports a syntax error that names the wrong line.
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"

REM Must equal VULKAN_SDK_VERSION in .github/workflows/ci.yml. Desert/Tests/Tools/SetupScripts
REM asserts the two agree, because a developer building against a different SDK than CI is how a
REM link error becomes "works on my machine".
set "VULKAN_SDK_VERSION=1.3.296.0"
REM The oldest CMake that reads CMakePresets.json version 6 and the root cmake_minimum_required.
set "CMAKE_MIN_MAJOR=3"
set "CMAKE_MIN_MINOR=28"

set "NO_INSTALL=0"
REM THE PARENTHESES ARE LOAD-BEARING. cmd splits on `&` at PARSE time, so
REM `if COND set X & shift & goto L` is read as `(if COND set X) & shift & goto L` — the shift and the
REM goto run for EVERY argument, whatever the condition. Without them this loop silently swallowed
REM --help and every unknown flag: a typo'd argument was accepted rather than rejected, which is the
REM same "empty successful answer" shape the contract forbids, wearing cmd's syntax.
:parse_args
if "%~1"=="" goto args_done
if /I "%~1"=="--no-install" (set "NO_INSTALL=1" & shift & goto parse_args)
if /I "%~1"=="--help" goto usage
if /I "%~1"=="-h" goto usage
echo [ERROR] unknown argument "%~1"
goto usage
:args_done

echo === Desert Engine Windows setup ===
if "%NO_INSTALL%"=="1" echo     --no-install: checking only, nothing will be installed.

set "FAILCOUNT=0"

REM ---------------------------------------------------------------------------
REM 0. git — everything below is a clone or a submodule, so this one IS fatal.
REM ---------------------------------------------------------------------------
where git >NUL 2>&1
if errorlevel 1 (
    echo [ERROR] git not found in PATH. Install Git for Windows ^(https://git-scm.com/download/win^)
    echo         or run: winget install --id Git.Git -e
    exit /b 1
)

REM ---------------------------------------------------------------------------
REM 1. MSVC toolset + Windows SDK.
REM ---------------------------------------------------------------------------
call :require_msvc
if errorlevel 1 call :fail "Visual Studio 2022 MSVC toolset (v143) + Windows SDK"

REM ---------------------------------------------------------------------------
REM 2. Vulkan SDK.
REM ---------------------------------------------------------------------------
call :require_vulkan
if errorlevel 1 call :fail "LunarG Vulkan SDK %VULKAN_SDK_VERSION%"

REM ---------------------------------------------------------------------------
REM 3. Git submodules, ONE AT A TIME.
REM
REM `git submodule update --init --recursive` walks every submodule in one process and dies on the
REM first one it cannot resolve, leaving each later submodule uninitialised. That is what this script
REM used to do, and it is the Windows half of Д5.
REM ---------------------------------------------------------------------------
echo --- Initializing git submodules
set "SM_COUNT=0"
REM `git ls-files --stage` prints "<mode> <sha> <stage><TAB><path>"; mode 160000 is a gitlink.
for /f "usebackq tokens=1,4" %%m in (`git ls-files --stage`) do (
    if "%%m"=="160000" (
        set /a SM_COUNT+=1
        call :init_submodule "%%n"
    )
)
if "%SM_COUNT%"=="0" call :fail "no submodules found in the git index (is this a git checkout?)"

REM ---------------------------------------------------------------------------
REM 4. Third-party sources that are NOT submodules (both are gitignored).
REM ---------------------------------------------------------------------------
call :clone_dep "ThirdParty\optick\src\optick.h" "ThirdParty\optick" ^
    "https://github.com/bombomby/optick.git" ""

REM meshoptimizer pinned to v1.3, the version cmake/ThirdParty/MeshOptimizer.cmake checks.
REM A tree holding another version is re-fetched.
if exist "ThirdParty\meshoptimizer\src\meshoptimizer.h" (
    findstr /C:"define MESHOPTIMIZER_VERSION 1030" "ThirdParty\meshoptimizer\src\meshoptimizer.h" >nul || (
        echo --- ThirdParty\meshoptimizer: not v1.3, re-fetching
        rmdir /s /q "ThirdParty\meshoptimizer"
    )
)
call :clone_dep "ThirdParty\meshoptimizer\src\meshoptimizer.h" "ThirdParty\meshoptimizer" ^
    "https://github.com/zeux/meshoptimizer.git" "v1.3"

REM reflect-cpp compiled sources. The header of this script used to claim Windows needs none of this
REM because it "links the prebuilt ThirdParty/reflect-cpp/lib". There is no such directory and there
REM never was: cmake/ThirdParty/ReflectCpp.cmake builds one ReflectCpp project on EVERY platform
REM and errors out if src/ is absent. The sources are committed, so this normally does nothing — but a
REM tree that lost them must be repairable from Windows, not only from a Mac.
call :fetch_reflectcpp

REM ---------------------------------------------------------------------------
REM 4b. Vendor runtime binaries (the DLSS runtime, NVIDIA Streamline): DOWNLOADED, never committed.
REM     ThirdParty\Streamline\manifest.json pins the release and every file's SHA-256; the script fetches
REM     the archive, checks it and writes ThirdParty\Streamline\bin\x64 (gitignored). Re-running with the
REM     files in place touches nothing. Without them everything still generates and builds; the build
REM     prints which file is missing, the editor and game report DLSS unavailable, the packager refuses.
REM ---------------------------------------------------------------------------
call :fetch_vendor_binaries

REM ---------------------------------------------------------------------------
REM 5. CMake 3.28+ and Ninja: the Visual Studio "C++ CMake tools for Windows" component
REM    (Microsoft.VisualStudio.Component.VC.CMake.Project) ships both, and vcvars64 puts them on PATH.
REM
REM NO GenVersion.bat CALL HERE. Setup runs once per machine, so a header it wrote would be frozen from
REM then on. Generating it is a custom command of the Common target, which runs on every build.
REM ---------------------------------------------------------------------------
call :require_cmake_ninja
if errorlevel 1 call :fail "CMake %CMAKE_MIN_MAJOR%.%CMAKE_MIN_MINOR%+ and Ninja (Visual Studio C++ CMake tools)"

REM ---------------------------------------------------------------------------
REM 6. Is everything the configure step reads actually on disk?
REM
REM CMake refuses a missing file it names. Everything else is pulled in by a glob, so when it is
REM missing the configure still succeeds and emits a target with no source files — the failure only
REM surfaces later as a link error naming a symbol you can see in the tree. Name them here instead.
REM Same list as scripts/MacOS/Setup.sh.
REM
REM One submodule is deliberately absent: Editor/ThirdParty/ImGuiColorTextEdit. No CMakeLists.txt and no
REM source refers to it. It is still initialised above and a failure to initialise it is still reported
REM — but the build does not read it, so its absence must not be reported as a missing build input.
REM
REM assimp is a SUBMODULE COMPILED FROM SOURCE since D40, so what the list below verifies is its
REM SOURCE TREE (code\Common\ImporterRegistry.cpp — the file the build derives its importer set from),
REM not the two prebuilt import libraries it used to name. Those are deleted; naming them here would
REM have reported MISSING twice on every Windows setup.
REM
REM AND THIS PARAGRAPH BELONGS HERE RATHER THAN BESIDE THE ENTRY IT EXPLAINS, WHICH IS NOT A STYLE
REM CHOICE. It was first written as three `REM` lines INSIDE the `for %%P in ( ... )` list below, and
REM cmd does not treat `REM` inside parentheses as a comment — it is DATA. The loop iterated the words
REM of the comment as if they were paths ("- REM", "- tree", "- not", "- the", ...) and Setup.bat
REM exited 1 on both Windows configurations while every macOS job stayed green. Same family as the
REM `set ERROR=0>>` that lost its digit and left the Windows test runner unable to fail at all before
REM 2026-08-15: cmd's parsing inside `( )` is invisible from this repository's other platform.
REM ---------------------------------------------------------------------------
echo --- Verifying the paths the build reads
set "MISSCOUNT=0"
for %%P in (
    "ThirdParty\optick\src\optick.h"
    "ThirdParty\meshoptimizer\src\meshoptimizer.h"
    "ThirdParty\reflect-cpp\src\reflectcpp.cpp"
    "ThirdParty\reflect-cpp\include"
    "ThirdParty\GLFW\include\GLFW\glfw3.h"
    "ThirdParty\ImGui\imgui.cpp"
    "ThirdParty\imgui-node-editor\imgui_node_editor.cpp"
    "ThirdParty\JoltPhysics\Jolt\Jolt.h"
    "ThirdParty\lua\lapi.c"
    "ThirdParty\spdlog\include\spdlog\spdlog.h"
    "ThirdParty\google-test\include\gtest\gtest.h"
    "ThirdParty\glm\glm\glm.hpp"
    "ThirdParty\entt\include\entt\entt.hpp"
    "ThirdParty\stb\include"
    "ThirdParty\VulkanAllocator"
    "Editor\ThirdParty\ImGuizmo\ImGuizmo.cpp"
    "Editor\ThirdParty\assimp\include\assimp\Importer.hpp"
    "Editor\ThirdParty\assimp\code\Common\ImporterRegistry.cpp"
) do (
    if not exist "%ROOT%\%%~P" (
        set /a MISSCOUNT+=1
        set "MISS_!MISSCOUNT!=%%~P"
        echo     MISSING: %%~P
    )
)
if "%MISSCOUNT%"=="0" echo     all required paths present

REM ---------------------------------------------------------------------------
REM 7. Summary
REM ---------------------------------------------------------------------------
echo.
if "%FAILCOUNT%"=="0" if "%MISSCOUNT%"=="0" (
    echo === Setup complete ===
    echo Build with:  scripts\Windows\BuildWindows.bat [Debug^|Release^|Shipping] [--unity]
    echo              ^(or open the folder in Visual Studio: it reads CMakePresets.json^)
    endlocal
    exit /b 0
)

echo === Setup INCOMPLETE ===
if not "%FAILCOUNT%"=="0" (
    echo.
    echo Steps that failed ^(%FAILCOUNT%^):
    for /L %%I in (1,1,%FAILCOUNT%) do echo   - !FAIL_%%I!
)
if not "%MISSCOUNT%"=="0" (
    echo.
    echo Paths the build needs that are still missing ^(%MISSCOUNT%^):
    for /L %%I in (1,1,%MISSCOUNT%) do echo   - !MISS_%%I!
)
echo.
echo Re-running this script is safe: it retries every step that failed.
echo.
echo If one submodule keeps failing, reset that one by hand and retry it:
echo     git submodule deinit -f -- ^<path^>
echo     rmdir /S /Q ".git\modules\^<path^>" ^<path^>
echo     git submodule update --init --recursive -- ^<path^>
echo.
echo Do not copy ThirdParty\ from another checkout to work around this. Each submodule's .git file
echo holds a relative path to its gitdir, which resolves only in the tree it was created in; copying
echo it is what produces the "Unable to find current revision" failure this script repairs.
endlocal
exit /b 1

REM ===========================================================================
REM Subroutines
REM ===========================================================================

:usage
echo.
echo   scripts\Windows\Setup.bat [--no-install]
echo.
echo   Installs the Windows build dependencies, fetches the non-submodule third-party sources
echo   and checks CMake and Ninja. Re-runnable. Build with scripts\Windows\BuildWindows.bat.
echo.
echo   --no-install   Report what is missing and do not install anything. Everything that is a
echo                  clone rather than an installer still runs.
echo.
exit /b 2

REM --- CMake + Ninja ---------------------------------------------------------
REM PATH first (a developer prompt, CI's msvc-dev-cmd), then the copies the Visual Studio C++ CMake tools
REM component installs under the instance :require_msvc found. The version is READ, never assumed: an
REM older CMake on PATH fails on CMakePresets.json with a message that does not name the version.
:require_cmake_ninja
set "CMAKE_EXE="
set "NINJA_EXE="
for /f "usebackq delims=" %%i in (`where cmake 2^>NUL`) do if not defined CMAKE_EXE set "CMAKE_EXE=%%i"
for /f "usebackq delims=" %%i in (`where ninja 2^>NUL`) do if not defined NINJA_EXE set "NINJA_EXE=%%i"
set "VS_CMAKE_DIR=!VSINSTALL!\Common7\IDE\CommonExtensions\Microsoft\CMake"
if defined VSINSTALL if not defined CMAKE_EXE if exist "!VS_CMAKE_DIR!\CMake\bin\cmake.exe" set "CMAKE_EXE=!VS_CMAKE_DIR!\CMake\bin\cmake.exe"
if defined VSINSTALL if not defined NINJA_EXE if exist "!VS_CMAKE_DIR!\Ninja\ninja.exe" set "NINJA_EXE=!VS_CMAKE_DIR!\Ninja\ninja.exe"
if not defined CMAKE_EXE goto cmake_missing
if not defined NINJA_EXE goto cmake_missing
set "CMAKE_VER="
for /f "usebackq tokens=3" %%v in (`""!CMAKE_EXE!" --version 2^>NUL ^| findstr /B /C:"cmake version""`) do set "CMAKE_VER=%%v"
if not defined CMAKE_VER (
    echo [ERROR] "!CMAKE_EXE!" --version printed no "cmake version" line.
    exit /b 1
)
set "CM_MAJ="
set "CM_MIN="
for /f "tokens=1,2 delims=." %%a in ("!CMAKE_VER!") do (
    set "CM_MAJ=%%a"
    set "CM_MIN=%%b"
)
set "CMAKE_OLD=0"
if !CM_MAJ! LSS %CMAKE_MIN_MAJOR% set "CMAKE_OLD=1"
if !CM_MAJ! EQU %CMAKE_MIN_MAJOR% if !CM_MIN! LSS %CMAKE_MIN_MINOR% set "CMAKE_OLD=1"
if "!CMAKE_OLD!"=="1" (
    echo [ERROR] "!CMAKE_EXE!" is CMake !CMAKE_VER!; this repository needs %CMAKE_MIN_MAJOR%.%CMAKE_MIN_MINOR% or newer.
    echo         Update Visual Studio ^(its C++ CMake tools carry a newer CMake^) or put a newer cmake first on PATH.
    exit /b 1
)
echo --- CMake !CMAKE_VER!: "!CMAKE_EXE!"
echo --- Ninja: "!NINJA_EXE!"
exit /b 0

:cmake_missing
echo [ERROR] CMake or Ninja not found ^(cmake: "!CMAKE_EXE!", ninja: "!NINJA_EXE!"^).
echo         Add the "C++ CMake tools for Windows" component in the Visual Studio Installer:
echo             Microsoft.VisualStudio.Component.VC.CMake.Project
exit /b 1

:fail
set /a FAILCOUNT+=1
set "FAIL_%FAILCOUNT%=%~1"
echo     [FAILED] %~1 1>&2
exit /b 0

REM --- MSVC ------------------------------------------------------------------
REM WHAT WE ACTUALLY NEED IS NOT "VISUAL STUDIO". It is the MSVC v143 toolset, a Windows SDK and the
REM C++ CMake tools. CI gets exactly that from the windows-2022 image plus ilammy/msvc-dev-cmd; nothing
REM there installs an IDE. So the check is for the components, and the offered install is Build Tools
REM for Visual Studio 2022 — a SEPARATE instance, which is why installing it cannot disturb an
REM existing Visual Studio.
REM
REM WHY v143 SPECIFICALLY, AND WHY 2022 IS THE FLOOR: v143 is the toolset every Windows build of this
REM repository has been measured with. A newer Visual Studio (17.14, or an 18.x "2026") carries a
REM different default toolset, and vcvars64 of that instance compiles with it. Checking the directory
REM is therefore the honest check; checking the IDE's version number is not, in either direction.
:require_msvc
if not exist "%VSWHERE%" goto msvc_missing
set "VSINSTALL="
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2^>NUL`) do set "VSINSTALL=%%i"
if not defined VSINSTALL goto msvc_missing

set "V143="
REM v143 IS A TOOLSET NAME, NOT A DIRECTORY NUMBER. This used to match "14.3*" on the assumption that
REM the two are the same string. They are not: Visual Studio 2022 shipped MSVC 14.3x at release and
REM ships 14.4x since 17.10, and every one of them is PlatformToolset v143. The narrow pattern turned
REM a healthy VS 2022 Enterprise into "[ERROR] has no MSVC v143 toolset" and took CI down with it.
REM
REM v144 does not exist yet; when it does, the directory alone will stop being enough and this has to
REM ask vswhere for the component instead. Until then any 14.x under a VS 2022-or-newer install IS the
REM toolset the build expects, and the FLOOR is enforced by the VS version above, not by this number.
for /d %%d in ("!VSINSTALL!\VC\Tools\MSVC\14.*") do set "V143=%%~nxd"
if not defined V143 (
    echo [ERROR] "!VSINSTALL!" has no MSVC v143 toolset ^(no VC\Tools\MSVC\14.* directory^).
    echo         vcvars64 has no compiler to put on PATH without it. Add it in the Visual Studio Installer:
    echo             Microsoft.VisualStudio.Component.VC.Tools.x86.x64
    exit /b 1
)

echo --- MSVC: v!V143! in "!VSINSTALL!"
exit /b 0

:msvc_missing
echo --- MSVC v143 toolset: NOT FOUND
if "%NO_INSTALL%"=="1" (
    echo [ERROR] No Visual Studio 2022+ instance carries the MSVC v143 toolset. Install it with:
    echo         winget install --id Microsoft.VisualStudio.2022.BuildTools -e --override "--quiet --wait --norestart --add Microsoft.VisualStudio.Workload.VCTools --add Microsoft.VisualStudio.Component.VC.Tools.x86.x64 --add Microsoft.VisualStudio.Component.VC.CMake.Project --add Microsoft.VisualStudio.Component.VC.Redist.14.Latest --add Microsoft.VisualStudio.Component.Windows11SDK.22621"
    exit /b 1
)
where winget >NUL 2>&1
if errorlevel 1 (
    echo [ERROR] winget is not available, so this cannot be installed automatically.
    echo         Install "Build Tools for Visual Studio 2022" by hand from
    echo         https://visualstudio.microsoft.com/downloads/ and select the
    echo         "Desktop development with C++" workload.
    exit /b 1
)
echo --- Installing Build Tools for Visual Studio 2022 ^(several GB; UAC will prompt^)
winget install --id Microsoft.VisualStudio.2022.BuildTools -e ^
    --accept-package-agreements --accept-source-agreements ^
    --override "--quiet --wait --norestart --add Microsoft.VisualStudio.Workload.VCTools --add Microsoft.VisualStudio.Component.VC.Tools.x86.x64 --add Microsoft.VisualStudio.Component.VC.CMake.Project --add Microsoft.VisualStudio.Component.VC.Redist.14.Latest --add Microsoft.VisualStudio.Component.Windows11SDK.22621"
if errorlevel 1 (
    echo [ERROR] winget could not install the Build Tools ^(exit %ERRORLEVEL%^).
    exit /b 1
)
REM Re-check rather than trust the installer's exit code: winget reports success for a reboot-pending
REM install too, and a toolset that is not on disk yet is not a toolset.
set "VSINSTALL="
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2^>NUL`) do set "VSINSTALL=%%i"
if not defined VSINSTALL (
    echo [ERROR] the Build Tools installer reported success but vswhere still finds no v143 toolset.
    echo         A restart may be pending; re-run this script after it.
    exit /b 1
)
echo --- MSVC installed at "!VSINSTALL!"
exit /b 0

REM --- Vulkan ----------------------------------------------------------------
REM The engine links shaderc*.lib out of the SDK's Lib directory (SPIRV-Cross is the vendored
REM ThirdParty/SPIRV-Cross, built from source - never the SDK's), so a
REM headers-only install is not enough. Pinned to the version CI builds against, and installed with
REM the SAME optional component CI selects: com.lunarg.vulkan.debug is the "Debuggable Shader API
REM Libraries", where shadercd.lib and friends live. A bare `install` takes the core only and the
REM Debug configuration then fails to link against a library that was never installed.
:require_vulkan
if defined VULKAN_SDK (
    if exist "%VULKAN_SDK%\Include\vulkan\vulkan.h" (
        echo --- Vulkan SDK: %VULKAN_SDK%
        exit /b 0
    )
    echo [WARN] VULKAN_SDK is set to "%VULKAN_SDK%" but Include\vulkan\vulkan.h is not there.
)
set "VK_ROOT=C:\VulkanSDK\%VULKAN_SDK_VERSION%"
if exist "%VK_ROOT%\Include\vulkan\vulkan.h" goto vulkan_export

echo --- Vulkan SDK %VULKAN_SDK_VERSION%: NOT FOUND
if "%NO_INSTALL%"=="1" (
    echo [ERROR] Install it from https://vulkan.lunarg.com/sdk/home#windows, or re-run without
    echo         --no-install. The "Debuggable Shader API Libraries" component is REQUIRED — the
    echo         Debug configuration links shadercd.lib, which the default install omits.
    exit /b 1
)
echo --- Downloading and installing the LunarG Vulkan SDK %VULKAN_SDK_VERSION% ^(~200 MB^)
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
    "$ErrorActionPreference = 'Stop';" ^
    "$ver = '%VULKAN_SDK_VERSION%';" ^
    "$url = \"https://sdk.lunarg.com/sdk/download/$ver/windows/VulkanSDK-$ver-Installer.exe\";" ^
    "$exe = Join-Path $env:TEMP 'VulkanSDK-Installer.exe';" ^
    "Invoke-WebRequest -Uri $url -OutFile $exe;" ^
    "& $exe --root '%VK_ROOT%' --accept-licenses --default-answer --confirm-command install com.lunarg.vulkan.debug;" ^
    "if ($LASTEXITCODE -ne 0) { throw \"Vulkan SDK installer exited $LASTEXITCODE\" };" ^
    "Remove-Item $exe -Force"
if errorlevel 1 (
    echo [ERROR] the Vulkan SDK install failed.
    exit /b 1
)
if not exist "%VK_ROOT%\Include\vulkan\vulkan.h" (
    echo [ERROR] the installer reported success but %VK_ROOT%\Include\vulkan\vulkan.h is not there.
    exit /b 1
)

:vulkan_export
REM Set it for this process AND for future shells. cmake/DesertDependencies.cmake reads VULKAN_SDK first and
REM only then guesses at Program Files, so leaving it unset makes the configure pick whichever SDK version
REM sorts highest — a different one from the pin above, silently.
set "VULKAN_SDK=%VK_ROOT%"
setx VULKAN_SDK "%VK_ROOT%" >NUL 2>&1
echo --- Vulkan SDK: %VULKAN_SDK% ^(also persisted for new shells^)
exit /b 0

REM --- Submodules ------------------------------------------------------------
REM A submodule whose clone succeeded but whose checkout did not leaves a directory holding nothing
REM but the .git pointer. git treats that as up to date: `git submodule update` exits 0 without
REM touching it, for ever. The exit status alone therefore cannot tell a populated submodule from an
REM empty one — look at the working tree.
:init_submodule
set "SM=%~1"
git submodule update --init --recursive -- "!SM!" >NUL 2>&1
if not errorlevel 1 (
    call :submodule_is_populated "!SM!"
    if not errorlevel 1 (
        echo     !SM!: ok
        exit /b 0
    )
)
echo     !SM!: failed or empty; resetting its local state and retrying once
git submodule deinit -f -- "!SM!" >NUL 2>&1
REM Where git keeps a submodule's real repository. In a LINKED WORKTREE that is not .git\modules but
REM .git\worktrees\<name>\modules, so ask git rather than assuming — a wrong guess here silently
REM leaves the broken gitdir in place and the retry fails exactly as the first attempt did.
set "GITDIR="
for /f "usebackq delims=" %%g in (`git rev-parse --absolute-git-dir 2^>NUL`) do set "GITDIR=%%g"
if defined GITDIR if exist "!GITDIR!\modules\!SM!" rmdir /S /Q "!GITDIR!\modules\!SM!"
set "GITCOMMON="
for /f "usebackq delims=" %%g in (`git rev-parse --path-format^=absolute --git-common-dir 2^>NUL`) do set "GITCOMMON=%%g"
if defined GITCOMMON if exist "!GITCOMMON!\modules\!SM!" rmdir /S /Q "!GITCOMMON!\modules\!SM!"
if exist "!SM!" rmdir /S /Q "!SM!"
git submodule update --init --recursive -- "!SM!"
if not errorlevel 1 (
    call :submodule_is_populated "!SM!"
    if not errorlevel 1 (
        echo     !SM!: ok ^(after reset^)
        exit /b 0
    )
    call :fail "!SM! is still empty after a reset and a re-clone"
    exit /b 0
)
call :fail "git submodule update --init --recursive -- !SM!"
exit /b 0

:submodule_is_populated
set "SM_ENTRIES=0"
for /f %%c in ('dir /b /a "%~1" 2^>NUL ^| find /v ".git" ^| find /c /v ""') do set "SM_ENTRIES=%%c"
if "!SM_ENTRIES!"=="0" exit /b 1
exit /b 0

REM --- Non-submodule clones --------------------------------------------------
REM clone_dep <sentinel-relative-path> <dest-relative-dir> <url> <branch-or-empty>
:clone_dep
set "SENTINEL=%~1"
set "DEST=%~2"
set "URL=%~3"
set "BRANCH=%~4"
if exist "%ROOT%\!SENTINEL!" (
    echo --- !DEST!: present
    exit /b 0
)
echo --- Cloning !DEST!
if exist "%ROOT%\!DEST!" rmdir /S /Q "%ROOT%\!DEST!"
if "!BRANCH!"=="" (
    git clone --depth 1 "!URL!" "%ROOT%\!DEST!"
) else (
    git clone --branch "!BRANCH!" --depth 1 "!URL!" "%ROOT%\!DEST!"
)
if errorlevel 1 (
    call :fail "git clone !URL! -> !DEST!"
    exit /b 0
)
REM A clone that exits 0 without producing the file we need is a silent wrong answer, so check the
REM file rather than the exit code alone.
if not exist "%ROOT%\!SENTINEL!" call :fail "!DEST! was cloned but !SENTINEL! is missing"
exit /b 0

REM --- vendor runtime binaries -----------------------------------------------
:fetch_vendor_binaries
echo --- Vendor runtime binaries (scripts\Windows\FetchVendorBinaries.py)
where python >NUL 2>&1
if errorlevel 1 (
    call :fail "Python 3 not found in PATH: it fetches ThirdParty\Streamline\bin\x64 (winget install --id Python.Python.3.11 -e)"
    exit /b 0
)
python "%ROOT%\scripts\Windows\FetchVendorBinaries.py"
if errorlevel 1 call :fail "fetching the DLSS runtime into ThirdParty\Streamline\bin\x64 (the [ERROR] line above names why)"
exit /b 0

REM --- reflect-cpp -----------------------------------------------------------
:fetch_reflectcpp
if exist "%ROOT%\ThirdParty\reflect-cpp\src\reflectcpp.cpp" (
    echo --- ThirdParty/reflect-cpp: sources present
    exit /b 0
)
echo --- Fetching reflect-cpp v0.19.0 sources
set "TMP_RCPP=%TEMP%\desert-reflectcpp-%RANDOM%"
git clone --branch v0.19.0 --depth 1 https://github.com/getml/reflect-cpp "!TMP_RCPP!"
if errorlevel 1 (
    call :fail "git clone reflect-cpp v0.19.0"
    exit /b 0
)
if not exist "%ROOT%\ThirdParty\reflect-cpp\src" mkdir "%ROOT%\ThirdParty\reflect-cpp\src"
copy /Y "!TMP_RCPP!\src\reflectcpp.cpp"      "%ROOT%\ThirdParty\reflect-cpp\src\" >NUL
copy /Y "!TMP_RCPP!\src\reflectcpp_json.cpp" "%ROOT%\ThirdParty\reflect-cpp\src\" >NUL
copy /Y "!TMP_RCPP!\src\yyjson.c"            "%ROOT%\ThirdParty\reflect-cpp\src\" >NUL
xcopy /E /I /Y /Q "!TMP_RCPP!\src\rfl" "%ROOT%\ThirdParty\reflect-cpp\src\rfl" >NUL
rmdir /S /Q "!TMP_RCPP!"
if not exist "%ROOT%\ThirdParty\reflect-cpp\src\reflectcpp.cpp" (
    call :fail "copying reflect-cpp sources into ThirdParty\reflect-cpp\src"
    exit /b 0
)
echo     ThirdParty/reflect-cpp: fetched
exit /b 0
