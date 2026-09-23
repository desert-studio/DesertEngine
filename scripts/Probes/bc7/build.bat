@echo off
REM Builds the BC7 upload+sample probe on Windows. See bc7upload.c for what it measures.
REM
REM THIS IS THE RUN THAT MATTERS. On macOS the probe passes with the feature enabled AND disabled,
REM so macOS cannot tell us whether the enable is load-bearing. A Windows driver is free to enforce
REM it, which is what `bc7upload.bat --disabled` is here to find out.
REM
REM Run from a Developer Command Prompt with the Vulkan SDK installed (VULKAN_SDK set).
setlocal
cd /d "%~dp0"

if "%VULKAN_SDK%"=="" (
    echo VULKAN_SDK is not set. Install the Vulkan SDK and open a shell where it is. 1>&2
    exit /b 1
)

"%VULKAN_SDK%\Bin\glslc.exe" -fshader-stage=compute bc7fetch.comp -o bc7fetch.spv -mfmt=c
if errorlevel 1 exit /b 1

REM /TC forces C. The file uses C designated initialisers, which C++20 rejects when a field is named
REM out of declaration order; they are written in order here, but /TC removes the question entirely.
cl /nologo /TC /std:c11 /O2 /I "%VULKAN_SDK%\Include" bc7upload.c /link /LIBPATH:"%VULKAN_SDK%\Lib" vulkan-1.lib /OUT:bc7upload.exe
if errorlevel 1 exit /b 1

echo built bc7upload.exe
echo run:  bc7upload.exe            (feature enabled -- the thing under test)
echo       bc7upload.exe --disabled (negative control; THIS is the interesting one on Windows)
