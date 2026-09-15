-- ThirdParty projects. The project definitions live in BuildScripts/ThirdParty/
-- (the submodules themselves don't carry premake files), so they can be
-- versioned with the engine and shared across platforms.

local buildScripts = _MAIN_SCRIPT_DIR .. "/BuildScripts/ThirdParty"

include( buildScripts .. "/GLFW.lua" )
include( buildScripts .. "/ImGui.lua" )
include( buildScripts .. "/ImGuiNodeEditor.lua" )
include( buildScripts .. "/YamlCpp.lua" )
include( buildScripts .. "/Jolt.lua" )
include( buildScripts .. "/Lua.lua" )
include( buildScripts .. "/Optick.lua" )
include( buildScripts .. "/MeshOptimizer.lua" )
include( buildScripts .. "/Dlib.lua" ) -- optional; no-op when ThirdParty/dlib is absent

-- assimp, compiled from the pinned submodule on every platform. It used to be a committed MSVC import
-- library on Windows and Homebrew's copy on macOS, with no version pinned on either side; see the file.
include( buildScripts .. "/Assimp.lua" )

-- reflect-cpp is compiled from the vendored v0.19.0 sources on EVERY platform. Windows
-- used to link a prebuilt reflectcpp.lib instead, of which only the Debug flavour was
-- ever committed — the Release job died on LNK1181 from the day it existed.
include( buildScripts .. "/ReflectCpp.lua" )

-- gtest, from the vendored v1.17.0 sources — Windows only; the unix-likes link the
-- package manager's build by name (see the file for why).
if os.target() == "windows" then
    include( buildScripts .. "/GoogleTest.lua" )
end
