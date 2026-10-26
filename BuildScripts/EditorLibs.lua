-- THE EDITOR AS FIVE LAYERED STATIC LIBRARIES (EDITOR-LIBS). One EditorCore archive held every editor source
-- but the entry point - 281 objects, 1.3 GB in Release - and an edit to any one .cpp re-archived all of it
-- (~2.5 min of lib.exe before the link could start). The editor is now cut along the dependency direction it
-- already had, the way the engine is cut into module libraries (BuildScripts/DesertModuleLibs.lua):
--
--   EditorCore    <- EditorImport <- EditorWidgets <- EditorPanels <- EditorLevel
--
-- A library may include the headers of the libraries to its LEFT only; nothing includes rightwards. An edit
-- re-archives one layer, and the biggest layer (EditorPanels) is under half of the old archive.
--
-- Read here, once, before any project, because three scripts link the editor and two of them are included
-- before Editor/premake5.lua: the Editor executable, the editor test runners (Desert/Tests/premake5.lua) and
-- GamePackager (Tools/GamePackager/premake5.lua). Each goes through DesertEditorLibs.Link(); no script names an
-- editor library by hand.
--
-- Where a source goes is decided by the longest directory prefix below (relative to Editor/Source/Editor), so
-- a whole directory moves with one row and a sub-directory can sit in a different layer from its parent. A
-- source no row claims fails premake with its path, rather than silently dropping out of every library.

DesertEditorLibs = {}

DesertEditorLibs.SourceRoot = _MAIN_SCRIPT_DIR .. "/Editor/Source"

-- LOWEST FIRST. Small directories sit in the lowest layer whose headers they need.
DesertEditorLibs.Layers = {
    -- The editor's model: commands and undo, selection and authoring, documents, preferences, control
    -- channel; the panel interface (IPanel.hpp) the document well and registries hold; the system file
    -- dialogs (Platform) and the editor's render passes (RenderSystems), which need nothing above Core.
    { Name = "EditorCore", Dirs = { "Core", "Platform", "RenderSystems" } },
    -- Source-file import (meshes, textures, landscape heightmaps) and the packager's cook, which calls it.
    { Name = "EditorImport", Dirs = { "Import", "Packaging" } },
    -- Reusable UI: thumbnails, preview viewports, controls; the ImGui host layer and the start-up splash,
    -- which draw with Widgets' styles.
    { Name = "EditorWidgets", Dirs = { "Widgets", "Splash", "ImGuiIntegration" } },
    -- Every panel and asset editor that another panel, or the level editor, may open.
    { Name = "EditorPanels", Dirs = { "Panels" } },
    -- The level editor: its shell (LevelEditor, EditorLayer, the generated subsystem list) and the panels
    -- only it hosts and no other panel includes - Details (SceneProperties, the component editor
    -- registrations), the Outliner (SceneHierarchy) and Modeling mode. They sit here, not in EditorPanels,
    -- because EditorPanels would otherwise pass ~650 MB in Release on its own.
    {
        Name      = "EditorLevel",
        Dirs      = { "LevelEditor", "Generated", "Panels/SceneProperties", "Panels/SceneHierarchy", "Panels/Modeling" },
        RootFiles = { "EditorLayer.cpp", "EditorLayer.hpp" },
    },
}

-- THE OBJECTIVE-C++ SOURCES (macOS only: Splash/MacOS, Platform/MacOS) are their own library, outside the
-- layers. Every layer force-includes its C++ precompiled header, and clang refuses a .gch built without
-- Objective-C in an Objective-C++ unit; gmake applies the forced include project-wide, so a per-file filter
-- cannot take it off. These sources need only the standard headers and a few small editor headers, so the
-- library has no precompiled header. It sits at EditorWidgets' level (it includes Splash/ headers).
DesertEditorLibs.ObjCLib = "EditorObjC"

-- The executable's entry point (project "Editor"), in no library: a test runner that links them has its own main.
DesertEditorLibs.EntryPoint = { ["Sandbox.cpp"] = true, ["Sandbox.hpp"] = true }

-- The library holding the editor's DesertHeaderTool prebuild: the one whose sources include what it writes
-- (Editor/Generated/EditorSubsystems.gen.cpp).
DesertEditorLibs.CodegenLib = "EditorLevel"

local function LayerOf( relative )
    local best, bestLength = nil, -1
    for _, layer in ipairs( DesertEditorLibs.Layers ) do
        for _, dir in ipairs( layer.Dirs ) do
            if relative:sub( 1, #dir + 1 ) == dir .. "/" and #dir > bestLength then
                best, bestLength = layer.Name, #dir
            end
        end
    end
    return best
end

local filesByLayer = {}

-- Absolute paths of every editor .mm, whichever layer's directory it sits in, sorted (DesertEditorLibs.ObjCLib).
function DesertEditorLibs.ObjCFiles()
    local result = os.matchfiles( DesertEditorLibs.SourceRoot .. "/Editor/**.mm" )
    table.sort( result )
    return result
end

-- Absolute paths of the sources a layer compiles, with the given extensions (default cpp/hpp/h/inl), sorted.
function DesertEditorLibs.FilesOf( name, extensions )
    extensions = extensions or { "cpp", "hpp", "h", "inl" }
    local key = name .. ":" .. table.concat( extensions, "," )
    if filesByLayer[key] then
        return filesByLayer[key]
    end
    local known = false
    for _, layer in ipairs( DesertEditorLibs.Layers ) do
        known = known or layer.Name == name
    end
    if not known then
        error( "BuildScripts/EditorLibs.lua: no editor library named '" .. name .. "'", 0 )
    end
    local result = {}
    for _, extension in ipairs( extensions ) do
        for _, path in ipairs( os.matchfiles( DesertEditorLibs.SourceRoot .. "/Editor/**." .. extension ) ) do
            local relative = path:sub( #DesertEditorLibs.SourceRoot + #"/Editor/" + 1 )
            local layer    = LayerOf( relative )
            if not layer then
                error( "BuildScripts/EditorLibs.lua: Editor/Source/Editor/" .. relative
                       .. " is in no editor library - give its directory a row in DesertEditorLibs.Layers", 0 )
            end
            if layer == name then
                table.insert( result, path )
            end
        end
        for _, layer in ipairs( DesertEditorLibs.Layers ) do
            if layer.Name == name then
                for _, file in ipairs( layer.RootFiles or {} ) do
                    if file:match( "%." .. extension .. "$" ) then
                        table.insert( result, DesertEditorLibs.SourceRoot .. "/" .. file )
                    end
                end
            end
        end
    end
    -- A root file that is neither a library's nor the entry point would be compiled by nothing.
    for _, path in ipairs( os.matchfiles( DesertEditorLibs.SourceRoot .. "/*" ) ) do
        local file    = path:sub( #DesertEditorLibs.SourceRoot + 2 )
        local claimed = DesertEditorLibs.EntryPoint[file] or file == "EditorPCH.hpp" or file == "EditorPCH.cpp"
        for _, layer in ipairs( DesertEditorLibs.Layers ) do
            for _, rootFile in ipairs( layer.RootFiles or {} ) do
                claimed = claimed or rootFile == file
            end
        end
        if not claimed then
            error( "BuildScripts/EditorLibs.lua: Editor/Source/" .. file .. " is in no editor library", 0 )
        end
    end
    table.sort( result )
    filesByLayer[key] = result
    return result
end

-- LINK THE WHOLE EDITOR: every library, highest layer first (a single-pass linker resolves left to right, so a
-- dependant comes before what it uses). `wholeArchive` takes every object of every library, as the Editor and
-- the test runners need: the component editors register from static initialisers nothing references
-- (BuildScripts/Platform.lua, LinkWholeArchive). Without it the archives are ordinary and the linker takes only
-- the members something reaches (GamePackager). Call it with no filter active; it leaves none active.
function DesertEditorLibs.Link( wholeArchive )
    local names = {}
    for i = #DesertEditorLibs.Layers, 1, -1 do
        table.insert( names, DesertEditorLibs.Layers[i].Name )
    end
    if DesertPlatform.IsMacOS then
        table.insert( names, 1, DesertEditorLibs.ObjCLib )
    end
    links( names )
    if wholeArchive then
        for _, name in ipairs( names ) do
            DesertPlatform.LinkWholeArchive( name )
        end
    end
end
