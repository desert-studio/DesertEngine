# NG6-DEV-A remainder (context limit reached)

Done in this branch: D2 `CMakeLists.txt` (BuildPool block of dev + ng6build presets comment, DESERT_TIME_TRACE MSVC,
ShaderProtocolGen, tool order), D3 `cmake/DesertWorkspace.cmake` (Ninja link pools desert_link_exe/dll removed: the
build pool admits links; /Z7 stays unconditional for Development/DebugGame as ng6build has it, FASTLINK needs it),
D4 `cmake/DesertModuleTable.cmake` (+WorldCellsHLODStatus|WorldPartitionHLODMember, Serialize `SceneUpgrade` dropped),
D5 `scripts/Dev/_common.sh` dev_build and `scripts/Dev/compile_check.sh` run_pass through `pool.sh run`,
D7 `Desert/Desert/Source/Engine/Core/Modules.cpp` (include + RegisterLandscapeHLODInputs()).

Decided against the design: `CCACHE_COMPILERCHECK=content` is NOT added to CACHE_ENV - BuildPool.cmake:146 lists it
as the module's own setting (FATAL_ERROR if passed); the module sets `string:{compiler_id}` (BuildPool.cmake:174).

Left:
1. D6 `scripts/Windows/BuildWindows.bat`: replace `--launcher`/`LAUNCHER_ARG`/`DESERT_COMPILER_LAUNCHER` (lines 7, 20-21,
   45, 72-78, 84, 136-140) with dev's `CACHE_ARG` from `BUILDPOOL_CACHE_PROGRAM` (`git diff e948e78ed origin/dev --
   scripts/Windows/BuildWindows.bat`); keep ng6build presets win-dev/win-debug/win-ship.
2. D6 `scripts/MacOS/BuildMacOS.sh`: same with dev's diff - drop `--no-ccache`/`CCACHE`/`LAUNCHER` (lines 6, 13, 26, 47,
   74-84, 159-171, 188-190), add `CACHE_PROGRAM`/`CACHE_ARGS`/`pool_ccache` reading `build/cmake/$PRESET/CMakeCache.txt`
   (check the ng6build build dir for presets mac-dev/mac-debug/mac-ship in CMakePresets.json `binaryDir`).
3. D6 `.github/workflows/ci.yml`: grep `DESERT_COMPILER_LAUNCHER`/`--launcher`; take dev's pool/cache steps
   (`git diff e948e78ed origin/dev -- .github/workflows/ci.yml`) onto ng6build preset names.
4. D8 `Desert/Common/Source/Common/Settings/MachineSettings.cpp`: merge tree has ng6build side; add dev's block from
   `git diff e948e78ed origin/dev -- <file>` (both blocks kept).
5. D1 `.claude/` six files: merge commit holds dev's text; the hook refuses `.claude` edits to agents - lead runs
   `git checkout ng6build -- .claude/agents/architect.md .claude/agents/engineer.md .claude/tools/agent_guard.py
   .claude/tools/limits.py .claude/tools/pipeline.py .claude/tools/studio.py`. `wrapper_lint.py` is red on both sides
   already: `.claude/skills/desert-engine-contract/SKILL.md` 9761 > 9600 characters (lead's file).
6. `python scripts/Dev/arch_census.py`, commit, push, reviewer chain per the brief.
