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
Build<Platform>  [Debug|Release] [--with-tests] [--gen-only] — generate, then compile
RunEditor        [Debug|Release] [editor args...]
RunRuntime       [Debug|Release] [--project ...] [--scene ...]  — standalone player
RunTests         the unit-test suites (RunTests.sh on macOS, RunTests.ps1 on Windows)
Package          [Release|Debug] — the downloadable engine drop into dist/
```

Windows also has `scripts\Windows\RebuildAssimp.bat`, which rebuilds the one third-party dependency
this repository commits as a prebuilt binary. It is not part of setting a machine up.

Formatting is gated on **clang-format 18** and nothing else; `Setup.sh` installs it and prints the
one command that runs the gate the way CI does.

## CI

Every push/PR runs a clang-format gate on the changed lines, builds macOS Debug under ASan+UBSan and
macOS Release, and builds Windows Debug + Release — each of them running the full test suite
(`.github/workflows/ci.yml`).
