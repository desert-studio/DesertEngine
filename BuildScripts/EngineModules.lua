-- THE ENGINE IS SEVERAL STATIC LIBRARIES, ONE PER GROUP OF SUBSYSTEMS (UE's modules), AND THIS IS THE ONE
-- PLACE THAT SAYS WHICH. Desert/Desert/premake5.lua defines one project per entry below with one shared
-- function; every consumer links them through DesertEngineModules.Link() and never names one by hand.
--
-- WHY. A COFF archive cannot exceed 4 GB (LNK1248 "image size exceeds FFFFFFFF"), and the single Debug
-- Desert.lib reached 4.30 GB: code volume (reflect-cpp and EnTT instantiations), not debug information,
-- which /Zi already keeps in the PDB. Measured 2026-10-14 on the Debug objects, MiB per source subdir:
-- Graphic 985, Assets 703, Core 459, Runtime 315, Geometry 292, Animation 224, Scripting 159, ECS 110,
-- World 78, ShaderResources 77, UI 59, Destruction 30, Project 27, Input 26, Localization 25, Physics 23,
-- VFX 19, Media 13, Reflection 11, Text 10, Platform 10, Audio 9, Generated 8, Vector 2. The groups below
-- keep the largest module near 1.1 GB, under half the limit; a module that approaches 2 GB is split
-- further here, and nowhere else.
--
-- THE RULES, which Desert/Tests/Engine/EngineModules asserts by reading this file:
--   * every directory under Desert/Desert/Source/Engine belongs to exactly ONE module (a new subsystem
--     directory that nobody adds here fails the census instead of compiling into nothing);
--   * the module that lists "Generated" runs DesertHeaderTool (its prebuild regenerates
--     Engine/Generated/*.gen.cpp), exactly once, and every other module dependson it;
--   * `platform = true` marks the module that also compiles Source/Platform/<OS> and the vendored
--     single-file libraries (VMA, vk-bootstrap, stb, miniaudio); exactly one module carries it.
--
-- Keep each entry on ONE line in this exact shape: the census parses it as text.
DesertEngineModules = {
    Modules = {
        { name = "DesertCore",      platform = true, dirs = { "Core", "ECS", "Reflection", "Generated", "Project", "Input", "Localization", "Text", "Audio", "Media", "Vector" } },
        { name = "DesertAssets",    dirs = { "Assets" } },
        { name = "DesertGraphic",   dirs = { "Graphic", "ShaderResources", "VFX" } },
        { name = "DesertWorld",     dirs = { "World", "Geometry", "Physics", "Destruction" } },
        { name = "DesertAnimation", dirs = { "Animation", "Hair" } },
        { name = "DesertRuntime",   dirs = { "Runtime", "Scripting", "UI" } },
    },
}

-- The module that compiles Engine/Generated and therefore owns the DesertHeaderTool prebuild.
function DesertEngineModules.CodegenModule()
    for _, module in ipairs(DesertEngineModules.Modules) do
        for _, dir in ipairs(module.dirs) do
            if dir == "Generated" then
                return module.name
            end
        end
    end
    error("BuildScripts/EngineModules.lua: no engine module lists \"Generated\"", 0)
end

-- LINK EVERY ENGINE MODULE. The modules reference each other in both directions (Graphic calls Assets and
-- Assets calls Graphic), and that is fine on both linkers this workspace uses, WITHOUT `linkgroups`:
--   * MSVC link.exe searches every library on the command line until nothing new resolves, in any order;
--   * Apple ld64 (macOS, clang + gmake) does the same: it keeps searching a static archive for as long as
--     undefined symbols remain, so archive order does not matter there either. `linkgroups "On"` would
--     emit -Wl,--start-group, which is a GNU ld option.
-- The order below (the table reversed: highest-level module first, DesertCore last) is still the one a
-- single-pass GNU ld would need for the downward edges, so the list reads the dependency direction.
function DesertEngineModules.Link()
    local names = {}
    for i = #DesertEngineModules.Modules, 1, -1 do
        table.insert(names, DesertEngineModules.Modules[i].name)
    end
    links(names)
end

-- For a project that does not link the engine but compiles a file the engine's codegen writes
-- (Engine/Generated/Reflection.gen.cpp): it must build after that prebuild has run.
function DesertEngineModules.DependOnCodegen()
    dependson { DesertEngineModules.CodegenModule() }
end
