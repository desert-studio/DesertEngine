-- Config entry point for Desert Engine.
-- Shared settings are split into BuildScripts/:
--   Platform.lua        — platform detection helpers (DesertPlatform)
--   Workspace.lua       — the workspace + settings common to every project
--   Configurations.lua  — Debug / Release configurations
--   PlatformWindows.lua — Windows-wide settings (x64)
--   PlatformMacOS.lua   — macOS-wide settings (Apple Silicon / ARM64)

include "BuildScripts/Platform.lua"
include "BuildScripts/Workspace.lua"
include "BuildScripts/Configurations.lua"
include "BuildScripts/PlatformWindows.lua"
include "BuildScripts/PlatformMacOS.lua"

group "ThirdParty"
include "ThirdParty/"
group ""

group "Tools"
include "Tools/DesertHeaderTool/"
include "Tools/FbxMeshSplitter/"
include "Tools/DShaderTool/"
include "Tools/PakTool/"
include "Tools/SceneMigrator/"
include "Tools/WorldGen/"
include "Tools/ImageStat/"
include "Tools/LineJump/"
include "Tools/ImageDiff/"
include "Tools/DesertCtl/"
include "Tools/DomeSheet/"
group ""

include "Desert/"

-- AFTER Desert/, which is where the `deps` table is defined. Every tool above it is dependency-free or
-- vendors its own single header; this one compiles an engine source that uses glm and the Result type,
-- so it needs the same include paths the engine has.
group "Tools"
include "Tools/CloudVolumeBaker/"
include "Tools/CloudLayoutBaker/"
include "Tools/LatticePeak/"
-- Below Desert/ for the `deps` table, and it also compiles two Editor translation units — the ones
-- that carry the asset-reference token rule and deliberately do not include the engine. It must NOT
-- move above Editor/: nothing here links the Editor project, and duplicating those two objects into a
-- tool is the point (see Tools/AssetClosure/premake5.lua).
include "Tools/AssetClosure/"
group ""

include "Editor/"
include "Runtime/"
