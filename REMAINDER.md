# NG6-DEV-A remainder (second run, context limit reached)

Done in run 2 (this commit): D6 `scripts/Windows/BuildWindows.bat` (--launcher/LAUNCHER_ARG -> dev's CACHE_ARG from
BUILDPOOL_CACHE_PROGRAM, presets win-dev/win-debug/win-ship kept), D6 `scripts/MacOS/BuildMacOS.sh` (--no-ccache/
CCACHE/LAUNCHER -> CACHE_PROGRAM/CACHE_ARGS/pool_ccache, POOL_HOME from build/cmake/$PRESET/CMakeCache.txt),
D6 `.github/workflows/ci.yml` (3-way merge of dev's pool/cache steps onto ng6build presets: BUILDPOOL_HOME on the
build steps, Cap ccache after each build with Development in place of Release, Architecture census steps kept, no
--unity/--launcher), D8 `MachineSettings.cpp` (dev's Notes + MigrateBloomOverrides kept beside ng6build's
MegaLights migration; chain temporal -> megaLights -> bloom -> ScreenOverrides at MachineSettings.cpp:586-598).

Left:
1. arch_census reports 24 RISE. 22 are dev's merged code outside this package (discarded-result in
   Editor/.../Sequencer/LevelBoneRows.cpp x11, Engine/Animation/Animator.cpp x6, VulkanFrameLoop.cpp x2,
   LevelSequenceTimeline.cpp:251, WorldPartitionPanel.cpp:2240; file-too-big ViewportPanel.cpp) - lead/other packages.
2. Mine (D4): layer-matrix Core -> Engine at Engine/Core/Serialize/WorldCellsHLODStatus.hpp:7
   (WorldPartitionHLODHierarchy.hpp) and WorldCellsHLODStatus.cpp:6 (WorldPartitionResidencyRules.hpp). Adding those two
   to the Core placement (cmake/DesertModuleTable.cmake:82) pulls WorldPartitionFormat.hpp and
   WorldPartitionStreamingRules.hpp along (ResidencyRules.hpp:81-83); decide: place the whole closure in Core, or place
   WorldCellsHLODStatus in Engine (dev's own table places it in Core while its Core has the same chain - check how
   dev's census passes: `git show origin/dev:scripts/Dev/arch_baseline.json`, rule layer-matrix).
3. Reviewer chain per the brief, then `studio.py accept NG6-DEV-A done`.
