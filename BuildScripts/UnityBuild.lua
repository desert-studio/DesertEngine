-- UNITY (JUMBO) BUILD FOR THE WINDOWS CI JOB, AND FOR NOTHING ELSE.
--
-- `premake5 vs2022 --unity` turns on MSBuild's own unity support (<EnableUnitySupport>) for the three
-- projects that hold almost all of the code -- Common, Desert and Editor -- through
-- DesertUnity.EnableForProject(), which each of those project scripts calls. MSBuild then writes
-- unity_*.cpp files into IntDir, each #including up to MaxFilesInUnityFile of the project's sources,
-- and compiles those instead of the sources one by one: the headers every source repeats are parsed
-- and the templates they instantiate are compiled once per group instead of once per source.
--
-- WHY NOT BY DEFAULT. A unity group changes what a translation unit IS: a `static` helper or an
-- anonymous-namespace type in one source becomes visible to -- and can collide with -- the next source
-- in the same group, and a source that forgets an #include can compile only because an earlier member
-- of its group included it. A developer's build (macOS gmake, a Visual Studio build on Windows) must
-- keep reporting the code as it is written, one source per translation unit, so the switch exists
-- only as an explicit option, and the one caller that passes it is .github/workflows/ci.yml (through
-- scripts\Windows\Setup.bat --unity).
--
-- WHY VISUAL STUDIO ONLY. premake 5.0-beta8 emits unity support for the vs20xx actions and for
-- nothing else; with `gmake` the option would be silently accepted and change nothing, which is the
-- "empty successful answer" this repository refuses. So any other action with --unity is an error.

newoption {
    trigger     = "unity",
    description = "Visual Studio only: compile Common, Desert and Editor as MSBuild unity files (CI speed; see BuildScripts/UnityBuild.lua)",
}

DesertUnity = {}

-- Sources per unity file. MSBuild's own default has no upper bound, and one huge unity file per project
-- would undo the only intra-project parallelism the CI build has (one cl.exe per source through the
-- MultiToolTask, see BuildScripts/MSBuild/Ccache.props): Desert (~340 sources) would compile on one
-- core. 12 keeps Desert at ~30 unity files and Editor at ~13 -- enough to fill the runner's four cores
-- -- while still sharing each group's header parse twelve ways.
DesertUnity.MaxFilesInUnityFile = 12

-- THE OPT-OUT LIST. Every entry names the file(s), and why that source must stay its own translation
-- unit. A pattern is a premake `filter "files:..."` pattern and starts with a bare `**` on purpose:
-- `**/ThirdParty/**` matched the engine's vendored sources (listed from the workspace root) but NOT
-- Editor's `ThirdParty/ImGuizmo/ImGuizmo.cpp` (listed relative to Editor/) -- measured in the
-- generated Editor.vcxproj, which carried no <IncludeInUnityFile> until the slash went.
DesertUnity.OptOut = {
    {
        pattern = "**ThirdParty/**",
        why = "vendored single-file implementations (stb_image, stb_truetype, vk_mem_alloc, miniaudio, "
           .. "pl_mpeg, ImGuizmo) define their implementation macros and file-scope helpers for ONE "
           .. "translation unit, and are compiled with warnings off per file",
    },
    {
        pattern = "**lightweightvk/**",
        why = "vendored LightweightVK sources (the second vendored tree, see BuildScripts/Workspace.lua)",
    },

    -- OUR OWN SOURCES THAT DO NOT SURVIVE A SHARED TRANSLATION UNIT, found by grouping each project's
    -- sources as the generated .vcxproj orders them (12 per group, at offsets 0 and 6) and compiling
    -- every group with clang -fsyntax-only. Two shapes: the same internal-linkage name defined in two
    -- sources (a `static`/anonymous-namespace constant or helper -- `kGroupSize`, `FloorDiv`, ...),
    -- and a `using namespace Desert::Editor` in one source that makes `Core::X` in the next resolve to
    -- Desert::Editor::Core. Each entry is the source the compiler reported; renaming the helper or
    -- qualifying the name takes the file off this list. MSVC-only clashes (windows.h) are not covered:
    -- the first Windows CI run with --unity is the proof.
    { pattern = "**Source/Common/Utilities/ContentManifest.cpp", why = "clang unity check: redefinition of 'kMagic'" },
    { pattern = "**Source/Engine/Animation/Rig/ControlShape.cpp", why = "clang unity check: redefinition of 'Finite'" },
    { pattern = "**Source/Engine/Assets/CloudNoiseVolume.cpp", why = "clang unity check: redefinition of 'kFormatRgba8'" },
    { pattern = "**Source/Engine/Assets/CloudNoiseVolumeSheet.cpp", why = "clang unity check: redefinition of 'kBytesPerVoxel'" },
    { pattern = "**Source/Engine/Assets/Serialization/TextureBinary.cpp", why = "clang unity check: redefinition of 'kByteOrderTag'" },
    { pattern = "**Source/Engine/Geometry/EditMeshAttributes.cpp", why = "clang unity check: redefinition of 'Next'" },
    { pattern = "**Source/Engine/Geometry/EditMeshConversion.cpp", why = "clang unity check: no viable conversion from 'EditMesh' to 'const FDynamicMesh3'" },
    { pattern = "**Source/Engine/Graphic/Systems/Scene/Fog/HeightFogRenderer.cpp", why = "clang unity check: redefinition of 'BytesToMiB'" },
    { pattern = "**Source/Engine/Graphic/Systems/Scene/PostProcessing/AutoExposureRenderer.cpp", why = "clang unity check: non-constexpr declaration of 'GroupCount' follows constexpr declaration" },
    { pattern = "**Source/Engine/Graphic/Systems/Scene/PostProcessing/BloomRenderer.cpp", why = "clang unity check: redefinition of 'kGroupSize'" },
    { pattern = "**Source/Engine/Graphic/Systems/Scene/PostProcessing/LensFlareRenderer.cpp", why = "clang unity check: redefinition of 'kGroupSize'" },
    { pattern = "**Source/Engine/Graphic/Systems/Scene/PostProcessing/LightShaftRenderer.cpp", why = "clang unity check: redefinition of 'kGroupSize'" },
    { pattern = "**Source/Engine/UI/UICanvasRenderer2D.cpp", why = "clang unity check: redefinition of 'CanvasFit'" },
    { pattern = "**Source/Engine/World/Landscape/LandscapeEditCache.cpp", why = "clang unity check: redefinition of 'FloorDiv'" },
    { pattern = "**Source/Editor/Panels/Clouds/CloudLayoutPanel.cpp", why = "clang unity check: no member named 'Formats' in namespace 'Desert::Editor::Core'; did you mean '::Desert::Core::Formats" },
    { pattern = "**Source/Editor/Panels/Clouds/CloudModellingVolumePanel.cpp", why = "clang unity check: redefinition of 'SubjectTitle'" },
    { pattern = "**Source/Editor/Panels/Clouds/CloudNoiseVolumePanel.cpp", why = "clang unity check: no member named 'Formats' in namespace 'Desert::Editor::Core'; did you mean '::Desert::Core::Formats" },
    { pattern = "**Source/Editor/Panels/FileExplorer/FileExplorerPanel.cpp", why = "clang unity check: invalid operands to binary expression ('ImVec2' and 'ImVec2')" },
    { pattern = "**Source/Editor/Panels/Photogrammetry/PhotogrammetryPanel.cpp", why = "clang unity check: no member named 'Formats' in namespace 'Desert::Editor::Core'; did you mean '::Desert::Core::Formats" },
    { pattern = "**Source/Editor/Panels/SceneHierarchy/SceneHierarchyPanel.cpp", why = "clang unity check: redefinition of 'AddCategory'" },
    { pattern = "**Source/Editor/Panels/SceneProperties/ComponentEditor.cpp", why = "clang unity check: redefinition of 'ContainsCI'" },
    { pattern = "**Source/Editor/Panels/SceneSettings/SceneSettingsPanel.cpp", why = "clang unity check: no type named 'TonemapOperator' in namespace 'Desert::Editor::Core'; did you mean '::Desert::Core::T" },
    { pattern = "**Source/Editor/Panels/TextureViewer/TextureViewerDocument.cpp", why = "clang unity check: redefinition of 'SubjectTitle'" },
    { pattern = "**Source/Editor/Panels/ViewportPanel/Tools/ElementSelectTool.cpp", why = "clang unity check: redefinition of 'CentreRay'" },
    { pattern = "**Source/Editor/Panels/ViewportPanel/Tools/LandscapeSculptTool.cpp", why = "clang unity check: redefinition of 'kTraceDistanceCm'" },
    { pattern = "**Source/Editor/Panels/WorldPartition/WorldPartitionMap.cpp", why = "clang unity check: no member named 'Rules' in namespace 'Desert::Editor::Core'; did you mean '::Desert::Core::Rules'?" },
    { pattern = "**Source/Editor/RenderSystems/Passes/EditorUIPass.cpp", why = "clang unity check: use of undeclared identifier 'input'" },
    { pattern = "**Source/Editor/Widgets/PreviewViewport.cpp", why = "clang unity check: redefinition of 'kDomeFov'" },
    { pattern = "**Source/Editor/Widgets/ThumbnailCache.cpp", why = "clang unity check: no member named 'Formats' in namespace 'Desert::Editor::Core'; did you mean '::Desert::Core::Formats" },
    { pattern = "**Source/Editor/Widgets/ThumbnailSubject.cpp", why = "clang unity check: no member named 'Formats' in namespace 'Desert::Editor::Core'; did you mean '::Desert::Core::Formats" },
    { pattern = "**Source/EditorLayer.cpp", why = "clang unity check: no member named 'ImGuiLayer' in namespace 'ImGui'; did you mean '::Desert::ImGui::ImGuiLayer'?" },
    { pattern = "**Source/Sandbox.cpp", why = "clang unity check: no viable conversion from 'unique_ptr<Desert::Editor::EditorLayer>' to 'unique_ptr<Common::Layer>'" },
}

-- Per-file opt-out. A boolean field of our own, because premake 5.0-beta8 has no spelling for MSBuild's
-- <IncludeInUnityFile> metadata; it is emitted by the override below and nowhere else.
premake.api.register {
    name  = "includeinunityfile",
    scope = "config",
    kind  = "boolean",
}

-- Called from inside a `project` block. Does nothing without --unity, so a project script reads the
-- same with and without it.
function DesertUnity.EnableForProject()
    if not _OPTIONS["unity"] then
        return
    end
    enableunitybuild "On"
    for _, entry in ipairs(DesertUnity.OptOut) do
        filter { "files:" .. entry.pattern }
            includeinunityfile(false)
    end
    filter {}
end

if _OPTIONS["unity"] then
    if not (_ACTION and _ACTION:startswith("vs")) then
        error("--unity is a Visual Studio (MSBuild) switch and does nothing for '" .. tostring(_ACTION)
            .. "'; see BuildScripts/UnityBuild.lua", 0)
    end

    -- The Visual Studio exporter is a module premake loads on demand; require it before overriding it.
    require("vstudio")
    local vc2010 = premake.vstudio.vc2010

    -- <MaxFilesInUnityFile> on the project's ClCompile item definition, only where unity is on.
    premake.override(vc2010.elements, "clCompile", function(base, cfg)
        local calls = base(cfg)
        table.insert(calls, function(c)
            if c.enableunitybuild == "On" then
                vc2010.element("MaxFilesInUnityFile", nil, tostring(DesertUnity.MaxFilesInUnityFile))
            end
        end)
        return calls
    end)

    -- <IncludeInUnityFile>false</IncludeInUnityFile> on each opted-out source.
    premake.override(vc2010, "fileConfigFunction", function(base, fcfg, condition)
        local calls = base(fcfg, condition)
        table.insert(calls, function(filecfg, cond)
            if filecfg and filecfg.includeinunityfile == false then
                vc2010.element("IncludeInUnityFile", cond, "false")
            end
        end)
        return calls
    end)
end
