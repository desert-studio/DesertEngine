# DesertEngine

[![CI](https://github.com/Y0MMY/DesertEngine/actions/workflows/ci.yml/badge.svg?branch=dev)](https://github.com/Y0MMY/DesertEngine/actions/workflows/ci.yml)

C++20 / Vulkan game engine with an ImGui editor, Lua gameplay scripting, a custom shader language,
a project system and a standalone Runtime player. Runs on macOS (Apple Silicon, MoltenVK) and
Windows (MSVC, LunarG Vulkan SDK).

## Quick start (macOS)

```bash
./scripts/MacOS/Setup.sh              # one-time: brew deps + submodules + third-party fetch
./scripts/MacOS/BuildMacOS.sh Debug   # generate project files and build everything
./scripts/MacOS/RunEditor.sh  Debug   # no --project = the built-in Desert Sandbox
```

## Quick start (Windows)

```bat
scripts\Windows\Setup.bat                       :: one-time: MSVC v143 + Vulkan SDK + submodules + Desert.sln
scripts\Windows\BuildWindows.bat Debug          :: generate project files and build everything
scripts\Windows\RunEditor.bat    Debug          :: no --project = the built-in Desert Sandbox
```

`Setup.bat` installs what is missing (Build Tools for Visual Studio 2022 through `winget`, the pinned
LunarG Vulkan SDK) and prompts through UAC while doing it. Pass `--no-install` to have it report what
is missing and change nothing. **Visual Studio 2022 or newer is required and the MSVC **v143**
toolset specifically**: the generated solution asks for `PlatformToolset=v143`, so a newer Visual
Studio satisfies it only if v143 is one of its installed toolsets.

## Both platforms

```
Setup            install every dependency and generate the project files. Re-runnable.
Build<Platform>  [Debug|Release] [--with-tests] [--gen-only] [--no-analyze] — generate, compile,
                 then run clang-tidy over the lines you changed
RunEditor        [Debug|Release] [editor args...]
RunRuntime       [Debug|Release] [--project ...] [--scene ...]  — standalone player
RunTests         the unit-test suites (RunTests.sh on macOS, RunTests.ps1 on Windows)
Package          [Release|Debug] — the downloadable engine drop into dist/
```

This repository commits **no prebuilt third-party binary**. assimp was the last one — two MSVC import
libraries whose toolset was typed into the link line and whose DLL half was never committed at all — and
since D40 it is a pinned submodule (`Editor/ThirdParty/assimp`, v6.0.5) compiled from source by
`BuildScripts/ThirdParty/Assimp.lua` on every platform, like the eight dependencies beside it. The
formats it is built with are one named row each in `BuildScripts/ThirdParty/AssimpImporters.txt`, and
`Desert/Tests/Editor/{AssimpBoundary,AssimpLibraryPin}` assert that the register, the build and the
linked library still agree.

Formatting is gated on **clang-format 18** and nothing else; `Setup.sh` installs it and prints the
one command that runs the gate the way CI does.

Static analysis is gated on **clang-tidy 18**, from the same Homebrew keg, and it is ON BY DEFAULT at
the end of a macOS build. It reads the lines you changed against your branch's merge-base, never the
whole tree — `.clang-tidy` reports five figures of diagnostics over the workspace, so a whole-tree
gate would be red forever and would simply be switched off. Run it on its own with
`scripts/CI/CheckTidy.sh [<base>]`, or over everything with `scripts/CI/CheckTidy.sh --all`. Its exit
code is its interface: **0 clean, 1 findings, 2 the gate could not run** — and the build script fails
on 2 exactly as it fails on 1, because "no verdict" must never read as "clean". The compiler
database it needs is not a separate ritual: the gate regenerates `compile_commands.json` itself, from
the makefiles premake already generated for the build you just ran.

Windows has no entry point for the analyser yet (`--no-analyze` is required there): the database is
derived from premake's gmake2 makefiles, which describe a toolchain MSVC does not use.

## CI

Every push/PR runs a clang-format gate and a clang-tidy gate on the changed lines, builds macOS Debug
under ASan+UBSan and macOS Release, and builds Windows Debug + Release — each of them running the full
test suite (`.github/workflows/ci.yml`). The clang-tidy job is deliberately off the critical
path — nothing waits on it and it waits on nothing — because Windows already runs at 73-93 minutes
against a 150-minute ceiling.
