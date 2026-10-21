-- THE ENGINE'S LEGACY LIBRARY, AS ONE STATIC LIBRARY PER MODULE (BUILD-MOD2). BuildScripts/DesertModuleLibs.lua
-- reads the module table (BuildScripts/DesertModules.lua) and places every file under its LegacyRoot with the
-- table's own Placement rows; this file defines one library per module that got files, all through
-- EngineModuleLibrary below, so pch, unity, defines, include directories, warnings and /bigobj are the same for
-- every one. Only the files, the module's Deps and the one codegen prebuild differ.
DesertModuleLibs.SetPrebuiltLibraries(deps.DesertSpecific.Libraries)

-- The vendored single-file third-party sources, compiled into the library of the module that uses them.
local kVendoredSources = {
    -- vk-bootstrap v1.3.290 (MIT, ThirdParty/vk-bootstrap/LICENSE.txt): instance, physical-device selection and
    -- logical-device creation. Vendored as its three source files, like VMA.
    RenderCore = {
        "%{_MAIN_SCRIPT_DIR}/ThirdParty/VulkanAllocator/vk_mem_alloc.cpp",
        "%{_MAIN_SCRIPT_DIR}/ThirdParty/vk-bootstrap/VkBootstrap.cpp",
    },
    ImageCore = { "%{_MAIN_SCRIPT_DIR}/ThirdParty/stb/stb_image.cpp" },
    TextCore  = { "%{_MAIN_SCRIPT_DIR}/ThirdParty/stb/stb_truetype.cpp" },
    Audio     = { "%{_MAIN_SCRIPT_DIR}/ThirdParty/miniaudio/miniaudio.cpp" },
}
for name in pairs(kVendoredSources) do
    if not DesertModuleLibs.LegacyFiles()[name] then
        error("Desert/Desert/premake5.lua: vendored sources assigned to '" .. name .. "', which builds no library", 0)
    end
end

-- Reflection codegen (UHT-style), THE ONE COMMAND LINE: DesertHeaderTool regenerates
-- Source/Engine/Generated/Reflection.gen.cpp (the module list) and Reflection_<Module>.gen.cpp (each module's
-- types, compiled by that module's library) from REFLECT()/PROPERTY() annotations. The same run verifies every
-- routed-event handler of the engine and Common (misspelt, non-public or outside the event tree fails the build
-- at file:line) and emits the DESERT_SUBSYSTEM lists of the application (Engine) and of every world (World,
-- owned by Scene). It runs once, as the prebuild of DesertModuleLibs.CodegenLib().
local kHeaderToolCommand =
    DesertPlatform.BuiltToolPath("DesertHeaderTool")
        .. ' --templates "' .. _MAIN_SCRIPT_DIR .. '/Tools/DesertHeaderTool/Templates"'
        .. ' --modules "' .. DesertModuleLibs.TableFile .. '"'
        .. ' --reflect "' .. _MAIN_SCRIPT_DIR .. '/Desert/Desert/Source" "Engine"'
        .. ' "' .. _MAIN_SCRIPT_DIR .. '/Desert/Desert/Source/Engine/Generated/Reflection.gen.cpp"'
        -- DesertUI's reflected UI*Data (UI-FW-4): its own project has no generator step, the engine's set
        -- registers them (Reflection_DesertUI.gen.cpp, which its Placement row gives to DesertUI's legacy library). --reflect-root extends the
        -- --reflect request above, so it must follow that request's three operands.
        .. ' --reflect-root "' .. _MAIN_SCRIPT_DIR .. '/Desert/UI/Source" "UI"'
        -- Enums only (no reflected types): a reflected field may be typed with Common's settings enums
        -- (PostProcessSettings' GIMethod / ReflectionMethod / ShadowMethod, Common/Settings/RenderMethods.hpp).
        .. ' --reflect-enums "' .. _MAIN_SCRIPT_DIR .. '/Desert/Common/Source/Common/Settings"'
        .. ' --reflect-components "' .. _MAIN_SCRIPT_DIR .. '/Desert/Desert/Source/Engine/Generated/ReflectedComponentBlocks.gen.hpp"'
        .. ' --check "' .. _MAIN_SCRIPT_DIR .. '/Desert/Desert/Source"'
        .. ' --check "' .. _MAIN_SCRIPT_DIR .. '/Desert/Common/Source"'
        .. ' --check "' .. _MAIN_SCRIPT_DIR .. '/Desert/UI/Source"'
        .. ' --check "' .. _MAIN_SCRIPT_DIR .. '/Desert/Render2DCore/Source"'
        .. ' --subsystems Engine Desert::Engine::Application Engine/Core/Application.hpp'
        .. ' "' .. _MAIN_SCRIPT_DIR .. '/Desert/Desert/Source/Engine/Generated/EngineSubsystems.gen.cpp"'
        .. ' --subsystems World Desert::Core::Scene Engine/Core/Scene.hpp'
        .. ' "' .. _MAIN_SCRIPT_DIR .. '/Desert/Desert/Source/Engine/Generated/WorldSubsystems.gen.cpp"'

local function EngineModuleLibrary(module, legacyFiles)
    project(DesertModules.LegacyLibName(module.Name))
    kind "StaticLib"
    DesertUnity.EnableForProject() -- no-op without --unity (BuildScripts/UnityBuild.lua)

    pchheader "pch.hpp"
    pchsource "Source/pch.cpp"
    forceincludes { "pch.hpp" }

    if DesertModules.LegacyLibName(module.Name) == DesertModuleLibs.CodegenLib() then
        dependson { "DesertHeaderTool" }
        prebuildcommands { kHeaderToolCommand }
    else
        DesertModuleLibs.DependOnCodegen()
    end

    -- Platform/Windows and Platform/MacOS (and any .mm) compile on their own system only. So do the vendor
    -- upscalers (Engine/Graphic/Upscalers/), every Streamline/ directory (the NVIDIA Streamline device
    -- integration DLSS runs on) and every FidelityFX/ directory (the device the FSR Vulkan backend assumes):
    -- their SDKs are built or shipped for Windows only (BuildScripts/ThirdParty/FidelityFX.lua,
    -- ThirdParty/Streamline).
    local commonFiles, windowsFiles, macFiles = { "Source/pch.cpp", "Source/pch.hpp" }, {}, {}
    for _, file in ipairs(legacyFiles) do
        local target = commonFiles
        if file:find("^Platform/Windows/") or file:find("^Engine/Graphic/Upscalers/") or file:find("/Streamline/")
            or file:find("/FidelityFX/") then
            target = windowsFiles
        elseif file:find("^Platform/MacOS/") or file:find("%.mm$") then
            target = macFiles
        end
        table.insert(target, "Source/" .. file)
    end
    files(commonFiles)
    files(kVendoredSources[module.Name] or {})

    includedirs {
        "Source/",
        "%{_MAIN_SCRIPT_DIR}/Desert/UI/Source",
        "%{_MAIN_SCRIPT_DIR}/Desert/Render2DCore/Source",
        "%{_MAIN_SCRIPT_DIR}/Desert/CoreReflection/Source",
        "%{_MAIN_SCRIPT_DIR}/Desert/Common/Source",
        -- The SHADER ROOT, for the one engine translation unit that compiles a shared `.glslh` AS C++:
        -- Graphic/SkyGroundTransmittance.cpp includes Common/SkyMedium.glslh so the sun light's colour
        -- and the transmittance LUT's texels come from one text (the arrangement the test references
        -- established). Nothing else in the engine may include a `.glslh` — the rest of the shader
        -- contract travels as payload structs with static_asserted offsets.
        "%{_MAIN_SCRIPT_DIR}/Editor/Resources/Shaders",
    }
    externalincludedirs {
        -- voro++ 0.4.6 (BuildScripts/ThirdParty/Voro.lua): the fracture bake's Voronoi cells.
        "%{_MAIN_SCRIPT_DIR}/ThirdParty/voro++/src",
        -- Engine/Media: AV1 (dav1d) and Opus, both compiled from their submodules
        -- (BuildScripts/ThirdParty/Dav1d.lua, Opus.lua).
        "%{_MAIN_SCRIPT_DIR}/ThirdParty/dav1d/include",
        "%{_MAIN_SCRIPT_DIR}/ThirdParty/opus/include",
        -- Luau, by module path (<VM/include/lua.h>, <Compiler/include/luacode.h>): its lua.h shares a name with
        -- PUC Lua's (DesertHeaderTool's), so the two VMs' headers can never resolve to each other.
        "%{_MAIN_SCRIPT_DIR}/ThirdParty/luau",
        -- Luau's C++ CodeGen API (<CodeGen/include/Luau/CodeGen.h>: the script debugger turns native execution
        -- off while attached) includes its siblings as "Luau/...", from these two roots.
        "%{_MAIN_SCRIPT_DIR}/ThirdParty/luau/CodeGen/include",
        "%{_MAIN_SCRIPT_DIR}/ThirdParty/luau/Common/include",
    }

    for name, path in DesertSortedPairs(deps.Common.IncludeDir) do
        externalincludedirs { path }
end

    for name, path in DesertSortedPairs(deps.DesertSpecific.IncludeDir) do
        externalincludedirs { path }
end

    -- NO "ImGui" HERE, AND THAT IS THE POINT OF Desert/Tests/Engine/ImGuiBoundary. Dear ImGui is the
    -- EDITOR's interface toolkit: the integration layer lives in Editor/Source/Editor/ImGuiIntegration/
    -- and the Editor links the library itself. While this line said "ImGui", every binary that linked the
    -- engine — the packaged Runtime included — carried a toolkit it never draws a pixel with.
    -- The libraries of the module's direct Deps (DesertModules.lua), then the third-party projects. Sibling
    -- PROJECTS only: a static library's `links` of a project is a build-order reference, not a copy.
    for _, dep in ipairs(module.Deps) do
        links(DesertModuleLibs.LibrariesOf(DesertModules.Find(dep)))
end
    links {
        "Jolt",
        -- Luau (BuildScripts/ThirdParty/Luau.lua): Engine/Scripting/Luau. Dependants before what they use.
        "LuauCodeGen",
        "LuauCompiler",
        "LuauAst",
        "LuauBytecode",
        "LuauVM",
        "LuauCommon",
        "Optick",
        "MeshOptimizer",
        "Voro",
        "OpenSubdiv",
        "Dav1d",
        "Opus",
    }

    for _, define in ipairs(deps.Common.Defines) do
        defines { define }
end

    -- NO PREBUILT ARCHIVE HERE (Vulkan, shaderc, reflect-cpp): DesertModuleLibs.Link() gives them to the
    -- executables, because lib.exe would copy each one into every module library that named it.

    filter "configurations:Debug"
        defines { "DESERT_CONFIG_DEBUG" }
        symbols "On"

    filter "configurations:Release"
        defines { "DESERT_CONFIG_RELEASE" }

    filter { "system:windows" }
        defines { "DESERT_PLATFORM_WINDOWS" }
        -- /bigobj: MSVC caps an object file at 65279 sections, and the generated reflection +
        -- component-registry translation units (one template instantiation per reflected type) blow
        -- past it. Clang has no such limit, which is why this only bites the Windows build.
        buildoptions { "/bigobj" }
        files(windowsFiles)
        -- The vendor upscalers (Engine/Graphic/API/UpscalerBuild.hpp reads the defines; the windowsFiles above
        -- hold their sources). FSR 3.1: Upscalers/FSR on the FidelityFX SDK. DLSS: Upscalers/DLSS on NVIDIA
        -- Streamline, whose device integration is API/Vulkan/Streamline (registered by VulkanDeviceExtensions.cpp).
        defines { "DESERT_UPSCALER_FSR", "DESERT_UPSCALER_DLSS" }
        externalincludedirs {
            "%{_MAIN_SCRIPT_DIR}/ThirdParty/FidelityFX-SDK/sdk/include",
            "%{_MAIN_SCRIPT_DIR}/ThirdParty/Streamline/include",
        }
        links { "FidelityFX" }

    filter { "system:macosx" }
        defines { "DESERT_PLATFORM_MACOS" }
        files(macFiles)

    filter {}
end

for _, module in ipairs(DesertModules.Modules) do
    local legacyFiles = DesertModuleLibs.LegacyFiles()[module.Name]
    if legacyFiles then
        EngineModuleLibrary(module, legacyFiles)
    end
end
