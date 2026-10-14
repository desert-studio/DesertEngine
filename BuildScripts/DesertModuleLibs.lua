-- THE ENGINE AS ONE STATIC LIBRARY PER MODULE (BUILD-MOD2, ENG-MODULES). The old single library (Desert.lib)
-- passed the 4 GB archive limit in Debug (LNK1248); premake now builds every module of
-- BuildScripts/DesertModules.lua that has files under its LegacyRoot as its own library,
-- DesertModules.LegacyLibName( module ), from exactly the files its Placement rows give it. A module that
-- already lives in its own Folder project (Common, Render2DCore, DesertUI) keeps that project.
--
-- Read here, once, before any project: the libraries are defined by Desert/Desert/premake5.lua, and every
-- project that uses the engine goes through DesertModuleLibs.Link() / DesertModuleLibs.DependOnCodegen() -
-- no premake5.lua names a module library by hand.

local kModuleTable = _MAIN_SCRIPT_DIR .. "/BuildScripts/DesertModules.lua"

-- Validated at generation time, so a dependency on an unknown or later-listed module (a cycle) or a placement
-- pattern outside the supported subset fails `premake5` with the row named.
DesertModules = dofile( kModuleTable )
DesertModules.Validate()

DesertModuleLibs = { TableFile = kModuleTable }

-- The legacy root's shared precompiled header: every library compiles its own pch.cpp from it, so it belongs
-- to all of them and is the one pair of files no row places.
DesertModuleLibs.SharedPch = { ["pch.cpp"] = true, ["pch.hpp"] = true }

local kLegacyExtensions = { "cpp", "hpp", "h", "inl", "mm" }
local legacyFiles

-- module name -> its legacy files (relative to LegacyRoot, sorted), for every module that has at least one.
function DesertModuleLibs.LegacyFiles()
    if legacyFiles then
        return legacyFiles
    end
    legacyFiles = {}
    local root = _MAIN_SCRIPT_DIR .. "/" .. DesertModules.LegacyRoot
    for _, extension in ipairs( kLegacyExtensions ) do
        for _, file in ipairs( os.matchfiles( root .. "/**." .. extension ) ) do
            local relative = file:sub( #root + 2 )
            if not DesertModuleLibs.SharedPch[relative] then
                local module = DesertModules.PlaceLegacy( relative )
                legacyFiles[module] = legacyFiles[module] or {}
                table.insert( legacyFiles[module], relative )
            end
        end
    end
    for _, files in pairs( legacyFiles ) do
        table.sort( files )
    end
    return legacyFiles
end

-- A module whose Folder is a project of its own: Desert/<Dir>/premake5.lua beside Desert/<Dir>/Source/.
function DesertModuleLibs.HasOwnProject( module )
    return os.isfile( _MAIN_SCRIPT_DIR .. "/" .. path.getdirectory( path.getdirectory( module.Folder ) ) .. "/premake5.lua" )
end

-- The libraries a module is linked as: its own project, then its legacy library, each when it exists.
function DesertModuleLibs.LibrariesOf( module )
    local libraries = {}
    if DesertModuleLibs.HasOwnProject( module ) then
        table.insert( libraries, module.Name )
    end
    if DesertModuleLibs.LegacyFiles()[module.Name] then
        table.insert( libraries, DesertModules.LegacyLibName( module.Name ) )
    end
    return libraries
end

-- THE ONE PREBUILD SITE: the library of the first module, in layer order, that has legacy files. A module only
-- depends on modules listed before it, so that library links no other legacy library and every other one can
-- `dependson` it without a cycle - all of them compile after DesertHeaderTool has written Engine/Generated.
function DesertModuleLibs.CodegenLib()
    for _, module in ipairs( DesertModules.Modules ) do
        if DesertModuleLibs.LegacyFiles()[module.Name] then
            return DesertModules.LegacyLibName( module.Name )
        end
    end
    error( "DesertModuleLibs.lua: no module has a file under " .. DesertModules.LegacyRoot, 0 )
end

-- For a project that does not link the engine but compiles against (or compiles) what its codegen writes.
function DesertModuleLibs.DependOnCodegen()
    dependson { DesertModuleLibs.CodegenLib() }
end

-- The prebuilt third-party archives (Vulkan, shaderc, SPIRV-Cross, reflect-cpp), per configuration. Named by
-- the EXECUTABLES, never by a module library: lib.exe copies a library file named in a static library's
-- `links` INTO that library, which is half a gigabyte per module (measured on BUILD-MOD1).
local prebuilt

function DesertModuleLibs.SetPrebuiltLibraries( libraries )
    prebuilt = libraries
end

-- LINK THE WHOLE ENGINE: every module's libraries, highest layer first, then the prebuilt archives. Call it
-- with no filter active; it leaves none active.
-- The module libraries reference each other in both directions (the code still crosses module boundaries
-- upward - the ModuleBoundary suite's report), and neither linker needs a group for that: MSVC link.exe and
-- Apple ld64 (macOS, clang + gmake) both keep searching every archive on the command line while undefined
-- symbols remain, so NO `linkgroups` (it would emit GNU ld's --start-group, which ld64 rejects). The list is
-- still ordered dependants-first, the order a single-pass linker needs for the declared Deps.
function DesertModuleLibs.Link()
    if not prebuilt then
        error( "DesertModuleLibs.Link() before Desert/Desert/premake5.lua set the prebuilt libraries: include \"Desert/\" first", 0 )
    end
    local names = {}
    for i = #DesertModules.Modules, 1, -1 do
        for _, library in ipairs( DesertModuleLibs.LibrariesOf( DesertModules.Modules[i] ) ) do
            table.insert( names, library )
        end
    end
    links( names )
    filter "configurations:Debug"
        for _, archive in DesertSortedPairs( prebuilt.Debug ) do
            links { archive }
        end
    -- `or Shipping`: the shipping build links the SAME third-party flavour Release does.
    filter "configurations:Release or Shipping"
        for _, archive in DesertSortedPairs( prebuilt.Release ) do
            links { archive }
        end
    filter {}
end
