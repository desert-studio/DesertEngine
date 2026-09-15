@echo off
setlocal EnableDelayedExpansion
REM One-time environment setup for building Desert Engine on Windows.
REM
REM   scripts\Windows\Setup.bat [--no-install] [--help]
REM
REM Installs every build dependency this repository actually needs, fetches the third-party pieces
REM that are not submodules, and generates the Visual Studio solution. Safe to re-run.
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
set "VULKAN_SDK_VERSION=1.3.290.0"
set "PREMAKE_VERSION=5.0.0-beta8"

REM The MSVC toolset the generated solution asks for. premake's vs2022 action emits
REM PlatformToolset=v143. NOTE that v143 is a TOOLSET NAME, not a directory number: VS 2022 shipped
REM 14.3x at first and ships 14.4x since 17.10, and both are v143. Checking for "14.3*" rejected a
REM perfectly good Visual Studio 2022 — see :require_msvc.
set "PREMAKE_ACTION=vs2022"

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
REM 4. Third-party sources that are NOT submodules (all three are gitignored).
REM ---------------------------------------------------------------------------
call :clone_dep "ThirdParty\optick\src\optick.h" "ThirdParty\optick" ^
    "https://github.com/bombomby/optick.git" ""

REM volk: the include is <volk/volk.h> and the directory on the include path is ThirdParty
REM (Desert/Dependencies.lua, `base = baseDir`), NOT the Vulkan SDK. This script did not clone it
REM until 2026-09-07 on the belief that the LunarG SDK supplies it; the SDK's copy is not on any
REM include path here, so Windows failed with "Cannot open include file: 'volk/volk.h'".
call :clone_dep "ThirdParty\volk\volk.h" "ThirdParty\volk" ^
    "https://github.com/zeux/volk.git" ""

REM meshoptimizer pinned to v0.20, the version BuildScripts/ThirdParty/MeshOptimizer.lua expects.
call :clone_dep "ThirdParty\meshoptimizer\src\meshoptimizer.h" "ThirdParty\meshoptimizer" ^
    "https://github.com/zeux/meshoptimizer.git" "v0.20"

REM reflect-cpp compiled sources. The header of this script used to claim Windows needs none of this
REM because it "links the prebuilt ThirdParty/reflect-cpp/lib". There is no such directory and there
REM never was: BuildScripts/ThirdParty/ReflectCpp.lua builds one ReflectCpp project on EVERY platform
REM and errors out if src/ is absent. The sources are committed, so this normally does nothing — but a
REM tree that lost them must be repairable from Windows, not only from a Mac.
call :fetch_reflectcpp

REM ---------------------------------------------------------------------------
REM 5. premake5 — fetched, not vendored.
REM     This step used to run "%ROOT%\vendor\bin\premake5.exe" unconditionally. That file has never
REM     been in the repository and cannot be: .gitignore excludes *.exe. Every clean checkout failed
REM     here with cmd's "The system cannot find the path specified" — including CI, on every commit.
REM ---------------------------------------------------------------------------
set "PREMAKE=%ROOT%\vendor\bin\premake5.exe"
where premake5 >NUL 2>&1
if not errorlevel 1 (
    set "PREMAKE=premake5"
    echo --- premake5 found on PATH
) else (
    if exist "%PREMAKE%" (
        echo --- premake5 present in vendor\bin
    ) else (
        echo --- Downloading premake5 %PREMAKE_VERSION%
        if not exist "%ROOT%\vendor\bin" mkdir "%ROOT%\vendor\bin"
        powershell -NoProfile -ExecutionPolicy Bypass -Command ^
            "$ErrorActionPreference = 'Stop';" ^
            "$url = 'https://github.com/premake/premake-core/releases/download/v%PREMAKE_VERSION%/premake-%PREMAKE_VERSION%-windows.zip';" ^
            "$zip = Join-Path $env:TEMP 'premake5.zip';" ^
            "Invoke-WebRequest -Uri $url -OutFile $zip;" ^
            "Expand-Archive -Path $zip -DestinationPath '%ROOT%\vendor\bin' -Force;" ^
            "Remove-Item $zip -Force"
        if errorlevel 1 call :fail "downloading premake5 %PREMAKE_VERSION%"
    )
)

REM ---------------------------------------------------------------------------
REM 6. Project files.
REM
REM NO GenVersion.bat CALL HERE. Setup runs once per machine, so the header it wrote was frozen from
REM then on and every msbuild after it — including every CI build — reported that frozen version.
REM Generating it is now Common.vcxproj's PreBuildEvent, which runs on every build. Calling it here as
REM well would hide a PreBuildEvent that had stopped firing: a missing header is a compile error, a
REM stale one is a lie.
REM ---------------------------------------------------------------------------
echo --- Generating project files ^(premake5 %PREMAKE_ACTION%^)
"%PREMAKE%" %PREMAKE_ACTION%
if errorlevel 1 call :fail "premake5 %PREMAKE_ACTION%"

REM ---------------------------------------------------------------------------
REM 7. Is everything premake5 and the generated projects read actually on disk?
REM
REM premake5 raises a hard error of its own for a few of these. Everything else is pulled in by a
REM wildcard, so when it is missing premake5 still succeeds and emits a project with no source files —
REM the failure only surfaces later as a link error naming a symbol you can see in the tree. Name them
REM here instead. Same list as scripts/MacOS/Setup.sh, plus the two Windows-only prebuilt binaries.
REM
REM Three submodules are deliberately absent: ThirdParty/NVRHI, ThirdParty/lightweightvk and
REM Editor/ThirdParty/ImGuiColorTextEdit. No premake file and no source refers to them. They are still
REM initialised above and a failure to initialise one is still reported — but the build does not read
REM them, so their absence must not be reported as a missing build input.
REM ---------------------------------------------------------------------------
echo --- Verifying the paths the build reads
set "MISSCOUNT=0"
for %%P in (
    "ThirdParty\optick\src\optick.h"
    "ThirdParty\meshoptimizer\src\meshoptimizer.h"
    "ThirdParty\reflect-cpp\src\reflectcpp.cpp"
    "ThirdParty\reflect-cpp\include"
    "ThirdParty\volk\volk.h"
    "ThirdParty\GLFW\include\GLFW\glfw3.h"
    "ThirdParty\ImGui\imgui.cpp"
    "ThirdParty\imgui-node-editor\imgui_node_editor.cpp"
    "ThirdParty\yaml-cpp\src\parser.cpp"
    "ThirdParty\JoltPhysics\Jolt\Jolt.h"
    "ThirdParty\lua\lapi.c"
    "ThirdParty\spdlog\include\spdlog\spdlog.h"
    "ThirdParty\sol2\include\sol\sol.hpp"
    "ThirdParty\google-test\include\gtest\gtest.h"
    "ThirdParty\glm\glm\glm.hpp"
    "ThirdParty\entt\include\entt\entt.hpp"
    "ThirdParty\stb\include"
    "ThirdParty\VulkanAllocator"
    "Editor\ThirdParty\ImGuizmo\ImGuizmo.cpp"
    "Editor\ThirdParty\assimp\include\assimp\Importer.hpp"
    REM assimp is a SUBMODULE COMPILED FROM SOURCE since D40, so what has to be present is its source
    REM tree, not the two prebuilt import libraries this list used to name. Those files no longer exist
    REM and this check would have reported MISSING twice on every Windows setup.
    "Editor\ThirdParty\assimp\code\Common\ImporterRegistry.cpp"
    "Desert.sln"
) do (
    if not exist "%ROOT%\%%~P" (
        set /a MISSCOUNT+=1
        set "MISS_!MISSCOUNT!=%%~P"
        echo     MISSING: %%~P
    )
)
if "%MISSCOUNT%"=="0" echo     all required paths present

REM ---------------------------------------------------------------------------
REM 8. Summary
REM ---------------------------------------------------------------------------
echo.
if "%FAILCOUNT%"=="0" if "%MISSCOUNT%"=="0" (
    echo === Setup complete ===
    echo Build with:  scripts\Windows\BuildWindows.bat [Debug^|Release] [--with-tests]
    echo              ^(or open Desert.sln^)
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
echo   and generates Desert.sln. Re-runnable.
echo.
echo   --no-install   Report what is missing and do not install anything. Everything that is a
echo                  clone rather than an installer still runs.
echo.
exit /b 2

:fail
set /a FAILCOUNT+=1
set "FAIL_%FAILCOUNT%=%~1"
echo     [FAILED] %~1 1>&2
exit /b 0

REM --- MSVC ------------------------------------------------------------------
REM WHAT WE ACTUALLY NEED IS NOT "VISUAL STUDIO". It is MSBuild, the MSVC v143 toolset and a Windows
REM SDK. CI gets exactly that from the windows-latest image plus microsoft/setup-msbuild; nothing
REM there installs an IDE. So the check is for the components, and the offered install is Build Tools
REM for Visual Studio 2022 — a SEPARATE instance, which is why installing it cannot disturb an
REM existing Visual Studio.
REM
REM WHY v143 SPECIFICALLY, AND WHY 2022 IS THE FLOOR: premake's `vs2022` action writes
REM PlatformToolset=v143 into every .vcxproj. A newer Visual Studio
REM (17.14, or an 18.x "2026") satisfies this only if it ALSO carries v143 — its own default toolset
REM is a different one, and msbuild fails with MSB8020 ("The build tools for v143 cannot be found")
REM rather than quietly retargeting. Checking the directory is therefore the honest check; checking
REM the IDE's version number is not, in either direction.
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
REM toolset premake asks for, and the FLOOR is enforced by the VS version above, not by this number.
for /d %%d in ("!VSINSTALL!\VC\Tools\MSVC\14.*") do set "V143=%%~nxd"
if not defined V143 (
    echo [ERROR] "!VSINSTALL!" has no MSVC v143 toolset ^(no VC\Tools\MSVC\14.* directory^).
    echo         The generated solution asks for PlatformToolset v143; msbuild fails with MSB8020
    echo         without it. Add it in the Visual Studio Installer:
    echo             Microsoft.VisualStudio.Component.VC.Tools.x86.x64
    exit /b 1
)

set "MSBUILD="
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe 2^>NUL`) do set "MSBUILD=%%i"
if not defined MSBUILD (
    echo [ERROR] MSVC v143 is installed but MSBuild.exe was not found in "!VSINSTALL!".
    echo         Add Microsoft.Component.MSBuild in the Visual Studio Installer.
    exit /b 1
)
echo --- MSVC: v!V143! in "!VSINSTALL!"
exit /b 0

:msvc_missing
echo --- MSVC v143 toolset: NOT FOUND
if "%NO_INSTALL%"=="1" (
    echo [ERROR] No Visual Studio 2022+ instance carries the MSVC v143 toolset. Install it with:
    echo         winget install --id Microsoft.VisualStudio.2022.BuildTools -e --override "--quiet --wait --norestart --add Microsoft.VisualStudio.Workload.VCTools --add Microsoft.VisualStudio.Component.VC.Tools.x86.x64 --add Microsoft.VisualStudio.Component.VC.Redist.14.Latest --add Microsoft.VisualStudio.Component.Windows11SDK.22621"
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
    --override "--quiet --wait --norestart --add Microsoft.VisualStudio.Workload.VCTools --add Microsoft.VisualStudio.Component.VC.Tools.x86.x64 --add Microsoft.VisualStudio.Component.VC.Redist.14.Latest --add Microsoft.VisualStudio.Component.Windows11SDK.22621"
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
REM The engine links shaderc*.lib and spirv-cross-*.lib out of the SDK's Lib directory, so a
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
REM Set it for this process AND for future shells. Desert/Dependencies.lua reads VULKAN_SDK first and
REM only then guesses at Program Files, so leaving it unset makes premake pick whichever SDK version
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
