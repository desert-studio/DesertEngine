#define IMGUI_DEFINE_MATH_OPERATORS

#include <Engine/Assets/ContentRegistry.hpp>

#include <Common/Core/AssetPathIndex.hpp>
#include <Common/Utilities/ContentScanLedger.hpp>
#include <Engine/Graphic/MemoryReadout.hpp>
#include <Engine/Graphic/ResourceLedger.hpp>
#include <Engine/Graphic/DrawCounters.hpp>
#include <Engine/Assets/SyncLoadLedger.hpp>
#include "EditorLayer.hpp"

#include <functional>

#include <Editor/Widgets/ThumbnailService.hpp>
#include <Common/Core/Core.hpp>
#include <Common/Core/Profiler.hpp>
#include <Editor/Import/MeshDnD.hpp>

// 1. Engine Core
#include <Engine/Core/Scene.hpp>
#include <Engine/ECS/Entity.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/Localization/LocalizationService.hpp>
#include <Engine/ECS/EntityLock.hpp>
#include <Engine/Geometry/PrimitiveType.hpp>
#include <Common/Core/Units.hpp>
#include <Engine/Geometry/DynamicMesh.hpp>
#include <Engine/Geometry/ProceduralCharacterFactory.hpp>
#include <Engine/Animation/Animator.hpp>
#include <Engine/Animation/ProceduralCharacterAnimations.hpp>
#include <Engine/Animation/Rig/ControlHierarchy.hpp>
#include <Engine/Animation/Rig/ControlRigStage.hpp>
#include <Engine/Assets/AssetEviction.hpp>
#include <Engine/Assets/Mesh/AnimationAsset.hpp>
#include <Engine/Scripting/ScriptEngine.hpp>
#include <Engine/Core/Serialize/SceneSerializer.hpp>
#include <Engine/Core/WorldStreamer.hpp>
#include <Engine/Core/Serialize/SceneFormat.hpp>
#include "Editor/Core/CommandLine.hpp"
#include "Editor/Core/Control/ControlChannelOptions.hpp"
#include "Editor/Core/Control/ControlDispatch.hpp" // resolving a request to a palette entry
#include "Editor/Core/CrashRecovery.hpp"

// The device-lost latch, read in OnDetach: a shutdown caused by a lost GPU must save the user's work
// before it goes, and must not report itself as a clean exit.
#include <Engine/Graphic/DeviceLost.hpp>
#include "Editor/Core/LayoutManager.hpp"
#include "Editor/Core/PanelRequests.hpp"
#include "Editor/Core/SceneOpenRequest.hpp"
#include "Editor/Core/SceneSaveRules.hpp"
#include "Editor/Core/ShotOptions.hpp"
#include "Editor/Core/DemoMaterials.hpp"
#include "Editor/Core/MaterialAssetUtils.hpp"
#include <Engine/Assets/Prefab/PrefabAsset.hpp>
#include <Common/Utilities/FileSystem.hpp>

// 2. Editor Base & Infrastructure
#include "Editor/Core/EditorResources.hpp"
#include "Editor/Core/ThemeManager.hpp"
#include "Editor/Core/GizmoState.hpp"
#include "Editor/Core/NumberFormat.hpp"
#include "Editor/Core/MeshResolve.hpp"            // the toolbar/status triangle census
#include "Editor/Core/Selection/ViewportMode.hpp" // the editor-mode rail
#include <Engine/Geometry/MeshStats.hpp>
#include "Editor/Core/CommandHistory.hpp"
#include "Editor/Core/Commands/LandscapeLayerCommands.hpp"
#include "Editor/Core/Commands/SceneCommands.hpp"
#include <Engine/ECS/LandscapeEditTarget.hpp>
#include <Engine/ECS/LandscapeRootOf.hpp>
#include "Editor/Core/EditorPreferences.hpp"
#include "Editor/Packaging/GamePackager.hpp"
#include "Editor/Core/ProjectContext.hpp"

#include <Engine/Graphic/API/Vulkan/VulkanSwapChain.hpp> // reading the PRESENTED frame back (shot.window)
#include <Engine/Graphic/Image.hpp> // Image2D::ReadPixelsRGBA8 (debug frame dump)
#include <Engine/Core/Input.hpp>
#include <Common/Core/KeyCodes.hpp>
#include <Common/Core/Version.hpp>
#include <Common/Settings/MachineSettings.hpp>
#include <stb_image/stb_image_write.h>
#include "Editor/Core/ImGuiUtilities.hpp"
#include <ImGui/imgui_internal.h>

#include <array>
#include <ImGuizmo.h>
#include "Editor/Import/ImportManager.hpp"
#include "Editor/Splash/SplashImage.hpp"
#include "Editor/Builtin/BuiltinMeshRegistry.hpp"

// 3. Editor Panels
#include "Editor/Panels/SceneHierarchy/SceneHierarchyPanel.hpp"
#include "Editor/Panels/SceneProperties/ScenePropertiesPanel.hpp"
#include "Editor/Panels/Debug/ShaderLibraryPanel.hpp"
#include "Editor/Panels/Debug/UIDebuggerPanel.hpp"
#include "Editor/Panels/FileExplorer/FileExplorerPanel.hpp"
#include "Editor/Panels/ViewportPanel/ViewportPanel.hpp"
#include "Editor/Panels/SceneSettings/SceneSettingsPanel.hpp"
#include "Editor/Panels/Landscape/LandscapePanel.hpp"
#include "Editor/Panels/Modeling/ModelingPanel.hpp"
#include "Editor/Panels/Logs/LogsPanel.hpp"
#include "Editor/Panels/Collections/CollectionsPanel.hpp"
#include "Editor/Panels/NodeGraph/NodeGraphPanel.hpp"
#include "Editor/Panels/NodeGraph/ShaderGraphDocumentOpen.hpp"
#include "Editor/Panels/MaterialEditor/MaterialEditorPanel.hpp"
#include "Editor/Panels/MaterialEditor/MaterialDocumentOpen.hpp"
#include "Editor/Panels/Animation/AnimGraphPanel.hpp"
#include "Editor/Panels/Photogrammetry/PhotogrammetryPanel.hpp"
#include "Editor/Panels/Particles/ParticleEditorPanel.hpp"
#include "Editor/Panels/UI/UIEditorPanel.hpp"
#include "Editor/Panels/AssetReferences/AssetReferencesPanel.hpp"
#include "Editor/Panels/LuaConsole/LuaConsolePanel.hpp"
#include "Editor/Panels/Sequencer/SequencerPanel.hpp"
#include "Editor/Panels/Build/BuildSettingsPanel.hpp"
#include "Editor/Panels/History/HistoryPanel.hpp"
#include "Editor/Panels/Localization/LocalizationPanel.hpp"
#include "Editor/Panels/Validation/SceneValidationPanel.hpp"
#include "Editor/Panels/Clouds/CloudModellingVolumePanel.hpp"
#include "Editor/Panels/Clouds/CloudDocumentOpen.hpp"
#include "Editor/Panels/Clouds/CloudLayoutPanel.hpp"
#include "Editor/Panels/Clouds/CloudNoiseVolumePanel.hpp"
#include "Editor/Panels/Clouds/CloudTypePanel.hpp"
#include "Editor/Panels/Clouds/CloudsPanel.hpp"
#include "Editor/Panels/Animation/AnimLayersPanel.hpp"
#include "Editor/Panels/Animation/ControlRigPanel.hpp"
#include "Editor/Core/Selection/AuthoringContext.hpp"
#include "Editor/Core/ToastManager.hpp"
#include "Editor/Core/OpenableAssets.hpp"
#include "Editor/Core/ViewportCameraProperties.hpp"
#include "Editor/Core/SubjectEditorRegistry.hpp"
#include "Editor/Core/ControlNudgeRequest.hpp"
#include "Editor/Core/SubjectOpenRequest.hpp"

// 4. Misc
#include <glm/gtx/matrix_decompose.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <Engine/Core/SceneRenderCollectors.hpp>
#include <Engine/ECS/System/MeshECSSystem.hpp>
#include <Engine/ECS/System/TextECSSystem.hpp>
#include <Engine/ECS/System/SkyboxECSSystem.hpp>
#include <Engine/ECS/System/HeightFogECSSystem.hpp>
#include <Engine/ECS/System/VolumetricCloudECSSystem.hpp>
#include <Engine/ECS/System/TimeOfDayECSSystem.hpp>
#include <Engine/Graphic/Materials/DataDrivenMaterial.hpp>
#include <Editor/Core/Rigging/RigBuilder.hpp>
#include <Editor/Core/Selection/MeshElementSelection.hpp>
#include <Editor/Core/Selection/MeshSelectionOperations.hpp>
#include <Editor/Core/Selection/LandscapeSculptState.hpp>
#include <Editor/Core/Selection/ModelingState.hpp>
#include <Editor/Core/Selection/SelectionManager.hpp>
#include <Engine/ECS/System/PointLightSystem.hpp>
#include <Engine/ECS/System/SpotLightSystem.hpp>
#include <Engine/ECS/System/AnimationECSSystem.hpp>
#include <Engine/ECS/System/AttachmentSystem.hpp>
#include <Engine/ECS/System/PhysicsECSSystem.hpp>
#include <Engine/ECS/System/LocomotionSystem.hpp>
#include <Engine/ECS/System/ScriptSystem.hpp>
#include <Engine/ECS/System/AudioECSSystem.hpp>

#include <algorithm> // std::sort / std::transform (scene list)
#include <span>      // the View menu's groups, declared as data rather than as control flow
#include <cctype>    // std::tolower (scene filter)
#include <chrono>    // per-stage startup timing (see the staged boot in OnUpdate)

namespace Desert::Editor
{
    // THE MENU BAR'S OWN MENUS, named once. Read by DrawMenuBar, which opens whichever one is held, and
    // by BuildPaletteCommands, which offers exactly these as commands. Two readers of one list, so the
    // palette cannot offer a menu the bar does not draw — the shape a hand-copied second list always ends
    // up in.
    static constexpr const char* kMenuBarMenus[] = { "File",   "Edit",     "View", "Window",
                                                     "Scenes", "Graphics", "About" };

    // THE SNAP STEPS A PERSON ACTUALLY USES, named once for the same reason the menus above are. Read by
    // DrawSnapControl, which draws them as the magnet popup's list, and by BuildPaletteCommands, which
    // offers exactly these as commands — so the palette cannot offer a step the toolbar does not, which
    // is the shape a hand-copied second list always ends up in.
    //
    // Translation in CENTIMETRES because 1 world unit IS 1 cm here, so the label and the value are the
    // same number and nothing has to be converted in anyone's head.
    static constexpr float kGridSteps[]  = { 1.0f, 5.0f, 10.0f, 25.0f, 50.0f, 100.0f, 500.0f };
    static constexpr float kAngleSteps[] = { 1.0f, 5.0f, 10.0f, 15.0f, 30.0f, 45.0f, 90.0f };

    // "Unsaved changes" marker: the CommandHistory revision at the last save/load. Compared against the
    // current revision for the status-bar dirty dot; reset wherever the scene is (re)loaded or saved.
    static uint64_t s_SavedRevision = 0;

    static bool s_ShowPreferences = false; // Edit -> Preferences... window

    // Icon shown before a panel's tab/title + its View-menu entry. Keyed by the panel's STABLE name
    // (GetName(), which is also the ImGui dock ID) so we never touch that ID.
    static const char* PanelIcon( const std::string& name )
    {
        if ( name == "Scene###scene" )
            return ICON_MDI_MONITOR;
        if ( name == "Scene Outliner" )
            return ICON_MDI_FILE_TREE;
        if ( name == "Details" )
            return ICON_MDI_TUNE;
        if ( name == "Assets" )
            return ICON_MDI_FOLDER_OUTLINE;
        if ( name == "Scene Settings" )
            return ICON_MDI_COG;
        if ( name == "Logs" )
            return ICON_MDI_TEXT_BOX_OUTLINE;
        if ( name == "History" )
            return ICON_MDI_HISTORY;
        if ( name == "Collections" )
            return ICON_MDI_SHAPE_OUTLINE;
        if ( name == "Anim Layers" )
            return ICON_MDI_ANIMATION;
        if ( name == "Model from Photos" )
            return ICON_MDI_CUBE_SCAN;
        // "Anim Graph", "Node Graph", "Particle Editor", "UI Editor" and "Sequencer" were here. They are
        // DOCUMENTS now,
        // and a document's icon comes from its registration rather than from a table keyed on a panel name
        // — this table can only ever match a tool's constant name, and a document is named after the thing
        // it edits. See SubjectEditorRegistry::Registration::Icon.
        if ( name == "Lua Console" )
            return ICON_MDI_CONSOLE;
        if ( name == "Build Settings" )
            return ICON_MDI_HAMMER_WRENCH;
        if ( name == "Asset References" )
            return ICON_MDI_LINK_VARIANT;
        if ( name == "Scene Validation" )
            return ICON_MDI_CLIPBOARD_CHECK_OUTLINE;
        if ( name == "Shader Library" )
            return ICON_MDI_PALETTE;
        return ICON_MDI_VIEW_DASHBOARD; // sensible default for any future panel
    }

    // Composes "<icon>  <label>###<stable id>". The visible part gets the icon; the trailing ###<name>
    // keeps the ImGui window ID EXACTLY panel->GetName(), so saved dock layouts and every GetName()==...
    // lookup keep working unchanged.
    // A tool panel that only makes sense for a particular selection or mode opens itself when that
    // context appears and steps aside when it goes away — so the tab strip carries what the current work
    // needs instead of every panel at once. Opening one BY HAND pins it (explicit intent wins) until the
    // user closes it again; see IPanel::IsContextual.
    void EditorLayer::UpdateContextualPanels()
    {
        for ( auto& panel : m_Panels )
        {
            // An EXPLICIT request always wins and applies to every panel, contextual or not: a button in
            // Details ("Anim Layers") asked for this panel BY NAME. It pins it, exactly like ticking it in
            // the View menu — the user asked, so nothing auto-closes it.
            //
            // BY NAME IS ALL A TOOL CAN BE ASKED FOR, and that is why the Anim Graph, the Particle Editor,
            // the UI Editor and the Sequencer no longer come through here: "show the one Sequencer window"
            // was the most their Details buttons could say, and the window then had to guess which rig it
            // was about from the selection. They ask for a SUBJECT now
            // (Core::SubjectOpenRequests::Request), which is a different wire because it carries what to
            // edit — see Editor/Core/SubjectOpenRequest.hpp.
            switch ( Core::PanelRequests::Consume( panel->GetName() ) )
            {
                case Core::PanelRequests::Action::Open:
                    panel->GetVisibility() = true;
                    panel->Pinned()        = true;
                    m_FocusPanel           = panel->GetName();
                    break;

                // A drawer button is a switch, not a summons: pressing it again puts the panel away.
                case Core::PanelRequests::Action::Toggle:
                    panel->GetVisibility() = !panel->GetVisibility();
                    panel->Pinned()        = panel->GetVisibility();
                    if ( panel->GetVisibility() )
                        m_FocusPanel = panel->GetName();
                    break;

                case Core::PanelRequests::Action::None:
                    break;
            }

            if ( !panel->IsContextual() )
                continue;

            const bool relevant = panel->IsRelevant();
            bool&      visible  = panel->GetVisibility();

            // Pinning is set ONLY where the user actually asks for the panel (View menu / command
            // palette). Inferring it from "visible but not relevant" also fired on the very first frame
            // for a panel that merely starts visible, pinning it open forever.
            if ( relevant && !visible && !panel->Pinned() )
            {
                visible      = true;
                m_FocusPanel = panel->GetName(); // bring it forward in whatever dock it lives
            }
            else if ( !relevant && visible && !panel->Pinned() )
            {
                visible = false;
            }
            else if ( !visible )
            {
                panel->Pinned() = false; // closed by hand -> stop pinning it open
            }
        }
    }

    static std::string PanelDisplayTitle( const std::string& name )
    {
        std::string label = name;
        if ( const auto pos = label.find( "###" ); pos != std::string::npos )
            label.erase( pos ); // visible part only (drop any existing ###id)
        return std::string( PanelIcon( name ) ) + "  " + label + "###" + name;
    }

    // THE DOCUMENT WELL'S OWN WINDOW (layout option B.1). A permanent occupant of the document dock node,
    // for two reasons that are both structural rather than decorative: an empty dock node is not drawn at
    // all, so without it the reserved area would be invisible for exactly as long as it was empty; and it is
    // the only stable window name in that node, which is how DrawDocumentWell recovers the node's runtime id
    // in a session that did not build the layout.
    static constexpr const char* kDocumentWellWindow =
         ICON_MDI_FILE_DOCUMENT_MULTIPLE_OUTLINE "  Documents###documentwell";

    // WHAT A DOCUMENT NOBODY CLAIMS LOOKS LIKE. The registry has no opinion about an unregistered subject
    // type and must not invent one (SubjectEditorRegistry::Icon takes the fallback as an argument for
    // exactly that reason), so the editor's answer lives here, once, rather than at each of the four places
    // that draw a document's glyph.
    static constexpr const char* kUnknownDocumentIcon = ICON_MDI_FILE_DOCUMENT_OUTLINE;

    // THE ICON SWITCH USED TO BE HERE, keyed on Assets::AssetTypeID, and it is gone rather than extended.
    //
    // It was one of the two hand-written tables a new kind of document had to be entered in — the other
    // being AssetTypeName for the text — neither of which is where the document is registered. That is
    // three edits in three files for one new kind, and the two that are not the registration are the ones
    // that get forgotten: the table's `default:` then quietly gave the new kind the generic page glyph and
    // nothing anywhere said so. The icon is now part of the registration
    // (SubjectEditorRegistry::Registration::Icon), so a kind that exists HAS one, by construction. It also
    // had no answer at all for a component subject, whose facet is not an AssetTypeID.

    // The window title a document is drawn with: its type's icon, its subject's name, and the "###doc<...>"
    // identity DocumentTitle already baked into GetName(). NOT PanelDisplayTitle, which would look the icon
    // up by a name that is an asset's and give every document the same fallback.
    std::string EditorLayer::DocumentDisplayTitle( const ISubjectDocument& document ) const
    {
        return std::string( m_SubjectEditors.Icon( document.Subject(), kUnknownDocumentIcon ) ) + "  " +
               DocumentDisplayName( document.GetName() ) + "###" + document.GetName();
    }

    // Cognitive complexity 27 against a threshold of 19, PRE-EXISTING and reported for any edit inside
    // this constructor (Г26 added the autosave-migration call below). Named as debt, not fixed here.
    // NOLINTNEXTLINE(readability-function-cognitive-complexity)
    EditorLayer::EditorLayer( const Engine::Application* application, const std::string& layerName,
                              std::unique_ptr<Splash::SplashScreen> splash )
         : Common::Layer( layerName ), m_Application( application ), m_Splash( std::move( splash ) )

    {
        m_AssetManager = std::make_shared<Assets::AssetManager>();

        m_ImportManager = std::make_unique<ImportManager>();
        // Cook only what's missing/stale (skips the expensive Assimp re-parse on every launch). Collections
        // hold packs (a character + its animation FBXs), so they're cooked too — their outputs land under
        // Cooked/Meshes/Collections/... where the preloader discovers them (see CookPaths::CookedMesh).
        //
        // STAGED: this used to run inline here and froze the window for seconds before the first frame.
        // The stages now execute one-per-frame from OnUpdate, each announced on the splash.
        // NOTE: shaders are NOT staged — they load synchronously in OnAttach, because the render systems
        // (MeshECSSystem's default PBR materials) resolve their shaders in their constructors.
        m_StartupStages.push_back(
             { "Cooking meshes...",
               [this] { m_ImportManager->ImportAllFromDirectory( Common::Constants::Path::MESH_PATH ); } } );
        m_StartupStages.push_back(
             { "Cooking collections...", [this]
               { m_ImportManager->ImportAllFromDirectory( Common::Constants::Path::COLLECTIONS_PATH ); } } );
        // AND THE LOOSE TEXTURES, WHICH NOTHING COOKED. A texture under `Assets/Textures/` reached its
        // cooked form only as a mesh's dependency or through a drag-and-drop, so the one cooked texture
        // this repository then committed had no producer in any automatic path -- and a stale one (a container
        // version moved, a PNG re-exported) stayed stale until somebody dragged the file back in. The
        // freshness question costs one CRC-32C pass per source at 8.17 GB/s and answers "nothing
        // changed" without decoding anything; see TextureImporter.cpp for why it is bytes and not
        // mtimes.
        //
        // AND THE TEXTURES THAT LIVE BESIDE A MESH. `Assets/Meshes/*.png` cook to
        // `Cooked/Textures/Assets/Meshes/*.tex`, and they are written only when the MESH is re-imported --
        // which the boot scan skips whenever the `.stmesh` is newer than its source. So a container version
        // bump left four of them stranded at version 1 and every launch printed four load failures that no
        // automatic path could clear. Both directories are `LooseTextureRoots()`, the list the packager
        // cooks too. (This stage used to walk `Assets/Meshes/` twice; the second walk found everything
        // fresh.)
        m_StartupStages.push_back(
             { "Cooking textures...", [this] { (void)m_ImportManager->CookLooseTextures(); } } );
        m_StartupStages.push_back( { "Preloading meshes, textures and materials...",
                                     [this] { m_AssetPreloader->PreloadCookedAssetsAndMaterials(); } } );
        m_StartupStages.push_back(
             { "Preloading environments...", [this] { m_AssetPreloader->PreloadSkyboxes(); } } );
        m_StartupStages.push_back(
             { "Preloading cloud noise volumes...", [this] { m_AssetPreloader->PreloadCloudNoiseVolumes(); } } );
        // AFTER the volumes, always: a cloud type binds the volume it names the moment it is created, and
        // one created first would find nothing to bind and render with the default edge.
        m_StartupStages.push_back(
             { "Preloading cloud types...", [this] { m_AssetPreloader->PreloadCloudTypes(); } } );
        m_StartupStages.push_back(
             { "Preloading hero clouds...", [this] { m_AssetPreloader->PreloadCloudModellingVolumes(); } } );
        // THIS LINE WAS MISSING FROM THE DAY THE PAINTED LAYOUT SHIPPED, and its absence made the whole
        // feature dead: AssetPreloader::PreloadCloudLayouts existed, scanned Clouds/Layouts and registered
        // every `.dclayout` with the service — and nothing ever called it. Every scene binding a painting
        // logged "referenced but not registered" and rendered its sky procedurally. Order-free, like the
        // hero clouds above: a layout names nothing and is named only by a material.
        m_StartupStages.push_back(
             { "Preloading painted layouts...", [this] { m_AssetPreloader->PreloadCloudLayouts(); } } );
        // THE SAME LINE THE PAINTED LAYOUT SPENT ITS WHOLE LIFE WITHOUT — added WITH the feature this
        // time, and for exactly the failure the comment above records: a scene naming a theme the scan
        // never ran would log "referenced but not registered" and draw every element's own colours, which
        // looks precisely like a theme system that does not work. Order-free: a theme names only font
        // paths, which FontService registers on demand, and nothing else names a theme.
        m_StartupStages.push_back(
             { "Preloading UI themes...", [this] { m_AssetPreloader->PreloadUIThemes(); } } );
        // CALLED, and that is the point of the line existing (A12). The stage above carries the note about
        // PreloadCloudLayouts having been a scan nobody ran; a rig library nobody scans is the same defect
        // with a different extension — the entity's rig slot would be empty in every project that has
        // rigs, and only a scene that already named one would ever load it.
        m_StartupStages.push_back(
             { "Preloading control rigs...", [this] { m_AssetPreloader->PreloadControlRigs(); } } );
        // And the graphs, for the same reason one line up: an entity's graph slot has to be able to offer
        // the project's `.danimgraph` files, and it does that by asking the manager for every one of them.
        m_StartupStages.push_back(
             { "Preloading anim graphs...", [this] { m_AssetPreloader->PreloadAnimGraphs(); } } );
        // AND THE RETARGETS, WHICH ARE NOT ORDER-FREE the way the two above are. A `.retarget` names its
        // source rig by signature and binds it while loading, so it has to run after the cooked scan that
        // registers the project's `.skeleton` files — which is the very first content stage. Placed here,
        // beside the other two animation preloads, that ordering holds by construction.
        m_StartupStages.push_back(
             { "Preloading retargets...", [this] { m_AssetPreloader->PreloadRetargets(); } } );
        // Order-free, and early among the optional stages on purpose: a missing translation shows up on
        // the very first frame drawn, and its log line is far easier to read before the rest of the
        // content's lines arrive.
        m_StartupStages.push_back(
             { "Preloading string tables...", [this] { m_AssetPreloader->PreloadStringTables(); } } );

        // WHAT THIS MACHINE CAN AFFORD — a different file from editor.json and deliberately so (К3).
        // editor.json is one person's copy of the EDITOR and the packaged game never opens it, while every
        // value in machine.json is read by SceneRenderer, which the packaged game runs; the schema is one
        // and the PLACE is the parameter, so a shipped build reads the same fields out of the player's own
        // directory (Runtime/Source/Main.cpp).
        //
        // FIRST, before any SceneRenderer is CONSTRUCTED, and for two independent reasons. MSAA is baked
        // into the pipelines at SceneRenderer::Init, so a later load would apply one start behind; and a
        // renderer initialises its own copy of these values from this store, so one built before the load
        // would hold the schema defaults and push two of them into global sampler state.
        Common::Settings::MachineSettings::Load( std::filesystem::path( ProjectContext::ConfigDirectory() ) /
                                                 "machine.json" );

        // BEFORE the preloader, which now takes it and fills it at the tail of the scan that finds the
        // clips. The editor used to fill it itself, in OnAttach — a whole startup stage BEFORE the scan
        // ran — so with two `.anim` files on disk it reported the four procedural clips and nothing else.
        m_AnimationLibrary = std::make_unique<Animation::AnimationLibrary>( m_AssetManager.get() );
        m_AssetPreloader   = std::make_unique<Assets::AssetPreloader>( m_AssetManager, *m_AnimationLibrary );
        m_SceneRenderer    = std::make_unique<Graphic::SceneRenderer>();
        m_MainScene        = std::make_shared<Desert::Core::Scene>( "New Scene", m_SceneRenderer.get() );
        m_PrimaryScene     = m_MainScene; // the always-present document #-1 (see SetActiveScene)

        // The scene/asset-manager the undoable structural commands operate on (the scene OBJECT is reused
        // across loads — Clear() + deserialize — so this stays valid; the history itself is cleared on
        // load/Play/Stop instead).
        Commands::SetContext( m_MainScene.get(), m_AssetManager.get() );

        LOG_INFO( "[Editor] Desert Engine {} ({} branch)", Common::Version::Full(), Common::Version::Branch() );

        // User prefs (snap steps, camera speed, autosave) from ~/.desertengine/editor.json. Snap values
        // apply immediately; the camera speed is applied on the first frame (the camera exists by then).
        EditorPreferences::Load();

        // Sandbox one-time bake of the Cornell showcase to a loadable scene (File -> Open ->
        // CornellDemo.desce). Runs BEFORE the default-scene handling below and clears itself, so it
        // starts from and ends on an empty scene — it never fights the Starter scene the sandbox's
        // own DefaultScene generates next.
        if ( ProjectContext::HasProject() && ProjectContext::Current().Name == "Desert Sandbox" )
        {
            const auto demoPath =
                 Common::Constants::Path::SCENE_PATH /
                 ( "CornellDemo" + std::string( Common::Constants::Extensions::SCENE_EXTENSION ) );
            std::error_code ec;
            if ( !std::filesystem::exists( demoPath, ec ) )
            {
                m_MainScene->Clear();
                BuildCornellShowcase();
                // The success line is inside the branch: this bake exists so the demo can be OPENED
                // later, and announcing a file that is not there sends the next reader looking for a
                // corrupt scene instead of a failed write.
                if ( SaveSceneTo( demoPath.generic_string() ) )
                {
                    LOG_INFO( "[Editor] Baked the Cornell showcase -> {}", demoPath.string() );
                }
                else
                {
                    LOG_ERROR( "[Editor] The Cornell showcase was NOT baked to {} — the sandbox has no "
                               "demo scene to open (see the write failure above).",
                               demoPath.string() );
                }
                m_MainScene->Clear();
            }
        }

        // Launched with --project (Project Hub): adopt the project's name and queue its default scene
        // (loaded through the normal deferred path on the first frame, when the renderer is ready).
        // A DefaultScene that does not exist yet (a FRESH project, sandbox included) is GENERATED: the
        // Starter playground built once and saved into the project — startup content is data, not code.
        // Screenshot mode names its own scene; it is the whole point of the flag.
        if ( const auto& shot = ShotOptions::Get(); !shot.Scene.empty() )
        {
            // In CAPTURE mode a `--scene` that is not there is fatal, not something to carry on past.
            // The scene loader already logs and leaves the current scene standing, which is right for an
            // editor and wrong for a capture: the run would go on to write PNGs named after the scene
            // that was asked for, holding the picture of a different one. That is worse than no evidence,
            // because it looks exactly like evidence. Interactive `--scene` keeps the old behaviour.
            //
            // The RULE itself lives in Editor/Core/CommandLine.hpp as a pure function taking the existence
            // as a parameter, so it is asserted by a test rather than only observable by launching the
            // editor at a path that is not there. This call site supplies the filesystem it cannot.
            const auto verdict = ValidateSceneForCapture( shot, std::filesystem::exists( shot.Scene ) );
            if ( !verdict.IsSuccess() )
            {
                LOG_ERROR( "[Shot] {} (looked from '{}')", verdict.GetError(),
                           std::filesystem::current_path().string() );
                // NOT std::exit(). The job system's workers are already running by the time this line is
                // reached, and exit() runs static destructors under them: nine threads threw
                // "recursive_mutex lock failed: Invalid argument" and the process aborted with 134. A
                // status of 134 says "the engine crashed", not "the scene you asked for is missing" — the
                // caller reading it learns the wrong thing. Ask for an ordered close with the real status;
                // Run() then draws no frames and teardown happens exactly as on a normal quit.
                const_cast<Engine::Application*>( m_Application )->Close( 2 );
            }
            else
            {
                LoadScene( Common::Filepath( shot.Scene ) );
            }
        }
        else if ( ProjectContext::HasProject() )
        {
            m_MainScene->SetSceneName( ProjectContext::Current().Name );
            if ( const auto scenePath = ProjectContext::DefaultScenePath(); !scenePath.empty() )
            {
                if ( std::filesystem::exists( scenePath ) )
                    LoadScene( scenePath );
                else
                {
                    BuildStarterScene();
                    if ( SaveSceneTo( scenePath ) )
                    {
                        // It was built AND written, so it is now an open file like any other — a later
                        // Ctrl+S has to go back to it rather than to a path derived from its name.
                        m_OpenScenePath = Common::Filepath( scenePath );
                        LOG_INFO( "[Editor] Generated the Starter scene -> {}", scenePath );
                    }
                    else
                    {
                        // The scene is BUILT and open — only the file is missing. Saying so is the
                        // difference between "your new project opens empty next time" being a mystery
                        // and being a known, fixable write failure the user can still Ctrl+S past.
                        LOG_ERROR( "[Editor] The Starter scene was built but NOT written to {} — this "
                                   "project will open empty next time unless it is saved.",
                                   scenePath );
                        Editor::ToastManager::Push( "The Starter scene could not be written — save it "
                                                    "before closing (see the log)",
                                                    Editor::ToastLevel::Error );
                    }
                }
            }
        }

        BuiltinMeshRegistry::Init( nullptr );

        // AUTOSAVES ARE RAISED TO THE CURRENT SCHEMA BEFORE ANYTHING ASKS TO OPEN ONE. They are the one
        // class of `.desce` no task can convert -- the directory is gitignored, so it is absent from the
        // worktree a schema step is written in and from the commit that converts the corpus -- and the
        // consequence was measured on the v17 -> v18 raise: five of the owner's recovery copies stopped
        // opening and had to be run through the migrator by hand. See CrashRecovery::MigrateAutosaves.
        if ( !CrashRecovery::MigrateAutosaves() )
        {
            Editor::ToastManager::Push( "An autosave could not be raised to the current scene format and "
                                        "will not open (see the log)",
                                        Editor::ToastLevel::Error );
        }

        // Crash recovery: if the previous session left its lock behind (unclean exit) and an autosave
        // exists, arm a prompt to reopen it. Then (re)arm the lock for THIS session; a clean shutdown
        // (OnDetach) removes it.
        if ( CrashRecovery::WasUncleanExit() )
        {
            m_RecoveryAutosave   = CrashRecovery::LatestAutosave();
            m_ShowRecoveryPrompt = !m_RecoveryAutosave.empty();
        }
        if ( !CrashRecovery::ArmSession() )
            Editor::ToastManager::Push( "Crash recovery is OFF for this session — the lock file could "
                                        "not be written (see the log)",
                                        Editor::ToastLevel::Error );

        // LoadScene( "Resources/Assets/Scene/HouseDemo.desce" );
    }

    EditorLayer::~EditorLayer() = default;

    [[nodiscard]] Common::BoolResultStr EditorLayer::OnAttach()
    {
        // THE CONTROL CHANNEL, IF ONE WAS ASKED FOR. Before anything else, so a client that started this
        // editor can connect and watch the boot rather than guessing how long to wait for the socket.
        //
        // A REFUSAL ENDS THE RUN. Carrying on unheard would be the worst of both: the client waits for a
        // socket that will never appear, and the editor it was meant to drive sits there being driven by
        // nobody. `--control-socket` is only ever passed by something that intends to connect.
        if ( const auto& channel = Control::ControlChannelOptions::Get(); channel.Requested() )
        {
            if ( const auto listening = m_ControlSocket.Listen( channel.SocketPath ); !listening )
                return Common::MakeFormattedError( "control channel: {}", listening.GetError() );
        }

        // THE WINDOW FRAME, IF THIS EDITOR OWNS IT. Asked of the window rather than assumed from the
        // ApplicationInfo that requested it: a fullscreen-over-the-taskbar window is frameless whatever was
        // asked for, and the window is the one that knows what actually happened (Window::IsDecorated).
        if ( const auto& window = m_Application->GetWindow(); window && !window->IsDecorated() )
        {
            m_WindowChrome.emplace(
                 *window,
                 // The same ordered close the control channel's `quit` takes: Run() leaves its loop, every
                 // layer is detached, the device goes idle. Two ways to end a session would drift.
                 [this]() { const_cast<Engine::Application*>( m_Application )->Close( 0 ); } );
        }

        // 1. Create ImGui Context first
        ::ImGui::CreateContext();

        // 2. Initialize Editor Resources (Adds fonts to the atlas)
        Editor::EditorResources::Initialize( "Resources/Fonts/materialdesignicons-webfont.ttf" );

        // 3. Initialize Engine ImGui Layer (Initializes backend and uploads fonts)
        m_ImGuiLayer = ImGui::ImGuiLayer::Create();
        if ( const auto attached = m_ImGuiLayer->OnAttach(); !attached.IsSuccess() )
            return Common::MakeFormattedError( "ImGui layer failed to attach: {}", attached.GetError() );

        ImGuiIO& io = ::ImGui::GetIO();
        (void)io;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard; // Enable Keyboard Controls
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;  // Enable Gamepad Controls
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;     // Enable Docking
        io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;   // Enable Multi-Viewport / Platform Windows

        // Setup ImGui style
        ThemeManager::SetDarkTheme();

        // When viewports are enabled we tweak WindowRounding/WindowBg so platform windows can look identical to
        // regular ones
        ImGuiStyle& style = ::ImGui::GetStyle();
        if ( io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable )
        {
            style.WindowRounding              = 0.0f;
            style.Colors[ImGuiCol_WindowBg].w = 1.0f;
        }

        // THE COOKED ASSET REGISTRY, BEFORE ANY PRELOAD — including the shader one on the next line,
        // which is the earliest scan this host runs. Every `Preload*` takes its candidates from the
        // registry since T2.4, so a preload that ran before the file was read would find nothing; and
        // the boot's cook stages call `ContentRegistry::NoteFile` as they write, which would be writing
        // into rows that `Load` was about to replace.
        //
        // A REFUSAL ENDS THE RUN, on the terms §1.4 sets: an editor that starts with a registry it
        // could not parse is an editor showing an empty Content Browser over a project full of files,
        // and "looks almost right" is the failure mode that costs the most to find.
        const auto registry = Assets::ContentRegistry::Load();
        if ( !registry )
            return Common::MakeFormattedError( "the cooked asset registry: {}", registry.GetError() );
        LOG_INFO( "[ContentRegistry] {} row(s), {} handle(s) bound before anything was loaded",
                  Assets::ContentRegistry::Get().Count(), registry.GetValue() );

        // Shaders must exist BEFORE the render systems below are constructed (their default materials
        // resolve shaders in the ctor). Meshes/skyboxes are staged instead. The longest single wait of the
        // start, and one call: the splash says what it is before it begins, and cannot say more during it.
        ReportSplashStep( "Compiling shaders...", kSplashShaderStep );
        m_AssetPreloader->PreloadShaders();

        BuildSceneSystems( *m_MainScene );

        // THE ANIMATION LIBRARY IS NOT FILLED HERE ANY MORE, and its absence is the fix rather than an
        // omission. A `FindAllByType<AnimationAsset>` loop and a `ProceduralCharacterAnimations::
        // RegisterClips` call used to stand on these lines, and both were wrong in the same way: they ran
        // in OnAttach, several startup stages BEFORE `PreloadCookedAssetsAndMaterials` scans `.anim` off
        // disk, so they filled the library out of a manager that had not been shown a single clip file —
        // measured at "4 clip(s) known" with three clips sitting in Cooked/Meshes. The runtime layer, which
        // has no OnAttach loop of its own, had no clips at all. Both are now `Animation::PopulateLibrary`,
        // called from the tail of that scan, and `Desert/Tests/Editor/AssetPreloadCensus` forbids either
        // host from growing its own copy again.

        // NOT INITIALISED WHEN A SCENE LOAD IS ALREADY QUEUED, and that condition is why the line moved
        // rather than why it is conditional. The constructor above has already called LoadScene() for
        // `--scene` or for the project's default scene, so by the time OnAttach gets here the empty "New
        // Scene" this would build a renderer for is a scene NOBODY WILL EVER SEE: OnUpdate returns early
        // for the whole of the staged startup load and draws no scene frame, and the first thing it does
        // when that finishes is LoadSceneInternal, whose own Init() ran second. Measured at ~1.4 s of every
        // Debug start (Г8) — pipelines and framebuffers built, waited on and destroyed.
        //
        // WHAT THIS SAVES CHANGED WITH Г11, AND THE LINE IS STILL RIGHT. Init() no longer rebuilds the
        // renderer on a second call, so the first one would no longer be THROWN AWAY — it would simply
        // happen earlier. What it would still be is a renderer built against an empty scene, before the
        // staged startup load has cooked and preloaded anything, on a frame nobody sees; deferring it to
        // the load that a person is actually waiting for is what keeps the two costs from being paid one
        // after the other in the same second.
        //
        // It is NOT a "run Init once" flag: re-running Init() is legal, and is what binds a newly loaded
        // scene to the renderer (SceneRenderer::Init — the renderer half is once, the scene half is every
        // time). Only this first, pre-empted one is skipped, and the deferred-load site in OnUpdate is what
        // guarantees the scene ends up initialised even if the load refuses the file.
        //
        // Propagated rather than reported: OnAttach owns a channel and Application::PushLayer now reads
        // it, and an editor whose main scene never initialised has no viewport to show anything in.
        if ( !m_SceneLoadRequested )
        {
            if ( const auto inited = m_MainScene->Init(); !inited.IsSuccess() )
                return Common::MakeFormattedError( "main scene failed to initialise: {}", inited.GetError() );
        }

        // EVERY TOOL ENTERS THROUGH PanelRegistry::Add / Adopt, and that is the whole of the guarantee that
        // the View menu lists tools only: the registry REFUSES an ISubjectDocument at compile time, so a
        // document cannot be here to be listed. See Editor/Core/PanelRegistry.hpp.
        m_Panels.Add<Editor::SceneHierarchyPanel>( m_MainScene, m_AssetManager );
        m_Panels.Add<Editor::ScenePropertiesPanel>( m_MainScene, m_AssetManager, m_AnimationLibrary.get() );
        m_Panels.Add<Editor::ShaderLibraryPanel>();
        {
            auto primaryViewport = std::make_unique<Editor::ViewportPanel>( m_MainScene, m_AssetManager.get() );
            // Focusing the main viewport rebinds the editor back to the primary scene.
            primaryViewport->SetOnActivate( [this] { SetActiveScene( kPrimarySceneViewId ); } );
            m_Panels.Adopt( std::move( primaryViewport ) );
        }
        {
            auto fileExplorer = std::make_unique<Editor::FileExplorerPanel>(
                 Common::Constants::Path::ASSETS_PATH, &m_SubjectEditors, m_AssetManager.get(), m_MainScene );
            m_FileExplorerPanel = fileExplorer.get();
            m_Panels.Adopt( std::move( fileExplorer ) );
        }
        m_Panels.Add<Editor::ModelingPanel>( m_MainScene );
        m_Panels.Add<Editor::LandscapePanel>( m_MainScene );
        m_Panels.Add<Editor::SceneSettingsPanel>( m_MainScene );
        m_Panels.Add<Editor::LogsPanel>();
        m_Panels.Add<Editor::CollectionsPanel>( m_AssetManager.get() );
        m_Panels.Add<Editor::HistoryPanel>();
        m_Panels.Add<Editor::SceneValidationPanel>( m_MainScene, m_AssetManager.get() );
        m_Panels.Add<Editor::LocalizationPanel>();
        m_Panels.Add<Editor::UIDebuggerPanel>( m_MainScene );
        // THE FOUR CLOUD PANELS ARE NOT CONSTRUCTED HERE ANY MORE. They were singletons in this list, each
        // reached from the View menu and bound to whatever file its own combo had last opened; they are now
        // asset DOCUMENTS, built on demand by the registry below. Dropping them from the list is what
        // removes them from the View menu, the command palette and `--open-panel <name>` at once — all three
        // are generic over m_Panels, so there was never a per-panel entry to delete. An asset is opened from
        // the asset, not from a menu (Docs/Clouds/DEV_CONTRACT.md §4).

        // THE NODE GRAPH IS NOT CONSTRUCTED HERE ANY MORE EITHER, and it was the last one: a `.dgraph` is
        // an asset now, so the window is a document over its handle and is built on demand by the registry
        // below. That is U7-2's refusal spent — see NodeGraphPanel.hpp for the four obstacles it named and
        // which of them turned out to be real.
        //
        // THE ANIM GRAPH, THE PARTICLE EDITOR, THE UI EDITOR AND THE SEQUENCER ARE NOT CONSTRUCTED HERE ANY
        // MORE, for the reason the four cloud panels above are not: they edit ONE thing, so they are
        // documents. The difference is what that one thing is — a component on an entity rather than a file
        // — which is what U7 made expressible (Editor/Core/EditorSubject.hpp). Dropping them from this list
        // removes them from the View menu, the command palette and `--open-panel` at once, because all three
        // are generic over m_Panels; they are reached from the component that holds them, in Details.
        m_Panels.Add<Editor::PhotogrammetryPanel>( m_MainScene, m_AssetManager.get() );
        m_Panels.Add<Editor::AssetReferencesPanel>( m_MainScene, m_AssetManager );
        m_Panels.Add<Editor::LuaConsolePanel>( m_MainScene.get(), m_AssetManager.get() );
        m_Panels.Add<Editor::AnimLayersPanel>( m_MainScene, m_AnimationLibrary.get() );
        m_Panels.Add<Editor::ControlRigPanel>( m_MainScene );
        m_Panels.Add<Editor::BuildSettingsPanel>();
        // THE CLOUDS WINDOW IS A TOOL, and it must be: it is a setting the user keeps (View ▸ Clouds), it
        // edits no subject of its own, and the compiler refuses a document here anyway (PanelRegistry).
        // What it DOES is show the documents that edit the six stages of the sky — asked of
        // m_OpenDocuments, which is the same container the document well reads, so both windows show the
        // same object and neither knows the other exists. See Editor/Panels/Clouds/CloudsPanel.hpp.
        m_Panels.Add<Editor::CloudsPanel>( m_MainScene, m_AssetManager, m_OpenDocuments );

        // ── WHICH EDITOR OPENS WHICH KIND OF SUBJECT ──────────────────────────────────────────────────
        //
        // Double-clicking a `.demat` in the browser opens ONE window bound to THAT material, and a second
        // material is a second window; the four cloud formats follow the same rule, and so — since U7 — do
        // the two kinds of subject that are not files at all. Adding the next kind is another block here
        // rather than another branch in FileExplorerPanel and another file-static inbox beside it. See
        // Editor/Core/SubjectEditorRegistry.hpp.
        //
        // THE NAME AND THE ICON ARE PART OF THE REGISTRATION. They used to be two hand-written tables in
        // this file keyed on Assets::AssetTypeID, so a new kind meant three edits and only one of them was
        // here; the two that were not are the ones that fell behind.
        using Registration = SubjectEditorRegistry::Registration;

        m_SubjectEditors.Register(
             AssetSubjectType( static_cast<uint32_t>( Assets::AssetTypeID::Material ) ),
             Registration{ "Material", ICON_MDI_PALETTE_SWATCH,
                           [this]( const SubjectId& subject ) -> std::unique_ptr<ISubjectDocument> {
                               return std::make_unique<Editor::MaterialEditorPanel>(
                                    Assets::AssetHandle( subject.Owner ), m_AssetManager );
                           },
                           [this]( const SubjectId& subject )
                           {
                               return m_AssetManager && m_AssetManager->FindMetadataByHandle(
                                                             Assets::AssetHandle( subject.Owner ) ) != nullptr;
                           } } );

        // THE FOUR CLOUD DOCUMENTS. Each takes the raw AssetManager pointer the panels already held, so the
        // move from singleton to document changed the panels' ownership of their subject and nothing about
        // how they reach their assets.
        m_SubjectEditors.Register(
             AssetSubjectType( static_cast<uint32_t>( Assets::AssetTypeID::CloudNoiseVolume ) ),
             Registration{ "CloudNoiseVolume", ICON_MDI_GRID,
                           [this]( const SubjectId& subject ) -> std::unique_ptr<ISubjectDocument>
                           {
                               return std::make_unique<Editor::CloudNoiseVolumePanel>(
                                    Assets::AssetHandle( subject.Owner ), m_AssetManager.get() );
                           },
                           [this]( const SubjectId& subject )
                           {
                               return m_AssetManager && m_AssetManager->FindMetadataByHandle(
                                                             Assets::AssetHandle( subject.Owner ) ) != nullptr;
                           } } );
        m_SubjectEditors.Register(
             AssetSubjectType( static_cast<uint32_t>( Assets::AssetTypeID::CloudType ) ),
             Registration{ "CloudType", ICON_MDI_WEATHER_CLOUDY,
                           [this]( const SubjectId& subject ) -> std::unique_ptr<ISubjectDocument> {
                               return std::make_unique<Editor::CloudTypePanel>(
                                    Assets::AssetHandle( subject.Owner ), m_AssetManager.get() );
                           },
                           [this]( const SubjectId& subject )
                           {
                               return m_AssetManager && m_AssetManager->FindMetadataByHandle(
                                                             Assets::AssetHandle( subject.Owner ) ) != nullptr;
                           } } );
        m_SubjectEditors.Register(
             AssetSubjectType( static_cast<uint32_t>( Assets::AssetTypeID::CloudModellingVolume ) ),
             Registration{ "CloudModellingVolume", ICON_MDI_CUBE_OUTLINE,
                           [this]( const SubjectId& subject ) -> std::unique_ptr<ISubjectDocument>
                           {
                               return std::make_unique<Editor::CloudModellingVolumePanel>(
                                    Assets::AssetHandle( subject.Owner ), m_AssetManager.get() );
                           },
                           [this]( const SubjectId& subject )
                           {
                               return m_AssetManager && m_AssetManager->FindMetadataByHandle(
                                                             Assets::AssetHandle( subject.Owner ) ) != nullptr;
                           } } );
        // The layout document also READS the active scene's cloud layer for its preview numbers — the scene
        // is an input, never a second subject, and SetScene keeps it following the focused viewport exactly
        // as the singleton did.
        m_SubjectEditors.Register(
             AssetSubjectType( static_cast<uint32_t>( Assets::AssetTypeID::CloudLayout ) ),
             Registration{ "CloudLayout", ICON_MDI_IMAGE_FILTER_HDR,
                           [this]( const SubjectId& subject ) -> std::unique_ptr<ISubjectDocument>
                           {
                               return std::make_unique<Editor::CloudLayoutPanel>(
                                    Assets::AssetHandle( subject.Owner ), m_MainScene, m_AssetManager.get() );
                           },
                           [this]( const SubjectId& subject )
                           {
                               return m_AssetManager && m_AssetManager->FindMetadataByHandle(
                                                             Assets::AssetHandle( subject.Owner ) ) != nullptr;
                           } } );

        // THE SHADER GRAPH. The seventh asset document and the last window in this editor to become one;
        // the display name is the graph's own `Name` (the file's stem when it has none), resolved here
        // because a document must not need the asset manager to know what it is called.
        m_SubjectEditors.Register(
             Editor::NodeGraphPanel::SubjectType(),
             Registration{ "ShaderGraph", ICON_MDI_GRAPH,
                           [this]( const SubjectId& subject ) -> std::unique_ptr<ISubjectDocument>
                           {
                               const Assets::AssetHandle handle( subject.Owner );
                               std::string               name = "Shader Graph";
                               if ( m_AssetManager )
                               {
                                   if ( const auto graph =
                                             m_AssetManager->FindByHandle<Assets::ShaderGraphAsset>( handle ) )
                                   {
                                       name = graph->GetDisplayName();
                                   }
                               }
                               return std::make_unique<Editor::NodeGraphPanel>( handle, name, m_AssetManager );
                           },
                           [this]( const SubjectId& subject )
                           {
                               return m_AssetManager && m_AssetManager->FindMetadataByHandle(
                                                             Assets::AssetHandle( subject.Owner ) ) != nullptr;
                           } } );

        // ── THE THREE DOCUMENTS WHOSE SUBJECT IS NOT A FILE ───────────────────────────────────────────
        //
        // This is what U7 bought. All three were singleton panels that drew "whatever entity is selected"
        // (or, for the UI editor, whichever canvas the registry listed first), and all three are now opened
        // FROM the component that holds their data, by a button in Details — which is the thing the owner
        // asked for and the thing the old asset-keyed seam could not express.
        //
        // THE FACTORY TAKES THE SCENE THAT IS ACTIVE AT THE MOMENT OF THE OPEN, and that is deliberate:
        // the subject is an entity UUID, and a UUID belongs to ONE registry. Captured by reference to the
        // member so a document opened from the second scene view binds to the second scene — and then
        // never follows the fanout again (AnimGraphPanel::SetScene is a documented no-op).
        //
        // The DISPLAY name is the entity's, resolved once here rather than by the document: the document
        // must not need a scene to know what it is called, and a name is a label while the subject is the
        // identity — renaming the entity does not open a second window.
        m_SubjectEditors.Register(
             Editor::AnimGraphPanel::SubjectType(),
             Registration{ Editor::AnimGraphPanel::kComponentTypeName, ICON_MDI_STATE_MACHINE,
                           [this]( const SubjectId& subject ) -> std::unique_ptr<ISubjectDocument>
                           {
                               return std::make_unique<Editor::AnimGraphPanel>(
                                    subject, SubjectEntityName( subject, "Anim Graph" ), m_MainScene,
                                    m_AnimationLibrary.get(), m_AssetManager.get() );
                           },
                           [this]( const SubjectId& subject )
                           { return EntityHasComponent<ECS::AnimationComponent>( subject.Owner ); } } );
        m_SubjectEditors.Register(
             Editor::ParticleEditorPanel::SubjectType(),
             Registration{ Editor::ParticleEditorPanel::kComponentTypeName, ICON_MDI_CREATION,
                           [this]( const SubjectId& subject ) -> std::unique_ptr<ISubjectDocument>
                           {
                               return std::make_unique<Editor::ParticleEditorPanel>(
                                    subject, SubjectEntityName( subject, "Particles" ), m_MainScene );
                           },
                           [this]( const SubjectId& subject )
                           { return EntityHasComponent<ECS::ParticleEmitterComponent>( subject.Owner ); } } );
        // THE UI CANVAS. Its window owns a Framebuffer and a Render2D rather than a SceneRenderer, so it
        // takes none of the six renderer slots and says so (UIEditorPanel::ClaimsRendererSlot) — a document
        // that renders is not automatically a document that costs a slot.
        m_SubjectEditors.Register(
             Editor::UIEditorPanel::SubjectType(),
             Registration{ Editor::UIEditorPanel::kComponentTypeName, ICON_MDI_VIEW_DASHBOARD,
                           [this]( const SubjectId& subject ) -> std::unique_ptr<ISubjectDocument> {
                               return std::make_unique<Editor::UIEditorPanel>(
                                    subject, SubjectEntityName( subject, "UI" ), m_MainScene );
                           },
                           [this]( const SubjectId& subject )
                           { return EntityHasComponent<ECS::UICanvasComponent>( subject.Owner ); } } );

        // THE TWO TIMELINES. One class, two subject types, and the argument for why they are two and not
        // one is written out at the top of SequencerPanel.hpp: the rig timeline keys BONE POSES and is
        // therefore about the SkinnedMeshComponent, while the anim graph next door keys states and
        // transitions and is about the AnimationComponent — two editors under one key is refused by this
        // registry by name, which is what made the question get answered.
        //
        // THE PRESENCE TEST ASKS FOR BOTH COMPONENTS, not just the one the subject is named after: a rig
        // with no AnimationComponent has no clip to pick and no animator to pose, so offering the entry
        // would open a window with nothing in it. It is the same predicate SequencerPanel::IsSubjectAlive
        // answers with, asked of the current scene rather than of the document's own — see
        // SubjectEditorRegistry::Registration::Exists for why those are two questions.
        m_SubjectEditors.Register(
             Editor::SequencerPanel::SkeletalSubjectType(),
             Registration{ Editor::SequencerPanel::kSkeletalComponentTypeName, ICON_MDI_CHART_TIMELINE,
                           [this]( const SubjectId& subject ) -> std::unique_ptr<ISubjectDocument>
                           {
                               return std::make_unique<Editor::SequencerPanel>(
                                    subject, SubjectEntityName( subject, "Sequencer" ),
                                    Editor::SequencerPanel::Timeline::Skeletal, m_MainScene,
                                    m_AnimationLibrary.get(), m_AssetManager.get() );
                           },
                           [this]( const SubjectId& subject )
                           {
                               return EntityHasComponent<ECS::SkinnedMeshComponent>( subject.Owner ) &&
                                      EntityHasComponent<ECS::AnimationComponent>( subject.Owner );
                           } } );
        m_SubjectEditors.Register(
             Editor::SequencerPanel::UISubjectType(),
             Registration{ Editor::SequencerPanel::kUIComponentTypeName, ICON_MDI_CHART_TIMELINE_VARIANT,
                           [this]( const SubjectId& subject ) -> std::unique_ptr<ISubjectDocument>
                           {
                               return std::make_unique<Editor::SequencerPanel>(
                                    subject, SubjectEntityName( subject, "UI Timeline" ),
                                    Editor::SequencerPanel::Timeline::UI, m_MainScene, m_AnimationLibrary.get(),
                                    m_AssetManager.get() );
                           },
                           [this]( const SubjectId& subject )
                           { return EntityHasComponent<ECS::UIAnimComponent>( subject.Owner ); } } );

        // ── AND HOW A PATH BECOMES ONE OF THEM ────────────────────────────────────────────────────────
        //
        // The asset browser's double-click used to carry a chain of `else if` over the file types, one arm
        // per kind of document, and this file carried a second copy of the same chain. Registered here
        // instead, beside the editors they feed, so the browser asks once and a new format is a line in
        // this block rather than an edit in two files somebody has to remember exist.
        //
        // AND WHICH EXTENSIONS EACH ONE ANSWERS FOR. Taken from the format's own constant, never spelled
        // again here: the palette ENUMERATES the project's openable files against this list, so a literal
        // that drifted from the resolver's would produce a list of entries the resolver then refuses.
        m_SubjectEditors.RegisterPathOpener( { std::string( Common::Constants::Extensions::MATERIAL_EXTENSION ) },
                                             [this]( const std::string& path )
                                             {
                                                 switch ( RequestMaterialDocument( m_AssetManager.get(), path ) )
                                                 {
                                                     case MaterialDocumentRequest::NotAMaterialPath:
                                                         return SubjectEditorRegistry::PathOpenOutcome::NotMine;
                                                     case MaterialDocumentRequest::Failed:
                                                         return SubjectEditorRegistry::PathOpenOutcome::Failed;
                                                     case MaterialDocumentRequest::Requested:
                                                         return SubjectEditorRegistry::PathOpenOutcome::Requested;
                                                 }
                                                 return SubjectEditorRegistry::PathOpenOutcome::NotMine;
                                             } );
        m_SubjectEditors.RegisterPathOpener( { std::string( Assets::kCloudNoiseVolumeExtension ),
                                               std::string( Assets::kCloudTypeExtension ),
                                               std::string( Assets::kCloudModellingVolumeExtension ),
                                               std::string( Assets::kCloudLayoutExtension ) },
                                             [this]( const std::string& path )
                                             {
                                                 switch ( RequestCloudDocument( m_AssetManager.get(), path ) )
                                                 {
                                                     case CloudDocumentRequest::NotACloudPath:
                                                         return SubjectEditorRegistry::PathOpenOutcome::NotMine;
                                                     case CloudDocumentRequest::Failed:
                                                         return SubjectEditorRegistry::PathOpenOutcome::Failed;
                                                     case CloudDocumentRequest::Requested:
                                                         return SubjectEditorRegistry::PathOpenOutcome::Requested;
                                                 }
                                                 return SubjectEditorRegistry::PathOpenOutcome::NotMine;
                                             } );
        m_SubjectEditors.RegisterPathOpener(
             { std::string( Assets::Serialization::ShaderGraph::kShaderGraphExtension ) },
             [this]( const std::string& path )
             {
                 switch ( RequestShaderGraphDocument( m_AssetManager.get(), path ) )
                 {
                     case ShaderGraphDocumentRequest::NotAGraphPath:
                         return SubjectEditorRegistry::PathOpenOutcome::NotMine;
                     case ShaderGraphDocumentRequest::Failed:
                         return SubjectEditorRegistry::PathOpenOutcome::Failed;
                     case ShaderGraphDocumentRequest::Requested:
                         return SubjectEditorRegistry::PathOpenOutcome::Requested;
                 }
                 return SubjectEditorRegistry::PathOpenOutcome::NotMine;
             } );

        // NOTHING OPENS A PANEL AT BOOT ANY MORE, and the absence is the point.
        //
        // `--open-panel <name>` stood here: a flag that put a tool on screen because macOS refuses this
        // machine synthetic input, so there was no other way to photograph one. It could only ever act
        // ONCE, at startup, which is all a flag can do — and every task that needed a different window
        // added another flag beside it.
        //
        // The control channel replaces the whole family. "Panel" / "Open Details" is a command palette
        // entry, so it is reachable by a person with Ctrl+P and by a client at any moment in the session,
        // as many times as it likes. See BuildPaletteCommands and Editor/Core/Control.

        // Only when the scene above really was initialised. Every editor pass builds its pipeline
        // against `scene->GetTargetFramebuffer()`, which does not exist until SceneRenderer::Init has
        // run — and when a scene load is already queued that Init is deliberately skipped (see the
        // comment beside it). The load recreates this registry after its own Init, which is what the
        // three other call sites of this line are for.
        if ( m_MainScene->IsInitialized() )
            m_RenderRegistry = std::make_unique<Render::RenderRegistry>( m_MainScene );

        // Boot into an empty "New Scene" — the demo scene (procedural character/house + player_controller.lua)
        // referenced assets that were cleared out for the from-scratch rebuild. Re-enable to get it back.
        // BuildCharacterDemoScene();

        // Default scene content: a sun + procedural sky (like UE's default level) so created meshes/primitives
        // are LIT and have a backdrop (an empty scene with no light renders everything ~black).
        // ONLY for a genuinely empty boot: the constructor already gave a fresh project its Starter
        // scene (own sun+sky) and queued any existing project scene for load (brings its own). Adding
        // a sun here regardless is what produced TWO directional lights — and the engine supports one.
        const bool sceneLoadPending = m_SceneLoadRequested.has_value();
        const bool hasSun           = !m_MainScene->GetRegistry().view<ECS::DirectionLightComponent>().empty();
        if ( !sceneLoadPending && !hasSun )
        {
            using namespace ::Desert;
            auto& sun         = m_MainScene->CreateNewEntity( "Sun" );
            auto& dl          = sun.AddComponent<ECS::DirectionLightComponent>();
            dl.Data.Color     = { 1.0f, 0.97f, 0.9f };
            dl.Data.Intensity = 3.0f;
            sun.GetComponent<ECS::TransformComponent>().Translation = { -0.4f, -1.0f, -0.5f };

            auto& skyEnt    = m_MainScene->CreateNewEntity( "Skybox" );
            auto& sky       = skyEnt.AddComponent<ECS::SkyAtmosphereComponent>();
            sky.RequestBake = true;
        }

        // THE FIRST SAMPLE IS TAKEN BEFORE THE FIRST FRAME, and without it the readiness gate is wrong in
        // the one case it exists for. ServiceControlChannel runs at the TOP of OnUpdate and judges the
        // sample from the frame before; on frame zero there is no frame before, so a default-constructed
        // EditorQuiescence would answer Settled() — every flag false — and the very first request of a
        // session, which is the one a client sends while the editor is still cooking, would be answered
        // from an editor that has read nothing. An unsampled census must not read as a settled editor.
        SampleFrameQuiescence();

        return BOOLSUCCESS;
    }

    [[nodiscard]] Common::BoolResultStr EditorLayer::OnUpdate( const Common::Timestep& ts )
    {
        DESERT_PROFILE_SCOPE( "Layer::OnUpdate" );

        // THE CONTROL CHANNEL GOES FIRST, ahead of the startup-loading return below and not after it.
        //
        // A client connects while the editor is still cooking assets — that is the normal case, since it
        // launched the process — and a channel that only started answering once loading finished would
        // look, from the outside, exactly like an editor that had hung. It answers `state` throughout, so
        // the client can watch the boot; anything that changes the picture is held by the quiescence gate
        // until the staged load is done, which is what PendingWork::StartupLoading is for.
        // THE NUDGE CLOCK, AND IT RUNS BEFORE THE CHANNEL DELIBERATELY. A request queued by a command
        // this frame must not be aged by the same frame's tick, or the drop that protects the reply gate
        // would fire a frame early and a slow viewport would lose a nudge it was about to perform.
        Core::ControlNudgeRequests::Tick();

        ServiceControlChannel();

        // Staged startup loading: run ONE heavy stage per frame. While loading, the scene is NOT rendered
        // at all (shaders/assets aren't there yet — rendering before the preload stage crashed on the
        // missing StaticMeshPBR shader); the frame is ImGui-only and the window it goes to is still hidden.
        //
        // THERE USED TO BE A GATE HERE — "only after one frame with the loading overlay has been
        // presented" — and it existed for the overlay alone: a stage run before that frame froze a blank
        // window. The overlay is gone (the splash, a window of its own, replaced it), and so is the gate.
        if ( StartupLoading() )
        {
            {
                DESERT_PROFILE_SCOPE( "Startup stage" );

                // EVERY STAGE IS TIMED, and the reason is a question nobody could answer. A client
                // watching a fresh editor over this project saw the command palette's 'Open' group stay
                // empty for five minutes and had no way to say WHICH of eight stages was spending them:
                // the only startup line the log ever carried was the shader preload's, which runs in
                // OnAttach and is not one of these at all. So "the preload finished" was read as "the
                // startup finished", and the two are minutes apart. Measured here, on an otherwise idle
                // machine, the eight stages cost 6.0 s of a 51 s boot — the other 45 s is OnAttach's
                // shader preload, which is exactly the phase the one existing line already reports.
                //
                // A phase nobody can name is a phase every brief guesses at, and three of this project's
                // timed investigations went looking in the wrong one.
                // THE TIMING AND THE LINE COME FROM `Core::BootTimeline` NOW, not from a chrono pair
                // here — because the shipping runtime needed the same thing and two copies of an
                // accumulation rule is how the two numbers stop being comparable. The SCHEDULER stays
                // here: running one stage per frame behind a progress overlay is this layer's own
                // arrangement and has nothing to do with timing. See Engine/Core/BootTimeline.hpp.
                ReportSplashStep( m_StartupStages[m_StartupNext].Label, m_StartupNext + 1 );
                const auto stageStart = std::chrono::steady_clock::now();
                m_StartupStages[m_StartupNext].Run();
                const double stageMs =
                     std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - stageStart )
                          .count();
                ++m_StartupNext;
                m_Boot.Record( m_StartupStages[m_StartupNext - 1].Label, stageMs );

                if ( !StartupLoading() )
                {
                    // THE SETTLE PHASE GETS ITS OWN LABEL. A splash that says nothing while it waits is
                    // indistinguishable from an editor that has hung, and this wait is the one the
                    // demand-driven model introduced.
                    ReportSplashStep( "Waiting for the scene's content...", SplashSettleStep() );
                    m_Boot.LogSummary();
                    LOG_INFO( "[Startup] all {} stage(s) done in {:.1f} ms; the editor is now answering "
                              "about a project it has actually read.",
                              m_StartupStages.size(), m_Boot.ElapsedMs() );
                    // AND THE BOOT IS OVER HERE — not at the first frame, which the hidden window has
                    // been presented for the whole of the staged load. Every synchronous asset load
                    // after this line reports itself as a hitch. See Engine/Assets/SyncLoadLedger.hpp.
                    Assets::SyncLoadLedger::NoteBootFinished();
                    LOG_INFO( "[SyncLoad] boot finished — {}", Assets::SyncLoadLedger::Report() );
                    LOG_INFO( "[Memory] boot finished — {}", Graphic::MemoryReadout::Take().Report() );
                    // HOW MANY HANDLES CAN NAME THEIR OWN FILE BY THE TIME THE BOOT IS OVER. The
                    // eager preloader's directory walk is what mints them, so this number IS the size
                    // of the path->handle inverse the engine holds at that moment — and therefore the
                    // exact quantity the demand-driven model has to reproduce some other way once the
                    // walk stops happening (GAP_ANALYSIS T2.4). Beside the two lines above because it
                    // answers the same question they do: what did the boot buy.
                    LOG_INFO( "[AssetPathIndex] boot finished — {} handle(s) can name their own path",
                              Common::AssetPathIndex::Size() );
                    // AND WHAT IT COST TO MINT THEM. The line above is only an achievement next to this
                    // one: the same count reached with directory walks and reached without them are two
                    // different boots, and nothing else in the process can tell them apart (§T2.4).
                    LOG_INFO( "[ContentScan] boot finished — {}", Common::Utils::ContentScanLedger::Report() );

                    // AND ONLY NOW THE EDITOR DOES ITS COOK — after the three lines above, which is not
                    // a tidiness choice. `Refresh` WALKS the content roots (it is the one walk left in
                    // this engine), and a walk before the `[ContentScan]` line would have made the
                    // registry a place the cost moved to rather than a place it stopped being paid:
                    // the number the whole tier is judged by would report the walk it removed.
                    //
                    // What it is for: content that arrived on disk without going through this editor —
                    // a `git pull`, a file dropped into the folder while the editor was closed — has no
                    // row, and since the boot no longer walks, it is content the engine does not have.
                    // This enters it, so it is there the NEXT time the project opens, and says how many
                    // it found. A file authored IN the editor never waits for this: `CreateAsset` notes
                    // its row the moment it exists.
                    //
                    // A REFUSAL IS LOGGED AND THE SESSION CONTINUES, unlike `Load`'s. Nothing in this
                    // session depends on the cook: the editor is already running over the registry it
                    // read, and failing to write the next boot's copy is a reason to say so loudly, not
                    // a reason to stop editing.
                    if ( const auto cooked = Assets::ContentRegistry::Refresh( *m_AssetManager ); !cooked )
                    {
                        LOG_ERROR( "[ContentRegistry] the content registry could not be cooked: {}",
                                   cooked.GetError() );
                    }
                    else
                    {
                        // THE COOK'S OWN WALK COST, READ OUT OF THE SAME LEDGER the boot line above
                        // reports zero from. This is the one number that says what removing the scan
                        // from the boot actually bought, measured rather than argued: the cook runs
                        // exactly the content scans the boot used to run, through the same primitive,
                        // on the same tree, seconds later on the same machine.
                        LOG_INFO( "[ContentRegistry] {}; the cook itself did {}", cooked.GetValue().Describe(),
                                  Common::Utils::ContentScanLedger::Report() );
                    }
                }
            }
            SampleFrameQuiescence();
            return BOOLSUCCESS;
        }

        // The timestep this frame is driven by. Identical to the wall-clock one the application measured,
        // EXCEPT under a `--play` capture, where it is the fixed step from ShotOptions.
        //
        // Substituted for the whole layer update rather than only for the scene: a capture is reproducible
        // only if nothing in it integrates a number that came from a clock, and "the scene is deterministic
        // but the thing above it is not" is the kind of split that holds until the day something above it
        // starts feeding the scene. Outside `--play` this is `ts` itself, so no existing frame moves.
        const Common::Timestep frameTs( ShotOptions::Get().FrameSeconds( ts.GetSeconds() ) );

        // A scene handed over by a panel (dropped on the viewport, double-clicked in the asset browser).
        // It goes through the SAME deferred load as the menu — but a drag is easy to do by accident, so
        // unsaved work is not thrown away silently: the confirm popup decides, and only then do we queue.
        if ( auto requested = Editor::Core::SceneOpenRequest::Consume() )
        {
            const Common::Filepath path( *requested );
            if ( CommandHistory::Get().Revision() != s_SavedRevision )
            {
                m_PendingOpenScene      = path;
                m_ConfirmOpenScenePopup = true;
            }
            else
            {
                LoadScene( path );
            }
        }

        // ONE PUMP PER TICK, AND IT IS THE FIRST THING THIS LAYER DOES AFTER THE BOOT.
        //
        // `AsyncAssetLoader` never calls a delegate from inside `Request()` -- not even for an asset that
        // is already resident -- so a host that forgets this line gets a loader that reads files and
        // never tells anybody. It is first rather than last because a completion is what UPLOADS a cloud
        // volume, and doing that before the frame's passes resolve their inputs is what lets the volume
        // be used by the same frame it arrived in rather than by the next one.
        Assets::AsyncAssetLoader::Get().Pump();
        UpdateContentSettling();

        // Scene loads wait until the startup stages finished (a scene expects cooked/preloaded assets).
        if ( m_SceneLoadRequested && !StartupLoading() )
        {
            auto path = m_SceneLoadRequested.value();
            m_SceneLoadRequested.reset();
            LoadSceneInternal( path );
            BeginContentSettle();

            // THE OTHER HALF OF THE SKIPPED Init() IN OnAttach. That skip is safe only because THIS load
            // initialises the scene — and LoadSceneInternal has three early returns (the file is gone,
            // unreadable, or written by an older build) that deliberately leave the editor exactly as it
            // was. "Exactly as it was" used to mean an initialised empty scene; with the first Init()
            // pre-empted it would mean a scene whose renderer has no systems, and the frame below would
            // record against it. Asked of the SCENE rather than inferred from the load's return value,
            // which is void, and rather than tracked in a flag here, which would be a second copy of a
            // fact the scene already holds.
            if ( !m_MainScene->IsInitialized() )
            {
                if ( const auto inited = m_MainScene->Init(); !inited.IsSuccess() )
                    LOG_ERROR( "[EditorLayer] the scene load was refused and the fallback initialise "
                               "failed too: {}",
                               inited.GetError() );
                // The registry follows the Init that built the framebuffers its passes bind to, exactly
                // as it does on the successful path inside LoadSceneInternal.
                m_RenderRegistry.reset();
                m_RenderRegistry = std::make_unique<Render::RenderRegistry>( m_MainScene );
            }
        }

        // New (empty) scene — deferred like a load so it never tears down resources mid-frame.
        if ( m_NewSceneRequested && !StartupLoading() )
        {
            m_NewSceneRequested = false;
            NewSceneInternal();
        }

        // Opening an extra scene view allocates a fresh SceneRenderer + Init() (WaitDeviceIdle + framebuffer
        // creation) — deferred here, between frames, for the same reason as scene load/stop above.
        if ( m_AddSceneViewRequested && !StartupLoading() )
        {
            m_AddSceneViewRequested = false;
            AddSceneView();
        }

        // A second ANGLE on the active document — same deferral, same reasons.
        if ( m_AddSceneViewportRequested && !StartupLoading() )
        {
            m_AddSceneViewportRequested = false;
            AddSceneViewport();
        }

        // Four angles at once — the same deferral again: it opens up to three views.
        if ( m_ViewportGridRequested && !StartupLoading() )
        {
            m_ViewportGridRequested = false;
            BuildViewportGrid();
        }

        // ...and closing one destroys the same resources, so it is deferred to the same place. It must also
        // run BEFORE the OnPreUpdate loop below and before UpdateSceneFrame: a document whose window the user
        // dismissed last frame would otherwise get one more full scene render, and — until this existed at
        // all — every subsequent frame for the rest of the session, holding one of the six renderer slots.
        CloseDismissedSceneViews();
        CloseDismissedSceneViewports();

        // A DOCUMENT WHOSE SUBJECT HAS GONE IS QUEUED FOR CLOSING BEFORE THE QUEUE IS SERVICED, so the
        // window disappears on the same frame the entity or the asset did rather than one later. It runs
        // after CloseDismissedSceneViews above deliberately: closing a scene view is one of the ways a
        // subject dies, and a document over an entity in that scene must see the scene gone, not still
        // going. See CloseDocumentsWhoseSubjectIsGone.
        CloseDocumentsWhoseSubjectIsGone();

        // Documents follow the scene views exactly, and for the same reason: closing one destroys a Scene
        // and a SceneRenderer, neither of which is legal from inside the ImGui pass that noticed the click.
        // Closes run BEFORE opens so a slot handed back this frame is available to whatever the user is
        // opening in it. The hidden-document slot release rides in the same function, behind the same
        // device-idle wait, for the same reason.
        ServiceDocumentCloses();
        ServiceSubjectOpenRequests();

        // Stop is deferred here (between frames) so it never destroys/recreates render resources while a
        // command buffer that references them is in flight — see m_PendingSceneStop.
        if ( m_PendingSceneStop )
        {
            m_PendingSceneStop = false;
            OnSceneStop();
        }

        // First-frame prefs application (needs a live camera) + autosave timer.
        {
            static bool s_CameraSpeedApplied = false;
            if ( !s_CameraSpeedApplied )
            {
                if ( auto cam = m_MainScene->GetMainCamera().lock() )
                    if ( auto* editorCam = dynamic_cast<::Desert::Core::EditorCamera*>( cam.get() ) )
                    {
                        editorCam->SetMovementSpeed( EditorPreferences::Get().CameraSpeed );
                        s_CameraSpeedApplied = true;
                    }
            }

            // Autosave: Edit mode only, only when something actually changed since the last autosave.
            // Writes a SEPARATE file (Scene/Autosave/<name>_autosave.desce) — never touches the main save.
            static float    s_AutosaveAccum        = 0.0f;
            static uint64_t s_LastAutosaveRevision = 0;
            const auto&     prefs                  = EditorPreferences::Get();
            if ( prefs.AutosaveMinutes > 0 && m_MainScene->GetState() == ::Desert::Core::Scene::SceneState::Edit )
            {
                s_AutosaveAccum += frameTs.GetSeconds();
                if ( s_AutosaveAccum >= static_cast<float>( prefs.AutosaveMinutes ) * 60.0f )
                {
                    s_AutosaveAccum    = 0.0f;
                    const uint64_t rev = CommandHistory::Get().Revision();
                    if ( rev != s_LastAutosaveRevision )
                    {
                        Desert::Core::SceneSerializer serializer( m_MainScene.get(), m_AssetManager.get() );
                        std::string                   name = m_MainScene->GetSceneName();
                        for ( auto& ch : name )
                            if ( ch == ' ' )
                                ch = '_';
                        const auto      dir = Common::Constants::Path::SCENE_PATH / "Autosave";
                        std::error_code ec;
                        std::filesystem::create_directories( dir, ec );
                        const auto path = dir / ( name + "_autosave" +
                                                  std::string( Common::Constants::Extensions::SCENE_EXTENSION ) );
                        const auto written = ec ? Common::MakeFormattedError( "could not create {}: {}",
                                                                              dir.string(), ec.message() )
                                                : Common::Utils::FileSystem::WriteContentToFileAtomic(
                                                       path, serializer.SerializeToJson() );
                        if ( written )
                        {
                            // The revision is marked done ONLY on a write that landed. It used to be
                            // marked before the write, so a failed autosave was never retried: the next
                            // tick saw the same revision, decided nothing had changed, and skipped —
                            // and the log said the autosave had happened. A user going for their
                            // autosave after a crash found an old file or none.
                            s_LastAutosaveRevision = rev;
                            LOG_INFO( "[Autosave] {}", path.string() );
                        }
                        else
                        {
                            LOG_ERROR( "[Autosave] {} was NOT written: {}. The next autosave tick will "
                                       "try this revision again.",
                                       path.string(), written.GetError() );
                        }
                    }
                }
            }
        }

        // Apply any deferred panel state (e.g. viewport resize) before scene rendering.
        // Panels defer GPU-side resize from OnUIRender to here so descriptor set pools are
        // never destroyed while their DS are bound to the recording command buffer.
        for ( auto& panel : m_Panels )
            panel->OnPreUpdate();
        // The documents get the same call, from their own owner. Two loops rather than one is the visible
        // cost of the split, and it is the cost that buys "the View menu cannot list a document": every
        // place that used to iterate one container now names which of the two it means.
        for ( auto& document : m_OpenDocuments )
            document->OnPreUpdate();

        // THE WORLDS INSIDE UI RENDER-TEXTURE ELEMENTS, advanced HERE and nowhere else (Ю16). Each one is
        // a whole scene render, and it has to be recorded before ANY pass of this frame opens: the canvas
        // walk that samples the result runs inside the UI external pass, and Vulkan has no nested render
        // pass. Same constraint, same position in the frame, as PreviewViewport::Update — which says so in
        // its own header after the editor was bitten by the descriptor-pool version of it.
        //
        // Every open document, not only the focused one, for the same reason UpdateSceneFrame below runs
        // for every one: a secondary viewport showing a canvas is a live view, and a render-texture
        // element in it that stopped being advanced would show a frozen world with nothing in the log.
        if ( m_AssetManager )
        {
            if ( m_RenderRegistry )
            {
                m_RenderRegistry->TickRenderTextures( *m_AssetManager, frameTs );
            }
            for ( auto& doc : m_ExtraScenes )
            {
                if ( doc->Registry )
                {
                    doc->Registry->TickRenderTextures( *m_AssetManager, frameTs );
                }
            }
        }

        // ONE thumbnail capture pump for the whole editor. Panels only request; whether the asset browser
        // is open, hidden or closed no longer changes whether previews progress, and a request made by one
        // panel is finished for all of them.
        ThumbnailService::Get().Tick();

        UpdateContextualPanels();

        // Asset hot-reload: pick up edited .demat/.shader files (runs BEFORE scene rendering so
        // a shader-triggered pipeline invalidation never touches an in-recording frame).
        if ( m_AssetManager )
            m_AssetHotReload.Tick( frameTs, *m_AssetManager, m_MainScene.get() );

        // Destroy invalidated runtime materials (shader switched in the editor / hot reload) at
        // the only safe point: before any command recording, behind a device-idle wait. Doing it
        // where Invalidate() is called (mid-UI, mid-recording) kills descriptor pools the current
        // command buffer references -> device lost.
        if ( auto* materialService = Runtime::ResourceRegistry::GetMaterialService() )
            materialService->CollectGarbage();

        // Advance + upload any playing UI videos at this safe point (behind the device-idle wait, before
        // command recording) so authored videos animate live in the viewport; SetData flushes its own
        // transfer, and the UI walk later just samples the freshly-updated frame texture.
        if ( auto* videoService = Runtime::ResourceRegistry::GetVideoService() )
            videoService->UpdateAll();

        // Screenshot mode, `--play`: start the world before the first frame that will be counted.
        //
        // Through OnScenePlay(), the same entry the toolbar's Play button uses, so a headless run is a Play
        // session and not a second definition of one — the snapshot it takes is what would let a Stop
        // restore the authored scene, and a capture that entered Play by some private shortcut would drift
        // from the editor the day either changed.
        //
        // The camera is PINNED first, and that is the whole reason this block is not one line. Play hands
        // the view to the scene's own CameraComponent (Scene::UpdateActiveCameraSource), which would take
        // the shot away from `--camera`/`--look` in any scene that has a camera entity — and take it
        // SILENTLY, because the placement below asks for an EditorCamera and would simply not find one.
        // Pinning is the engine's existing "this view is driven from outside" mechanism and headless
        // capture is exactly that case, so `--play` changes what MOVES in the frame and nothing about
        // where the frame is taken from.
        if ( auto& shot = ShotOptions::Get(); shot.PlayActive() && !m_SceneLoadRequested && !StartupLoading() &&
                                              m_MainScene &&
                                              m_MainScene->GetState() == ::Desert::Core::Scene::SceneState::Edit )
        {
            if ( m_MainScene->GetActiveCamera() )
            {
                m_MainScene->PinActiveCamera( m_MainScene->GetActiveCamera() );
                if ( shot.FlightRoute.has_value() )
                {
                    // A ZERO AVERAGING WINDOW makes the profiler publish every frame, so the numbers read
                    // on frame k are exactly frame k-1's (FlightLog). The window is a display setting; a
                    // headless flight has no panel to smooth for, and one source of timing serves both.
                    Common::Profiling::Profiler::Get().AvgWindowSeconds() = 0.0f;
                    LOG_INFO( "[Flight] '{}': {:.0f} m at {:.0f} cm/s, {} warm-up frame(s) then {} frame(s); "
                              "CSV to '{}'",
                              shot.FlightRoute->Spec, Flight::RouteLength( *shot.FlightRoute ) / 100.0,
                              shot.FlightSpeed, Flight::kWarmupFrames, shot.Frames - Flight::kWarmupFrames,
                              shot.FlightCsv );
                }
                OnScenePlay();
                LOG_INFO( "[Shot] --play: gameplay running at a fixed {} s step; the {} captured frames are "
                          "{} s of simulated time",
                          ShotOptions::PlayStepSeconds, shot.Frames, shot.SimulatedSeconds( shot.Frames ) );
            }
            else
            {
                // Refused rather than played anyway: with nothing to pin, Play would pick a view of its own
                // and the capture would answer a question about a pose nobody asked for — while looking
                // exactly like a legitimate result.
                LOG_ERROR( "[Shot] --play refused: scene '{}' has no active camera to pin, and Play would "
                           "choose the view itself. No gameplay time advanced; this capture is a frozen "
                           "world.",
                           m_MainScene->GetSceneName() );
                shot.Play = false;
            }
        }

        // Screenshot mode, FIRST HALF: place the camera for the frame that is about to be rendered.
        //
        // Before the render and not after it, because the capture below reads back whatever the render
        // produced: with the placement after it, the image written as frame N was rendered from the pose
        // of frame N-2, and on a MOVING path that is not a bookkeeping detail — a 120-degree pan over 90
        // frames puts the last captured frame 1.35 degrees, about 28 pixels, short of the endpoint the
        // command line named. Frame N is rendered from pose N, and the final frame lands exactly on
        // `--camera-to` / `--look-to`.
        //
        // With `--camera-to` / `--look-to` the pose is re-placed EVERY frame, walking the path across
        // exactly the warm-up frames. Without them `HasMotion()` is false, the placement happens once at
        // parameter 0, and the pose it computes is (Position, Forward) to the bit.
        if ( auto& shot = ShotOptions::Get(); shot.Active() && shot.HasCamera && !m_SceneLoadRequested &&
                                              !StartupLoading() &&
                                              ( !m_ShotCameraPlaced || shot.HasMotion() || shot.FlightRoute ) )
        {
            if ( ::Desert::Core::EditorCamera* cam = ActiveEditorCamera(); cam && shot.FlightRoute )
            {
                const Flight::Pose pose =
                     Flight::PoseAt( *shot.FlightRoute, Flight::DistanceAt( m_ShotFrame, shot.FlightSpeed,
                                                                            ShotOptions::PlayStepSeconds ) );
                PlaceEditorCamera( *cam, pose.Position, pose.Forward );
                cam->SetInputEnabled( false );
            }
            else if ( cam )
            {
                // THE SAME PLACEMENT THE CONTROL CHANNEL USES. It used to be spelled out here, with the
                // framing distance written twice on one line as a bare 500.0f — and it was the ONLY way to
                // place the camera at all, so a developer who wanted a viewpoint and no capture had to
                // launch with `--shot --shot-frames 1000000` to reach it. See ViewportCameraProperties.hpp.
                const ShotCamera view = shot.CameraAt( shot.Parameter( m_ShotFrame ) );
                PlaceEditorCamera( *cam, view.Position, view.Forward );
                cam->SetInputEnabled( false ); // nothing may nudge it between here and the capture
            }
            m_ShotCameraPlaced = true;
        }

        // WAS ANYTHING STILL OUTSTANDING WHEN THIS FRAME WAS MADE? Sampled HERE, and the position is the
        // whole of its meaning: after every deferred queue above has drained — scene loads, document
        // closes, asset opens, leaving Play — and before a single pixel of this frame is rendered.
        //
        // Sampled rather than asked for later, because by the time the frame has been presented the
        // answer has moved on, and the question the control channel needs answered is about the picture:
        // "did this frame have everything the command asked for in it, or was some of it still queued?"
        // Editor/Core/Control/ControlPipeline.hpp is where that question is judged.
        SampleFrameQuiescence();

        // Multi-scene editing: drive EVERY open document each frame so all viewports render live. The active
        // one is m_MainScene (rebound on viewport focus); RigBuilder / F9 below act on it only. The outline
        // aid + Begin/RegistryRender/OnUpdate/End are folded into UpdateSceneFrame (see below), applied per
        // scene so a secondary viewport is a full, independent render — not a static snapshot.
        if ( auto r = UpdateSceneFrame( *m_PrimaryScene, m_RenderRegistry.get(), frameTs ); !r )
            return Common::MakeError( r.GetError() );
        for ( auto& doc : m_ExtraScenes )
            if ( auto r = UpdateSceneFrame( *doc->Scene, doc->Registry.get(), frameTs ); !r )
                return Common::MakeError( r.GetError() );

        // Runs a queued "Convert to Skinned" (rig builder) here, outside ImGui component iteration — the swap
        // removes the StaticMeshComponent the Details panel is drawing, so it must not happen mid-render.
        if ( m_MainScene && m_AssetManager )
            RigBuilder::ProcessPending( *m_MainScene, *m_AssetManager );

        // Screenshot mode, SECOND HALF: the frame just rendered is the frame that gets written. The frame
        // count is not decoration — a temporally accumulating pass needs several frames to converge, so an
        // early shot is a picture of the dither rather than of the scene.
        // `!ContentSettling()` IS NOT A CONVENIENCE HERE, IT IS THE CORRECTNESS OF EVERY CAPTURE THIS
        // REPOSITORY TAKES. `--shot-frames N` counts rendered frames, and before the cloud kinds became
        // demand-driven every one of them was a frame whose content was already resident. Counting from
        // the first frame after a scene load would now start the count while a worker is still reading
        // the sky, so a low-frame capture would photograph a scene with no clouds in it and file it as
        // the picture of the scene -- the same shape as the blank-PNG trap the verification skill warns
        // about, and just as invisible in a diff of two such frames.
        if ( const auto& shot = ShotOptions::Get();
             shot.FlightRoute && m_MainScene && !m_SceneLoadRequested && !StartupLoading() &&
             m_MainScene->GetState() == ::Desert::Core::Scene::SceneState::Play )
            RecordFlightFrame( !ContentSettling() );

        if ( auto& shot = ShotOptions::Get();
             shot.Active() && !m_SceneLoadRequested && !StartupLoading() && !ContentSettling() )
        {
            ++m_ShotFrame;

            // The sequence counts RENDERED frames, so `frame_00001` is the first frame rendered, from path
            // parameter 0, and `frame_000NN` at --shot-frames NN is the last, from parameter 1. When
            // `--shot-every` divides `--shot-frames` the last file of the sequence and the `--shot` PNG are
            // the same image — a cheap invariant to check a capture against.
            if ( !shot.Sequence.empty() && ( m_ShotFrame % shot.SequenceEvery ) == 0 )
            {
                char name[64];
                std::snprintf( name, sizeof( name ), "/frame_%05d.png", m_ShotFrame );
                const std::string path = shot.Sequence + name;
                if ( !WriteViewportPng( path ) )
                {
                    LOG_ERROR( "[Shot] sequence frame {} not written to '{}'", m_ShotFrame, path );
                    m_ShotFailed = true;
                }
            }

            if ( m_ShotFrame >= shot.Frames )
            {
                if ( !shot.Output.empty() && !WriteViewportPng( shot.Output ) )
                {
                    LOG_ERROR( "[Shot] the final frame was not captured to '{}'", shot.Output );
                    m_ShotFailed = true;
                }
                if ( shot.GpuProfile )
                    DumpProfilerToLog();
                if ( shot.FlightRoute && !FinishFlight() )
                    m_ShotFailed = true;

                // WHAT THE CAPTURE COST ON THE DEVICE, AT THE ONE INSTANT THE PICTURE DESCRIBES.
                //
                // Until this line the only memory readings a headless run produced came from BOOT and
                // from the moment a renderer slot was built — both of them BEFORE any texture the scene
                // needs has been uploaded, because `TextureService::Get` builds the GPU texture on first
                // use and first use is a frame. Measured on the world scene: at "Renderer slot 0 built"
                // the ledger reports AssetService holding 76 shaders and ZERO Image2D, and the scene's
                // one texture only appears a hundred frames later. So every figure anybody had for
                // "texture memory on this scene" was taken before the textures existed.
                //
                // Unconditional, and not behind `--gpu-profile`: a reading nobody remembers to ask for
                // is a reading nobody has. It is three queries and one locked walk, once, on the frame
                // that ends the process.
                LOG_INFO( "[Memory] shot taken — {}", Graphic::MemoryReadout::Take().Report() );
                LOG_INFO( "[Resources] {}", Graphic::ResourceLedger::Report() );
                LOG_INFO( "[Memory] {}", Graphic::MemoryWatch::Report() );
                // A capture that wrote no PNG must not leave a zero exit status behind: the whole value of
                // an exit code is that a script can trust it, and this one used to say "fine" either way.
                const_cast<Engine::Application*>( m_Application )->Close( m_ShotFailed ? 1 : 0 );
            }
        }

        // DEBUG: press F9 to dump the final rendered viewport image to F:/DesertEngine/frame_dump.png. Useful
        // because external GDI/PrintWindow capture returns white for the Vulkan surface — this reads the actual
        // rendered frame back from the GPU. Edge-detected so one press = one dump.
        {
            static bool s_f9Prev = false;
            const bool  f9       = Input::Keyboard::IsKeyPressed( Common::KeyCode::F9 );
            if ( f9 && !s_f9Prev )
            {
                if ( !WriteViewportPng( "F:/DesertEngine/frame_dump.png" ) )
                    LOG_ERROR( "[Dump] final frame could not be written" );
            }
            s_f9Prev = f9;
        }

        return BOOLSUCCESS;
    }

    // The one readback. Every picture the editor writes out of the viewport comes through here, so a
    // capture cannot quietly differ from a dump in flip, format or the device-idle wait that makes the
    // readback legal at all.
    Common::BoolResultStr EditorLayer::ReadViewportRGBA8( std::vector<uint8_t>& outPixels, uint32_t& outWidth,
                                                          uint32_t& outHeight )
    {
        if ( !m_MainScene )
            return Common::MakeError<bool>( "no scene to capture" );

        Graphic::Renderer::GetInstance().WaitDeviceIdle();

        auto img = m_MainScene->GetFinalImage();
        if ( !img )
            return Common::MakeError<bool>( "scene has no final image" );

        // Г13: ReadPixelsRGBA8 answers instead of returning an empty vector for every kind of failure.
        // The size check below is kept and now means only what it says — this arm carries the reason.
        auto read = img->ReadPixelsRGBA8();
        if ( !read.IsSuccess() )
            return Common::MakeFormattedError<bool>( "readback refused: {}", read.GetError() );

        outPixels = read.ExtractValue();
        outWidth  = img->GetWidth();
        outHeight = img->GetHeight();
        if ( outPixels.size() != static_cast<size_t>( outWidth ) * outHeight * 4 )
            return Common::MakeFormattedError<bool>( "readback is {} bytes, expected {}x{}x4 = {}",
                                                     outPixels.size(), outWidth, outHeight,
                                                     static_cast<size_t>( outWidth ) * outHeight * 4 );
        return BOOLSUCCESS;
    }

    Common::BoolResultStr EditorLayer::WriteProjectThumbnail()
    {
        // The picture belongs to a project, so with no project there is nowhere for it to go. Not an
        // error: the editor can be running a scene that was opened without one.
        const std::string projectDirectory = Desert::Project::ProjectContext::Directory();
        if ( projectDirectory.empty() )
            return BOOLSUCCESS;

        std::vector<uint8_t> px;
        uint32_t             w = 0;
        uint32_t             h = 0;
        if ( const auto read = ReadViewportRGBA8( px, w, h ); !read.IsSuccess() )
            return read;
        if ( w == 0 || h == 0 )
            return Common::MakeError<bool>( "the viewport has no size" );

        // 16:9 window out of the middle of whatever the viewport is, then a box downsample to the
        // fixed output size. Cropping rather than squashing, because a squashed frame is a picture
        // of the wrong world; centre rather than top, because the interesting part of a viewport is
        // where the camera is pointed.
        constexpr uint32_t kOutW = 512;
        constexpr uint32_t kOutH = 288; // 16:9 — the aspect the launcher's grid is built out of

        uint32_t cropW = w;
        uint32_t cropH = ( w * kOutH ) / kOutW;
        if ( cropH > h )
        {
            cropH = h;
            cropW = ( h * kOutW ) / kOutH;
        }
        const uint32_t cropX = ( w - cropW ) / 2;
        const uint32_t cropY = ( h - cropH ) / 2;

        std::vector<uint8_t> out( static_cast<size_t>( kOutW ) * kOutH * 4 );
        for ( uint32_t y = 0; y < kOutH; ++y )
        {
            // Source rows this output row averages over. Integer bounds on both ends so no source
            // pixel is counted twice and none is skipped.
            const uint32_t sy0 = cropY + ( y * cropH ) / kOutH;
            const uint32_t sy1 = std::max( sy0 + 1u, cropY + ( ( y + 1 ) * cropH ) / kOutH );
            for ( uint32_t x = 0; x < kOutW; ++x )
            {
                const uint32_t sx0 = cropX + ( x * cropW ) / kOutW;
                const uint32_t sx1 = std::max( sx0 + 1u, cropX + ( ( x + 1 ) * cropW ) / kOutW );

                uint32_t accum[4] = { 0, 0, 0, 0 };
                uint32_t samples  = 0;
                for ( uint32_t sy = sy0; sy < sy1 && sy < h; ++sy )
                    for ( uint32_t sx = sx0; sx < sx1 && sx < w; ++sx )
                    {
                        const size_t at = ( static_cast<size_t>( sy ) * w + sx ) * 4;
                        for ( int c = 0; c < 4; ++c )
                            accum[c] += px[at + static_cast<size_t>( c )];
                        ++samples;
                    }
                const size_t dst = ( static_cast<size_t>( y ) * kOutW + x ) * 4;
                for ( int c = 0; c < 4; ++c )
                    out[dst + static_cast<size_t>( c )] =
                         static_cast<uint8_t>( samples ? accum[c] / samples : 0u );
            }
        }

        // The name is a CONVENTION shared with the launcher, which looks for exactly this file
        // beside the .deproj. Leading dot so it does not show up as project content.
        const std::string file = ( std::filesystem::path( projectDirectory ) / ".thumbnail.png" ).string();
        stbi_flip_vertically_on_write( 0 );
        if ( stbi_write_png( file.c_str(), static_cast<int>( kOutW ), static_cast<int>( kOutH ), 4, out.data(),
                             static_cast<int>( kOutW ) * 4 ) == 0 )
            return Common::MakeFormattedError<bool>( "could not write {}", file );
        LOG_INFO( "[Project] thumbnail -> {} ({}x{})", file, kOutW, kOutH );
        return BOOLSUCCESS;
    }

    // Read the resolved viewport back off the GPU and write it as a PNG. The one place that does this: the
    // still capture, every frame of a `--shot-sequence`, and the F9 dump all go through here, so a capture
    // cannot quietly differ from a dump in flip, format or the device-idle wait that makes the readback
    // legal at all.
    bool EditorLayer::WriteViewportPng( const std::string& path )
    {
        if ( !m_MainScene )
        {
            LOG_ERROR( "[Shot] no scene to capture ('{}')", path );
            return false;
        }

        // The directory of a sequence is named on the command line and usually does not exist yet. Create
        // it rather than letting stb fail on a path that is only missing a folder.
        const std::filesystem::path file = std::filesystem::path( path );
        if ( file.has_parent_path() && !file.parent_path().empty() )
        {
            std::error_code ec;
            std::filesystem::create_directories( file.parent_path(), ec );
            if ( ec && !std::filesystem::exists( file.parent_path() ) )
            {
                LOG_ERROR( "[Shot] could not create '{}': {}", file.parent_path().string(), ec.message() );
                return false;
            }
        }

        std::vector<uint8_t> px;
        uint32_t             w = 0;
        uint32_t             h = 0;
        if ( const auto read = ReadViewportRGBA8( px, w, h ); !read.IsSuccess() )
        {
            LOG_ERROR( "[Shot] {} ('{}')", read.GetError(), path );
            return false;
        }

        stbi_flip_vertically_on_write( 0 );
        const bool written = stbi_write_png( path.c_str(), w, h, 4, px.data(), w * 4 ) != 0;
        LOG_INFO( "[Shot] {} -> {} ({}x{})", written ? "wrote" : "FAILED to write", path, w, h );
        return written;
    }

    // =============================================================================================
    // THE CONTROL CHANNEL
    //
    // Four functions and one promise. The promise is that a reply leaves only after a frame that already
    // reflects the command it answers — see Editor/Core/Control/ControlPipeline.hpp for why that is not
    // the same as "the next frame", and what it took to make it true rather than usually true.
    //
    // The order around one frame is:
    //   OnUpdate      ServiceControlChannel()   read a request, run it, arm the gate
    //   OnUpdate      ...deferred queues drain, the scene renders...
    //   OnUpdate      SampleFrameQuiescence()   what was still outstanding while this frame was made
    //   OnUIRender ...the interface is recorded into the swapchain...
    //   present
    //   OnFramePresented                        judge the frame; take the shot; release the reply
    // =============================================================================================

    void EditorLayer::ServiceControlChannel()
    {
        if ( !m_ControlSocket.IsListening() )
            return;

        // A request whose reply has not gone out yet holds the channel. Reading a second one here would
        // hand the ordering guarantee to whoever wrote the client: two commands in flight cannot both be
        // "the command the next settled frame proves".
        if ( m_ControlInFlight )
        {
            // THE CONNECTION IS STILL SERVICED, THE SOCKET IS JUST NOT READ FROM. A second request must
            // not be taken while one is in flight — that is the whole of the ordering guarantee — but a
            // readiness wait can last a whole boot, and a peer that has GONE is only ever detected by
            // reading from it. Without this the editor would hold the channel open for a client that is no
            // longer there and refuse every new one until the wait ended by itself.
            m_ControlSocket.ServiceConnection();
            if ( AbandonControlRequestIfItsAskerIsGone() )
                return;

            // ONE STATE MEANS ONE THING: a request in flight with an IDLE gate is a request the readiness
            // wait has just released and that has not run yet. Every other combination clears itself in
            // OnFramePresented, so this is the only way to be here. Running it at the top of OnUpdate and
            // not at the point of release keeps ONE execution site for control commands — the same point
            // in the frame a request that never had to wait is run at.
            if ( m_ControlGate.IsArmed() || m_ControlPendingReply )
                return;

            const Control::Request held = *m_ControlInFlight;
            m_ControlInFlight.reset();
            RunControlRequest( held );
            return;
        }

        const std::optional<std::string> line = m_ControlSocket.PollRequestLine();
        if ( !line )
            return;

        const auto parsed = Control::ParseRequest( *line );
        if ( !parsed )
        {
            // Answered immediately: a request that did not parse has no id to echo and nothing to wait
            // for. Silence here would be indistinguishable from an editor that had stopped reading.
            m_ControlSocket.SendResponseLine(
                 Control::FormatResponse( Control::Response::Failure( 0, parsed.GetError() ) ) );
            return;
        }

        const Control::Request request = parsed.GetValue();

        // AN EDITOR THAT HAS NOT READ THE PROJECT DOES NOT ANSWER ABOUT IT.
        //
        // This is the whole of A6-1's second half, and it is one branch because the mechanism it needs
        // already existed: PendingWork::StartupLoading has been in the quiescence census since the channel
        // landed, and the gate has always been able to hold something until a presented frame proves the
        // census empty. What was missing is that the READS never asked. `commands` was answered from the
        // asset cache the moment it arrived, and the cache is filled by five separate startup stages — so
        // for 3.3 s of every boot the palette successfully offers 106 of this project's 130 openable
        // assets, and for the seconds before that, none of them. Neither answer says which it is.
        //
        // Held, not refused, because a refusal only moves the problem: the client would have to guess how
        // long to wait and ask again, which is the polling loop this channel exists to delete. The refusal
        // still exists — it is what a gate timeout produces, and it names what never finished.
        //
        // WHICH operations need this is the protocol's decision, not this file's: `state` and `quit` are
        // exempt, for reasons written where the table is (Control/ControlProtocol.hpp). EditorLayer.cpp is
        // compiled by no test suite, so a rule stated here is a rule nothing can show going red.
        if ( Control::NeedsReadyEditor( request.Operation ) && !m_FrameQuiescence.Settled() )
        {
            LOG_INFO( "[Control] request {} is waiting for the editor to finish coming up: {}.", request.Id,
                      m_FrameQuiescence.Describe() );
            m_ControlInFlight       = request;
            m_ControlInFlightClient = m_ControlSocket.ClientGeneration();
            m_ControlGate.ArmForReadiness( m_FrameIndex );
            return;
        }

        RunControlRequest( request );
    }

    // Execute one request and decide whether its reply leaves now or waits for the frame that proves it.
    //
    // Split out of ServiceControlChannel because there are now two ways to ARRIVE at a request — read from
    // the socket, or released by the readiness gate a few frames later — and exactly one way to RUN one.
    // Two execution sites for one thing is the shape this codebase spends its days removing.
    void EditorLayer::RunControlRequest( const Control::Request& request )
    {
        Control::Response response = ExecuteControlRequest( request );

        // READS ANSWER NOW; ANYTHING THAT CAN CHANGE THE PICTURE WAITS FOR ONE.
        //
        // `commands`, `properties` and `state` observe and change nothing, so making them wait for a
        // FURTHER frame would buy latency and no guarantee at all. `run`, `set` and the two shots are the
        // ones the promise is about — and a shot does not merely wait for the settled frame, it IS taken
        // on it, which is why its response is finished in OnFramePresented rather than here.
        //
        // NOT TO BE CONFUSED WITH THE READINESS WAIT ABOVE, which the reads DO take part in. The two are
        // different questions about different moments: "has the editor finished coming up, so that this
        // answer is about the real project?" is asked BEFORE a request runs, of every operation but the
        // two exemptions; "has a frame been presented that shows what this command did?" is asked AFTER,
        // and only of the commands that did something. By the time execution reaches this line the editor
        // is settled either way, so a read answers from a state it has actually finished building.
        //
        // `set` is in the list for exactly the reason `run` is: it moves the preview, and a client that
        // set a value and captured immediately would photograph the frame BEFORE it. That failure is the
        // whole subject of the sequence this document exists to prove.
        const bool waitsForAFrame =
             response.Ok() && ( request.Operation == Control::Op::Run || request.Operation == Control::Op::Set ||
                                Control::IsShot( request.Operation ) );

        if ( !waitsForAFrame )
        {
            m_ControlSocket.SendResponseLine( Control::FormatResponse( response ) );
            if ( request.Operation == Control::Op::Quit && response.Ok() )
                m_ControlQuitCode = request.ExitCode;
            return;
        }

        m_ControlInFlight     = request;
        m_ControlPendingReply = std::move( response );
        m_ControlInFlightClient = m_ControlSocket.ClientGeneration();
        m_ControlGate.ArmAfterExecution( m_FrameIndex );
    }

    /**
     * @brief Is the connection that asked still the connection on the other end? Abandon the request if
     *        not, and say whether it did.
     *
     * A REPLY MUST REACH THE CLIENT THAT ASKED FOR IT, and "somebody is connected" does not say that.
     * Measured while building the readiness wait: client A parked a `commands` during the boot and was
     * killed; the editor noticed the loss and, in the SAME service call, accepted client B into the freed
     * slot — the accept loop runs immediately after the read that detects a disconnect. HasClient() was
     * true again, the parked request went on, and A's answer of 311 commands was written to B's socket.
     * B had also sent id 1, so the reply was indistinguishable from its own at both ends.
     *
     * The generation is what makes the question answerable. Nothing is sent to the vanished client — there
     * is nobody to tell — and nothing is sent to its successor either, which is the whole point.
     */
    bool EditorLayer::AbandonControlRequestIfItsAskerIsGone()
    {
        if ( !m_ControlInFlight )
            return false;
        if ( m_ControlSocket.HasClient() && m_ControlSocket.ClientGeneration() == m_ControlInFlightClient )
            return false;

        LOG_INFO( "[Control] the client that asked for request {} is gone (generation {} -> {}); abandoning "
                  "it unanswered rather than replying to whoever holds the channel now.",
                  m_ControlInFlight->Id, m_ControlInFlightClient, m_ControlSocket.ClientGeneration() );

        m_ControlGate.Disarm();
        m_ControlInFlight.reset();
        m_ControlPendingReply.reset();
        m_ControlInFlightClient = 0;
        return true;
    }

    void EditorLayer::SampleFrameQuiescence()
    {
        Control::EditorQuiescence quiescence;
        // CONTENT SETTLING COUNTS AS STARTUP LOADING FOR THE CHANNEL, and it has to: the channel's whole
        // contract is that a reply is released only after a presented frame on whose entry no deferred
        // work from that command remained. A `shot.viewport` answered while a worker is still reading the
        // sky would hand back a picture of a scene without its clouds and call it the scene.
        quiescence.Set( Control::PendingWork::StartupLoading, StartupLoading() || ContentSettling() );
        quiescence.Set( Control::PendingWork::SceneLoad, m_SceneLoadRequested.has_value() );
        quiescence.Set( Control::PendingWork::NewScene, m_NewSceneRequested );
        // THE GRID BELONGS HERE TOO, and the pending DOCK is part of it: the layout is applied a frame
        // after the views open, so a reply released between the two would hand back a screenshot of four
        // floating windows and call it the grid.
        quiescence.Set( Control::PendingWork::SceneView, m_AddSceneViewRequested ||
                                                              m_AddSceneViewportRequested ||
                                                              m_ViewportGridRequested ||
                                                              !m_PendingViewportGrid.empty() );
        quiescence.Set( Control::PendingWork::SceneStop, m_PendingSceneStop );
        quiescence.Set( Control::PendingWork::DocumentCloses, !m_DocumentsToClose.empty() );
        // The queue is a file-static inbox drained by ServiceSubjectOpenRequests, so "is anything queued"
        // is asked of the queue itself rather than of a copy this layer keeps — a copy would be a second
        // answer, and the two would disagree on exactly the frame an open was handled halfway.
        quiescence.Set( Control::PendingWork::AssetOpens, Core::SubjectOpenRequests::HasPending() );
        quiescence.Set( Control::PendingWork::OpenRefusal, m_OpenRefusalPending );
        // Asked of the request itself, for SubjectOpenRequests' reason above. It stays pending for one
        // frame AFTER it is performed, because the frame that performs a nudge is not the frame that
        // draws it -- see Editor/Core/ControlNudgeRequest.hpp.
        quiescence.Set( Control::PendingWork::ControlNudge, Core::ControlNudgeRequests::HasPending() );
        m_FrameQuiescence = quiescence;
    }

    void EditorLayer::OnFramePresented()
    {
        ++m_FrameIndex;

        RevealWhenReady();

        if ( !m_ControlSocket.IsListening() )
            return;

        // A quit waits for its own answer to leave the machine. A client that asked the editor to close
        // and never heard back cannot tell a clean shutdown from a crash, and it is the last thing it
        // will ever hear from this process.
        if ( m_ControlQuitCode && !m_ControlSocket.HasUnsentOutput() )
        {
            const int32_t code = *m_ControlQuitCode;
            m_ControlQuitCode.reset();
            LOG_INFO( "[Control] quit requested; closing with status {}.", code );
            const_cast<Engine::Application*>( m_Application )->Close( code );
            return;
        }

        if ( !m_ControlInFlight )
            return;

        // A reply must reach the client that ASKED. Checked here as well as in ServiceControlChannel
        // because a shot's capture and its answer both happen on this side of the frame, and writing
        // either to a successor connection would hand one client another's picture.
        if ( AbandonControlRequestIfItsAskerIsGone() )
            return;

        // WHICH OF THE TWO WAITS this frame is being judged for, sampled BEFORE the verdict: a discharge
        // clears the subject, so asking afterwards would always answer "nothing".
        const Control::GateSubject holding = m_ControlGate.Holding();

        // The frame just presented is judged by the quiescence sampled while it was being BUILT. The gate
        // uses m_FrameIndex - 1 because the counter was advanced above: the frame that has just gone out
        // is the one that was being made when ServiceControlChannel armed the gate.
        const Control::GateVerdict verdict =
             m_ControlGate.ObserveFramePresented( m_FrameIndex - 1, m_FrameQuiescence );

        if ( verdict == Control::GateVerdict::Waiting || verdict == Control::GateVerdict::Idle )
            return;

        // THE READINESS HALF. The request has not run; this frame says whether it may.
        //
        // A discharge does nothing here on purpose: it leaves the request parked with an idle gate, which
        // is the one state ServiceControlChannel reads as "run this at the top of the next update". That
        // costs one frame and buys a single execution site for every control command — a request released
        // by this wait runs at exactly the point in the frame a request that never waited runs at.
        if ( holding == Control::GateSubject::Readiness )
        {
            if ( verdict == Control::GateVerdict::TimedOut )
            {
                m_ControlSocket.SendResponseLine( Control::FormatResponse( Control::Response::Failure(
                     m_ControlInFlight->Id,
                     Control::DescribeReadinessTimeout( m_FrameQuiescence, m_ControlGate.FramesWaited() ) ) ) );
                m_ControlInFlight.reset();
            }
            return;
        }

        // Unreachable while the two arms above are the only ways to arm the gate, and stated rather than
        // dereferenced: an Effect wait without a reply to release would be a request that ran and produced
        // nothing, which is the silent failure Response exists to make impossible.
        if ( !m_ControlPendingReply )
        {
            m_ControlSocket.SendResponseLine( Control::FormatResponse( Control::Response::Failure(
                 m_ControlInFlight->Id, "the channel held a reply-less request past its frame; that is a "
                                        "defect in the control channel, not in the request." ) ) );
            m_ControlInFlight.reset();
            return;
        }

        Control::Response reply = *m_ControlPendingReply;

        if ( verdict == Control::GateVerdict::TimedOut )
        {
            reply = Control::Response::Failure(
                 m_ControlInFlight->Id,
                 Control::DescribeSettleTimeout( m_FrameQuiescence, m_ControlGate.FramesWaited() ) );
        }
        else if ( Control::IsShot( m_ControlInFlight->Operation ) )
        {
            // THE SHOT IS TAKEN HERE AND NOWHERE ELSE, and that is the whole reason this hook exists.
            // This instant — after present, before the next acquire — is the only one at which the frame
            // a person would be looking at exists as bytes on the device, and it is a frame this gate has
            // just certified as reflecting the command that came before it.
            std::string error;
            const bool  window  = ( m_ControlInFlight->Operation == Control::Op::ShotWindow );
            const bool  written = window ? WriteWindowPng( m_ControlInFlight->Path, error )
                                         : WriteViewportPng( m_ControlInFlight->Path );

            if ( written )
            {
                rfl::Generic::Object payload;
                payload["path"] = rfl::Generic( m_ControlInFlight->Path );
                // Named on the wire so a report cannot quote a viewport capture as a picture of the
                // editor. The two are different subjects and only one of them contains an interface.
                payload["subject"] = rfl::Generic( std::string( window ? "window" : "viewport" ) );
                reply              = Control::Response::Success( m_ControlInFlight->Id, std::move( payload ) );
            }
            else
            {
                reply = Control::Response::Failure(
                     m_ControlInFlight->Id,
                     error.empty() ? "the capture could not be written; the log has the reason." : error );
            }
        }

        m_ControlSocket.SendResponseLine( Control::FormatResponse( reply ) );
        m_ControlInFlight.reset();
        m_ControlPendingReply.reset();
    }

    Control::Response EditorLayer::ExecuteControlRequest( const Control::Request& request )
    {
        switch ( request.Operation )
        {
            case Control::Op::Commands:
            {
                rfl::Generic::Array entries;
                for ( const PaletteCommand& command : BuildPaletteCommands() )
                {
                    rfl::Generic::Object entry;
                    entry["group"] = rfl::Generic( command.Group );
                    entry["label"] = rfl::Generic( command.Label );
                    entries.push_back( rfl::Generic( entry ) );
                }

                rfl::Generic::Object payload;
                payload["commands"] = rfl::Generic( entries );
                return Control::Response::Success( request.Id, std::move( payload ) );
            }

            case Control::Op::Run:
            {
                // ONE dictionary, built once, both resolved against and run out of. Building it twice —
                // once to look the command up and once to run it — would let the two disagree on any
                // frame where something opened or closed in between, and the command that ran would not
                // be the command that was found.
                const std::vector<PaletteCommand> dictionary = BuildPaletteCommands();
                const Control::CommandAddress     wanted{ request.Group, request.Label };
                const Control::Resolution         resolved = Control::ResolveCommand( dictionary, wanted );

                if ( !resolved.Found )
                {
                    return Control::Response::Failure( request.Id,
                                                       Control::DescribeUnknownCommand( wanted, resolved ) );
                }

                // THE COMMAND'S OWN ANSWER IS THE REPLY'S. `Run()` returned void until A6-2, so this line
                // reported success for every command that had failed — a document that would not resolve,
                // a scene that would not save, an Apply that published nothing. A script driving the
                // editor could not stop on any of them, and the whole point of the exit status is that it
                // can be trusted (Tools/DesertCtl/Source/Main.cpp says so at the top).
                if ( const auto ran = dictionary[resolved.Index].Run(); !ran )
                {
                    return Control::Response::Failure( request.Id, "'" + request.Group + "' / '" + request.Label +
                                                                        "' ran and refused: " + ran.GetError() );
                }
                return Control::Response::Success( request.Id );
            }

            case Control::Op::Properties:
            {
                if ( request.Whose == Control::Subject::Viewport )
                {
                    ::Desert::Core::EditorCamera* camera = ActiveEditorCamera();
                    if ( !camera )
                        return Control::Response::Failure( request.Id, NoEditorCameraReason() );

                    return Control::Response::Success(
                         request.Id,
                         Control::PropertiesToJson(
                              Control::kSubjects[static_cast<std::size_t>( Control::Subject::Viewport )].Name,
                              DescribeViewportCamera( camera->GetPosition(), camera->GetDirection() ) ) );
                }

                // `m_Documents` here on А6-1's side; О9-2 moved document OWNERSHIP out of the well into
                // Editor/Core/OpenDocuments.hpp and renamed the member, so the merged line asks the owner.
                ISubjectDocument* focused = m_OpenDocuments.Find( m_FocusedDocument );
                if ( !focused )
                {
                    return Control::Response::Failure(
                         request.Id,
                         "no document has the focus, so there is nothing whose properties could be listed. "
                         "Open one — 'commands' offers an entry per openable asset under the group 'Open'. "
                         "The editor's own view is a subject of its own: ask with subject 'viewport'." );
                }
                return Control::Response::Success(
                     request.Id, Control::PropertiesToJson( DocumentDisplayName( focused->GetName() ),
                                                            focused->EditableProperties() ) );
            }

            case Control::Op::Set:
            {
                if ( request.Whose == Control::Subject::Viewport )
                    return SetViewportCameraProperty( request );

                // THE FOCUSED DOCUMENT AND NO OTHER. A property named without a document would have to be
                // searched for across every open window, and the first match would win — which is a
                // different document from the one the person or the capture is looking at, on any frame
                // where two materials declare the same parameter. They almost all do.
                ISubjectDocument* focused = m_OpenDocuments.Find( m_FocusedDocument );
                if ( !focused )
                {
                    return Control::Response::Failure(
                         request.Id, "no document has the focus, so '" + request.Property +
                                          "' belongs to nothing. Open the document first; 'state' names the "
                                          "one that has the focus." );
                }

                if ( const auto written = focused->SetEditableProperty( request.Property, request.Value );
                     !written )
                {
                    return Control::Response::Failure( request.Id, written.GetError() );
                }
                return Control::Response::Success( request.Id );
            }

            case Control::Op::State:
            {
                if ( const auto valid = Control::ValidateSections( request.Sections ); !valid )
                    return Control::Response::Failure( request.Id, valid.GetError() );

                return Control::Response::Success( request.Id,
                                                   Control::ToJson( TakeEditorSnapshot(), request.Sections ) );
            }

            case Control::Op::ShotWindow:
            case Control::Op::ShotViewport:
                // Nothing happens now. The capture belongs to the settled frame this request is about to
                // wait for, and is taken in OnFramePresented; answering here would be a picture of the
                // frame BEFORE the commands that preceded it had been drawn.
                return Control::Response::Success( request.Id );

            case Control::Op::Quit:
                return Control::Response::Success( request.Id );
        }

        // Unreachable while every Op is handled above, and stated rather than left to fall off the end:
        // an Op added without a case here would otherwise return a default-constructed response, which is
        // a failure with nothing said — the one thing Response is built to make impossible.
        return Control::Response::Failure( request.Id,
                                           "this operation parsed but has no implementation; that is a "
                                           "defect in the control channel." );
    }

    // ── THE EDITOR'S OWN VIEW, AS SOMETHING THE CHANNEL CAN ADDRESS ────────────────────────────────────
    //
    // The scene's active camera, IF it is the editor's fly camera. Null in Play, where the view belongs to
    // the scene's own CameraComponent — and null is the honest answer there rather than a pinned override,
    // because "the editor camera" is not what is being looked through.

    ::Desert::Core::EditorCamera* EditorLayer::ActiveEditorCamera() const
    {
        if ( !m_MainScene )
            return nullptr;
        return dynamic_cast<::Desert::Core::EditorCamera*>( m_MainScene->GetActiveCamera().get() );
    }

    std::string EditorLayer::NoEditorCameraReason() const
    {
        if ( !m_MainScene )
            return "there is no scene, so there is no view to address.";
        return "the active view is not the editor's fly camera — the scene is in Play and its own "
               "CameraComponent is driving. Leave Play ('Action' / 'Stop' in the palette) and ask again; "
               "moving the editor camera now would change a view nobody is looking through.";
    }

    // THE ONE PLACEMENT. Both the `--camera`/`--look` capture path and the control channel's
    // `set Camera.Position` land here, which is the rule the protocol states for a property write: the
    // value goes into the same setter the widget calls, so there is one route into the camera and two ways
    // to reach it. Two copies of this would drift the day one of them learned about roll.
    //
    // SnapToDirection + Focus are the EDITOR'S OWN gestures — the clickable view-axis gizmo and F-focus —
    // and that is what makes this the same path a person's hands take rather than a private back door.
    // Focus backs the camera off along the current view direction by the framing distance, so aiming one
    // framing distance ahead is what lands it exactly on the position asked for; the two uses of that
    // distance are one named constant for that reason.
    void EditorLayer::PlaceEditorCamera( ::Desert::Core::EditorCamera& camera, const glm::vec3& position,
                                         const glm::vec3& forward )
    {
        camera.SnapToDirection( glm::normalize( forward ) );
        camera.Focus( ViewportCameraFocalPoint( position, forward ), kViewportCameraFramingDistance );
    }

    Control::Response EditorLayer::SetViewportCameraProperty( const Control::Request& request )
    {
        ::Desert::Core::EditorCamera* camera = ActiveEditorCamera();
        if ( !camera )
            return Control::Response::Failure( request.Id, NoEditorCameraReason() );

        const auto which = ValidateViewportCameraWrite( request.Property, request.Value );
        if ( !which )
            return Control::Response::Failure( request.Id, which.GetError() );

        // THE OTHER HALF OF THE POSE IS READ BACK FROM THE CAMERA, not remembered here. A client that sets
        // only the direction means "look that way from where you are", and a copy of the position kept on
        // this side would be the second answer to where the camera is — wrong the first time a person
        // dragged it.
        const glm::vec3 position = ( which.GetValue() == ViewportCameraWrite::Position )
                                        ? glm::vec3( request.Value[0], request.Value[1], request.Value[2] )
                                        : camera->GetPosition();
        const glm::vec3 forward  = ( which.GetValue() == ViewportCameraWrite::Direction )
                                        ? glm::vec3( request.Value[0], request.Value[1], request.Value[2] )
                                        : camera->GetDirection();

        PlaceEditorCamera( *camera, position, forward );

        // INPUT IS NOT DISABLED, and the difference from the capture path is deliberate. `--shot` turns it
        // off because nothing may nudge the camera between the placement and the readback, and there is no
        // one at the keyboard anyway. A channel client may well be driving an editor a person is also
        // sitting at, and taking their camera away for the rest of the session would be a side effect they
        // never asked for and could not undo. The ordering guarantee already covers the capture case: the
        // reply is released only after a frame rendered from this pose.
        return Control::Response::Success( request.Id );
    }

    Control::EditorSnapshot EditorLayer::TakeEditorSnapshot() const
    {
        Control::EditorSnapshot snapshot;

        snapshot.SceneName              = m_MainScene ? m_MainScene->GetSceneName() : std::string();
        snapshot.SceneHasUnsavedChanges = CommandHistory::Get().Revision() != s_SavedRevision;
        snapshot.InPlayMode             = ( m_EditorState == EditorState::Play );

        for ( const Common::UUID& uuid : Core::SelectionManager::GetSelection() )
        {
            Control::EntitySnapshot entity;
            entity.Uuid = uuid.ToString();
            if ( m_MainScene )
            {
                for ( const auto& candidate : m_MainScene->GetAllEntities() )
                {
                    if ( !candidate.HasComponent<ECS::UUIDComponent>() )
                        continue;
                    if ( candidate.GetComponent<ECS::UUIDComponent>().UUID != uuid )
                        continue;
                    if ( candidate.HasComponent<ECS::TagComponent>() )
                        entity.Tag = candidate.GetComponent<ECS::TagComponent>().Tag;
                    break;
                }
            }
            snapshot.Selection.push_back( std::move( entity ) );
        }

        // MOST RECENTLY USED ORDER, which is the order the well lists and Ctrl+Tab walks. Reporting the
        // storage order instead would be a second sequence for the same documents, and a client reading
        // it would predict a different answer from Ctrl+Tab than the editor gives.
        for ( const SubjectId& subject : m_DocumentWell.MostRecentOrder() )
        {
            const ISubjectDocument* document = m_OpenDocuments.Find( subject );
            if ( !document )
                continue;

            Control::DocumentSnapshot entry;
            entry.Name               = DocumentDisplayName( document->GetName() );
            entry.Type               = m_SubjectEditors.TypeName( subject );
            entry.Subject            = subject.ToString();
            entry.HoldsRendererSlot  = document->HoldsRendererSlot();
            entry.ClaimsRendererSlot = document->ClaimsRendererSlot();
            entry.Focused            = ( subject == m_FocusedDocument );

            // The three states, asked of the document itself. Written out as words here rather than
            // exported as enums, because the wire is read by clients that have none of our headers.
            entry.EditModel =
                 ( document->GetEditModel() == ISubjectDocument::EditModel::Staged ) ? "staged" : "write-through";
            entry.HasUnappliedEdits = document->HasUnappliedEdits();
            switch ( document->GetDiskState() )
            {
                case ISubjectDocument::DiskState::Clean:
                    entry.DiskState = "clean";
                    break;
                case ISubjectDocument::DiskState::Dirty:
                    entry.DiskState = "dirty";
                    break;
                case ISubjectDocument::DiskState::Untracked:
                    // NOT "clean". A document that took no snapshot has no evidence about its file, and a
                    // client that read the two as one would report an unsaved edit as saved.
                    entry.DiskState = "untracked";
                    break;
            }

            snapshot.Documents.push_back( std::move( entry ) );
        }

        for ( const ClosedDocument& closed : m_DocumentWell.RecentlyClosed() )
        {
            Control::ClosedDocumentSnapshot entry;
            entry.Name    = closed.DisplayName;
            entry.Type    = m_SubjectEditors.TypeName( closed.Subject );
            entry.Subject = closed.Subject.ToString();
            snapshot.RecentlyClosed.push_back( std::move( entry ) );
        }

        // TOOLS ONLY, and by construction: m_Panels is a PanelRegistry, which cannot hold a document.
        // A client reading this list is reading exactly what the View menu lists.
        for ( const auto& panel : m_Panels )
        {
            Control::PanelSnapshot entry;
            entry.Name = panel->GetName();
            if ( const auto hash = entry.Name.find( "##" ); hash != std::string::npos )
                entry.Name.erase( hash );
            entry.Visible    = panel->GetVisibility();
            entry.Pinned     = panel->Pinned();
            entry.Contextual = panel->IsContextual();
            entry.Relevant   = panel->IsRelevant();
            snapshot.Panels.push_back( std::move( entry ) );
        }

        snapshot.RendererSlotsLive    = Graphic::SceneRenderer::GetLiveRendererCount();
        snapshot.RendererSlotsPending = PendingRendererSlotDemand( m_OpenDocuments.Documents() );
        snapshot.RendererSlotsMax     = EngineContext::kMaxRendererSlots;

        snapshot.LogInfoCount    = LogsPanel::InfoCount();
        snapshot.LogWarningCount = LogsPanel::WarningCount();
        snapshot.LogErrorCount   = LogsPanel::ErrorCount();
        snapshot.LogTail         = LogsPanel::Tail( 40 );

        // 07 §14.2's mode, so a window capture of the overlay can be read with a number beside it.
        {
            const auto& authoring     = Core::ActiveAuthoringContext();
            snapshot.Authoring.Mode   = Core::AuthoringModeName( authoring.Mode() );
            snapshot.Authoring.Holder = authoring.Holder().Describe();
            snapshot.Authoring.Entity =
                 authoring.Entity().IsNull() ? std::string() : authoring.Entity().ToString();
            snapshot.Authoring.SelectedBone = authoring.SelectedBoneIndex();
            snapshot.Authoring.SelectedControl =
                 authoring.SelectedControl() ? static_cast<int>( *authoring.SelectedControl() ) : -1;
            snapshot.Authoring.ShowBoneNames    = authoring.ShowBoneNames();
            snapshot.Authoring.PreviewsBindPose = authoring.PreviewsBindPose();
        }

        snapshot.Quiescence = m_FrameQuiescence;
        return snapshot;
    }

    void EditorLayer::RecordWindowCaptureIfDue()
    {
        // THE CAPTURE IS RECORDED WHILE THE FRAME IS STILL BEING BUILT, and that is not an optimisation.
        //
        // A swapchain image may only be touched between its acquire and its present. The first version of
        // this read it back after the present, in OnFramePresented, and the picture was correct — which is
        // exactly what made it dangerous. Only the validation layer objected: "vkQueueSubmit(): performs a
        // layout transition on presentable VkImage, but the image has not been acquired from
        // VkSwapchainKHR". A capture that quietly breaks the frame loop it was taken to document is worth
        // less than no capture.
        //
        // So the editor asks the gate, before the submit, whether THIS frame is the one the reply waits
        // for — the same question, on the same inputs, that OnFramePresented will answer afterwards.
        if ( !m_ControlInFlight || m_ControlInFlight->Operation != Control::Op::ShotWindow )
            return;
        // THE EFFECT WAIT AND NOT THE READINESS ONE. A shot parked behind the readiness gate has not been
        // taken yet — its `run`-like half is precisely this capture — so recording on the frame that
        // merely proves the editor came up would photograph the boot instead of the thing asked for, and
        // it would still be released as the answer to the request. The gate's two subjects are what keep
        // "the editor is ready" and "the command has landed" from being read as one fact.
        if ( m_ControlGate.Holding() != Control::GateSubject::Effect )
            return;
        if ( !m_ControlGate.WouldDischarge( m_FrameIndex, m_FrameQuiescence ) )
            return;

        auto swapChain = std::dynamic_pointer_cast<Graphic::API::Vulkan::VulkanSwapChain>(
             EngineContext::GetInstance().GetWindow()->GetWindowSwapChain() );
        if ( !swapChain )
        {
            m_ControlCaptureError = "there is no Vulkan swapchain to capture the presented frame from.";
            return;
        }

        if ( const auto recorded = swapChain->RecordFrameCapture(); !recorded )
        {
            m_ControlCaptureError = recorded.GetError();
            return;
        }
        m_ControlCaptureError.clear();
    }

    bool EditorLayer::WriteWindowPng( const std::string& path, std::string& outError )
    {
        // THE WHOLE EDITOR, INTERFACE INCLUDED — which is what WriteViewportPng next door cannot do and
        // never could. That one reads the scene's own final image; ImGui is recorded into the SWAPCHAIN
        // render pass, so no capture this engine took before this existed held a single pixel of a panel,
        // a menu or a dialog. It is why proving anything about the interface meant photographing the
        // window from outside the process, by PID.
        //
        // This is the second half of the capture: the copy was recorded into this frame's command buffer
        // by RecordWindowCaptureIfDue, and the bytes are collected here, once the present that carried it
        // has gone out.
        if ( !m_ControlCaptureError.empty() )
        {
            outError = m_ControlCaptureError;
            m_ControlCaptureError.clear();
            return false;
        }

        auto swapChain = std::dynamic_pointer_cast<Graphic::API::Vulkan::VulkanSwapChain>(
             EngineContext::GetInstance().GetWindow()->GetWindowSwapChain() );
        if ( !swapChain )
        {
            outError = "there is no Vulkan swapchain to collect the captured frame from.";
            return false;
        }

        const std::filesystem::path file = std::filesystem::path( path );
        if ( file.has_parent_path() && !file.parent_path().empty() )
        {
            std::error_code ec;
            std::filesystem::create_directories( file.parent_path(), ec );
            if ( ec && !std::filesystem::exists( file.parent_path() ) )
            {
                outError = "could not create '" + file.parent_path().string() + "': " + ec.message();
                return false;
            }
        }

        // The copy was submitted with this frame; waiting is what makes the staging buffer readable.
        Graphic::Renderer::GetInstance().WaitDeviceIdle();

        uint32_t   width  = 0;
        uint32_t   height = 0;
        const auto pixels = swapChain->TakeCapturedFrameRGBA8( width, height );
        if ( !pixels )
        {
            outError = pixels.GetError();
            return false;
        }

        stbi_flip_vertically_on_write( 0 );
        const bool written = stbi_write_png( path.c_str(), static_cast<int>( width ), static_cast<int>( height ),
                                             4, pixels.GetValue().data(), static_cast<int>( width ) * 4 ) != 0;
        if ( !written )
        {
            outError = "stb_image_write refused to write '" + path + "'.";
            return false;
        }

        LOG_INFO( "[Control] wrote the presented frame (editor and interface) -> {} ({}x{})", path, width,
                  height );
        return true;
    }

    void EditorLayer::BuildSceneSystems( Desert::Core::Scene& scene )
    {
        // The nine collectors that turn components into render data, and their order, live in ONE place
        // now (Engine/Core/SceneRenderCollectors.hpp). They were copied by hand into five call sites, and
        // the sixth — Ю16's render-texture cache — omitted them and got a world that rendered a single
        // flat colour with nothing in the log. The gameplay systems below still belong to the host: each
        // needs a service only the host owns.
        Desert::Core::AddSceneRenderCollectors( scene );
        scene.AddSystem<ECS::AnimationECSSystem>( m_AnimationLibrary.get(), m_AssetManager.get() );
        // AttachmentSystem runs right AFTER animation: weapons-in-hand follow the freshly-posed bone this frame.
        scene.AddSystem<ECS::AttachmentSystem>( &scene );
        // ScriptSystem runs BEFORE physics: scripts set the character's move intent (+ look) which
        // PhysicsECSSystem then executes the same frame.
        scene.AddSystem<ECS::ScriptSystem>( &scene, m_AssetManager.get() );
        scene.AddSystem<ECS::PhysicsECSSystem>( &scene );
        // Maps character movement state (speed/onGround from physics) -> locomotion clip. Kept OUT of physics
        // (mechanism vs behaviour); runs after it so it reads this frame's state.
        scene.AddSystem<ECS::LocomotionSystem>( &scene );
        scene.AddSystem<ECS::AudioECSSystem>( &scene );
    }

    Common::BoolResultStr EditorLayer::UpdateSceneFrame( Desert::Core::Scene&    scene,
                                                         Render::RenderRegistry* registry,
                                                         const Common::Timestep& ts )
    {
        // Editor-only VIEW state (from EditorPreferences, not scene data) pushed per scene before it
        // records this frame: the selection outline, and — since К2 — the debug/show flags that used to be
        // serialized into the level. Both must land BEFORE BeginScene, which is where the renderer hands
        // them on to its systems.
        //
        // Only scenes that reach this function are pushed to, and that is the point: the asset-thumbnail,
        // inspector-preview and photogrammetry renderers own their own SceneRenderer, are never fed here,
        // and therefore keep DebugViewState's all-off defaults. They used to have to remember to switch the
        // grid off by hand on a scene they owned.
        //
        // ONCE PER VIEW, not once per scene. These reach the renderer's systems from BeginScene, and a
        // scene has a LIST of renderers now — pushing only to view 0 left every second viewport with
        // DebugViewState's all-off defaults, i.e. no grid, no collider wireframes, and the machine
        // quality schema defaults instead of this machine's.
        for ( size_t viewIndex = 0; viewIndex < scene.GetViewCount(); ++viewIndex )
        {
            auto* sr = scene.GetViewRenderer( viewIndex );
            if ( sr == nullptr )
                continue;
            const auto& prefs = EditorPreferences::Get();
            sr->SetOutlineSettings( prefs.OutlineColor, prefs.OutlineWidth, prefs.OutlineSmoothness,
                                    prefs.EnableOutline );
            // THE USER'S ANSWER, MINUS WHAT THIS SCENE'S VIEWPORTS ARE HIDING RIGHT NOW. `prefs.DebugView`
            // is what the user chose and what editor.json holds; a viewport MODE (2D UI editing hides the
            // ground grid) suppresses a flag in the COPY that reaches the renderer and never in the store.
            // Before К10 the mode wrote the store directly and every unrelated EditorPreferences::Save()
            // could make the suppression permanent — see Editor/Core/ViewportModes.hpp.
            sr->SetDebugView( ViewportPanel::EffectiveDebugView( prefs.DebugView, scene ) );
            // AND WHAT THIS MACHINE CAN AFFORD, on the same terms and for the same reason: post AA, mesh
            // LOD, the sampler's filter and anisotropy, the cloud tier. It was scene data until К3, so a
            // weak machine could not turn the picture down without editing a file that goes to everybody.
            // The offscreen preview renderers are not fed here either — the inspector preview pushes its
            // own copy with a cheaper cloud tier, and the other two keep the schema defaults.
            sr->SetQuality( Common::Settings::MachineSettings::Get() );
        }

        // BEFORE the scene's frame, not between its phases: the scene opens and closes each view's
        // renderer itself now (Scene::OnUpdate), and nothing may sit between a renderer's open and its
        // close. Today this records nothing into the graph anyway — the editor's injected passes execute
        // inside the renderer's own update — so moving it costs the frame nothing.
        if ( registry )
            registry->Render();

        // BEFORE the systems run, so no system sees an entity whose cell has just left.
        if ( m_WorldStreamer && m_WorldStreamer->Streams( scene ) )
        {
            DESERT_PROFILE_SCOPE( "WorldStreamer::Tick" );
            m_WorldStreamClock += ts.GetSeconds();
            if ( auto streamed = m_WorldStreamer->Tick( m_WorldStreamClock ); !streamed )
            {
                // The world stays as it is now, and Play goes on in it; streaming does not.
                LOG_ERROR( "[Scene] world streaming stopped: {0}", streamed.GetError() );
                Editor::ToastManager::Push( "World streaming stopped — see the log", Editor::ToastLevel::Error );
                m_WorldStreamer.reset();
            }
        }

        {
            DESERT_PROFILE_SCOPE( "Scene::OnUpdate" );
            if ( auto frame = scene.OnUpdate( ts ); !frame )
                return Common::MakeError( frame.GetError() );
        }

        return BOOLSUCCESS;
    }

    void EditorLayer::AddSceneView()
    {
        auto           doc = std::make_unique<SceneDocument>();
        const uint64_t id  = m_SceneViewIds.Next();
        doc->Id            = id;
        // Numbered by the id, not by the current count: with closing implemented, "Scene 3" reappearing as
        // the name of a fourth view after the third was closed would put two different documents under one
        // label across a session, and the log lines below are how a slot leak is read.
        doc->Name = "Scene " + std::to_string( id + 1 ); // the main scene reads as "Scene 1"

        doc->Renderer = std::make_unique<Graphic::SceneRenderer>();
        doc->Scene    = std::make_shared<Desert::Core::Scene>( std::string( doc->Name ), doc->Renderer.get() );
        BuildSceneSystems( *doc->Scene );
        // Reported: AddSceneView is void and the document is already in the well by the time this runs, so
        // there is nothing to hand a failure to. What matters is that the log names the view — a scene that
        // did not initialise renders an empty viewport, which reads as a content problem, not an engine one.
        if ( const auto inited = doc->Scene->Init(); !inited.IsSuccess() )
            LOG_ERROR( "[EditorLayer] scene view '{}' failed to initialise: {}", doc->Name, inited.GetError() );
        doc->Registry = std::make_unique<Render::RenderRegistry>( doc->Scene );

        // Unique ImGui id per viewport — two windows sharing an id would merge into a single dockable window.
        // Keyed on the document id so a closed window's saved imgui.ini entry (position, dock node, size) is
        // never inherited by an unrelated later view.
        const std::string title = doc->Name + "###sceneview" + std::to_string( id );
        auto vp = std::make_unique<Editor::ViewportPanel>( doc->Scene, m_AssetManager.get(), title, id );
        // Captures the ID, never the index. See Editor/Core/SceneViewIdentity.hpp.
        vp->SetOnActivate( [this, id] { SetActiveScene( id ); } );
        vp->GetVisibility() = true;
        doc->Viewport       = vp.get();
        m_Panels.Adopt( std::move( vp ) );

        m_ExtraScenes.emplace_back( std::move( doc ) );
        LOG_INFO( "[Editor] Opened scene view #{} (now {} scenes open, {}/{} renderer slots in use)", id,
                  m_ExtraScenes.size() + 1, Graphic::SceneRenderer::GetLiveRendererCount(),
                  EngineContext::kMaxRendererSlots );
    }

    void EditorLayer::CloseDismissedSceneViews()
    {
        // Collect first, close after: CloseSceneView erases from both m_ExtraScenes and m_Panels, so deciding
        // and mutating in one pass over either would be iterating a container while emptying it.
        std::vector<uint64_t> dismissed;
        for ( const auto& doc : m_ExtraScenes )
            if ( doc->Viewport && !doc->Viewport->GetVisibility() )
                dismissed.push_back( doc->Id );

        for ( const uint64_t id : dismissed )
            CloseSceneView( id );
    }

    void EditorLayer::CloseSceneView( uint64_t id )
    {
        const auto index = IndexOfSceneView(
             m_ExtraScenes, []( const std::unique_ptr<SceneDocument>& doc ) { return doc->Id; }, id );
        if ( !index )
            return; // already closed — a second X on the same window in the same frame, or a stale request

        // Closing the document that is PLAYING ends play mode with it. The snapshot Stop would restore is a
        // snapshot of a scene that is about to cease existing, and OnSceneStop acts on whatever document is
        // active — so leaving the state alone would strand the editor in Play with an Edit-mode scene under
        // it: the Stop button would early-return and never come back up.
        if ( m_EditorState == EditorState::Play && m_ActiveSceneId == id )
        {
            LOG_INFO( "[Editor] Scene view #{} was playing when it was closed — play mode ends with it and "
                      "its snapshot is discarded.",
                      id );
            m_EditorState      = EditorState::Paused;
            m_PendingSceneStop = false;
            m_PlaySnapshot.clear();
            m_WorldStreamer.reset();
        }

        // The editor must not stay bound to a scene that is about to stop existing. Rebinding BEFORE the
        // teardown, not after, so no panel is holding the dying scene when its registry is destroyed.
        if ( const uint64_t next = ActiveSceneViewAfterClose( m_ActiveSceneId, id ); next != m_ActiveSceneId )
            SetActiveScene( next );

        auto&             doc  = m_ExtraScenes[*index];
        const std::string name = doc->Name;

        // EVERY EXTRA ANGLE ON THIS DOCUMENT GOES WITH IT. A viewport opened on a document's scene holds
        // that scene by shared_ptr, so leaving it open would keep a closed document's world alive and
        // rendering — through a RenderRegistry that is about to be destroyed — in a window whose title
        // names a document that no longer exists. Collected first and closed after, because
        // CloseSceneViewport erases from the container this walks.
        {
            std::vector<uint64_t> onThisDocument;
            for ( const auto& view : m_ExtraViewports )
                if ( view->Scene.lock() == doc->Scene )
                    onThisDocument.push_back( view->Id );
            for ( const uint64_t viewportId : onThisDocument )
                CloseSceneViewport( viewportId );
        }

        // The destruction order is the one ~PreviewViewport established and it is not interchangeable: the
        // last submitted frame may still be executing against this document's pipelines, framebuffers and
        // descriptor pools. Idle the device; then drop the PANEL (its UIHelper holds descriptor sets that
        // reference the scene's images); then the registry, which holds render commands built from the
        // scene; then the scene, which owns the passes; and only then the renderer that owns them all —
        // whose destructor is what hands the renderer slot back.
        Graphic::Renderer::GetInstance().WaitDeviceIdle();

        IPanel* panel = doc->Viewport;
        m_Panels.Remove( panel );
        doc->Viewport = nullptr;

        doc->Registry.reset();
        doc->Scene.reset();
        doc->Renderer.reset();
        m_ExtraScenes.erase( m_ExtraScenes.begin() + static_cast<ptrdiff_t>( *index ) );

        // The count is printed, not left to be derived: a surface that fails to return its slot produces no
        // error at all, and this line beside the one in AddSceneView is what makes the leak readable.
        LOG_INFO( "[Editor] Closed scene view #{} '{}' ({} scenes open, {}/{} renderer slots in use)", id, name,
                  m_ExtraScenes.size() + 1, Graphic::SceneRenderer::GetLiveRendererCount(),
                  EngineContext::kMaxRendererSlots );
    }

    void EditorLayer::AddSceneViewport()
    {
        const auto scene = m_MainScene;
        if ( !scene )
        {
            LOG_ERROR( "[Editor] no active scene to open a second viewport on." );
            return;
        }

        auto       renderer  = std::make_unique<Graphic::SceneRenderer>();
        const auto viewIndex = scene->AddView( renderer.get() );
        if ( !viewIndex )
        {
            // Scene::AddView has already said why. The renderer is destroyed on the way out of this
            // scope, which is what gives its slot straight back — a refused view must not cost one.
            LOG_ERROR( "[Editor] '{}' refused a second viewport.", scene->GetSceneName() );
            return;
        }

        auto view   = std::make_unique<SceneViewport>();
        view->Id    = m_SceneViewIds.Next();
        view->Name  = scene->GetSceneName() + " (view " + std::to_string( *viewIndex + 1 ) + ")";
        view->Scene = scene;

        // Unique ImGui id per window, keyed on the id and not the index — a closed window's saved
        // imgui.ini entry must never be inherited by an unrelated later one.
        const std::string title = view->Name + "###sceneviewport" + std::to_string( view->Id );
        auto vp = std::make_unique<Editor::ViewportPanel>( scene, m_AssetManager.get(), title, view->Id,
                                                           renderer.get() );
        vp->GetVisibility() = true;
        view->Viewport      = vp.get();
        m_Panels.Adopt( std::move( vp ) );

        view->Renderer = std::move( renderer );
        m_ExtraViewports.emplace_back( std::move( view ) );

        LOG_INFO( "[Editor] Opened viewport #{} on '{}' ({} view(s) of that world, {}/{} renderer slots in "
                  "use)",
                  m_ExtraViewports.back()->Id, scene->GetSceneName(), scene->GetViewCount(),
                  Graphic::SceneRenderer::GetLiveRendererCount(), EngineContext::kMaxRendererSlots );
    }

    void EditorLayer::BuildViewportGrid()
    {
        const auto scene = m_MainScene;
        if ( !scene )
        {
            LOG_ERROR( "[Editor] no active scene to build a viewport grid on." );
            return;
        }

        // The four panes, in the order the dock split below consumes them. The MAIN viewport is always
        // the perspective one and always the top-left: it is the window every other part of the editor
        // already refers to as "Scene", and moving the user's familiar view into a corner to make room
        // for a new one is exactly the "the editor lost my panel" complaint.
        //
        // Top / Front / Right and not all six axis views: four panes is what the request names, and the
        // remaining three are one combo away in each pane's camera gear.
        static constexpr std::array<ViewportCameraPreset, 3> kExtraAngles = {
             ViewportCameraPreset::Top, ViewportCameraPreset::Front, ViewportCameraPreset::Right };

        // REUSE WHAT IS ALREADY OPEN. Running this twice must not open three more viewports and burn the
        // renderer budget — the second run re-aims the panes it finds and re-docks them, which is also
        // what makes it the "put my viewports back" command.
        std::vector<SceneViewport*> panes;
        for ( const auto& view : m_ExtraViewports )
            if ( view->Scene.lock() == scene && view->Viewport && panes.size() < kExtraAngles.size() )
                panes.push_back( view.get() );

        while ( panes.size() < kExtraAngles.size() )
        {
            const size_t before = m_ExtraViewports.size();
            AddSceneViewport();
            if ( m_ExtraViewports.size() == before )
            {
                // AddSceneViewport has already said why (the view list refused, or there was no scene).
                // REFUSING HALFWAY IS STILL AN ANSWER: the panes that did open are laid out below, and a
                // grid of two is honest where a grid of four that silently became two is not.
                LOG_WARN( "[Editor] the viewport grid stops at {} pane(s): '{}' would not open another.",
                          panes.size() + 1, scene->GetSceneName() );
                break;
            }
            panes.push_back( m_ExtraViewports.back().get() );
        }

        m_PendingViewportGrid.clear();
        m_PendingViewportGrid.push_back( PanelDisplayTitle( "Scene###scene" ) );
        if ( const auto aimed = Editor::ViewportPanel::SetCameraPreset( kPrimarySceneViewId,
                                                                        ViewportCameraPreset::Perspective );
             !aimed )
        {
            LOG_WARN( "[Editor] the grid's perspective pane was not aimed: {}", aimed.GetError() );
        }

        for ( size_t i = 0; i < panes.size(); ++i )
        {
            m_PendingViewportGrid.push_back( PanelDisplayTitle( panes[i]->Viewport->GetName() ) );
            if ( const auto aimed = Editor::ViewportPanel::SetCameraPreset( panes[i]->Id, kExtraAngles[i] );
                 !aimed )
            {
                LOG_WARN( "[Editor] grid pane '{}' was not aimed: {}", panes[i]->Name, aimed.GetError() );
            }
        }

        LOG_INFO( "[Editor] Viewport grid: {} pane(s) on '{}' ({}/{} renderer slots in use); the dock "
                  "split runs on the next frame.",
                  m_PendingViewportGrid.size(), scene->GetSceneName(),
                  Graphic::SceneRenderer::GetLiveRendererCount(), EngineContext::kMaxRendererSlots );
    }

    void EditorLayer::CloseDismissedSceneViewports()
    {
        std::vector<uint64_t> dismissed;
        for ( const auto& view : m_ExtraViewports )
            if ( view->Viewport && !view->Viewport->GetVisibility() )
                dismissed.push_back( view->Id );

        for ( const uint64_t id : dismissed )
            CloseSceneViewport( id );
    }

    void EditorLayer::CloseSceneViewport( uint64_t id )
    {
        const auto index = IndexOfSceneView(
             m_ExtraViewports, []( const std::unique_ptr<SceneViewport>& view ) { return view->Id; }, id );
        if ( !index )
            return; // already closed

        auto&             view = m_ExtraViewports[*index];
        const std::string name = view->Name;

        // Same order as CloseSceneView, minus the scene: idle the device, drop the panel (its UIHelper
        // holds descriptor sets referencing this renderer's images), take the view off the scene so no
        // frame records into it again, and only then destroy the renderer — whose destructor hands the
        // renderer slot back.
        Graphic::Renderer::GetInstance().WaitDeviceIdle();

        m_Panels.Remove( view->Viewport );
        view->Viewport = nullptr;

        if ( const auto scene = view->Scene.lock() )
        {
            // Reported, not asserted: the scene may have been closed first, in which case the view went
            // with it and there is nothing here to remove.
            if ( !scene->RemoveView( view->Renderer.get() ) )
                LOG_WARN( "[Editor] viewport '{}' was not a view of '{}' when it closed.", name,
                          scene->GetSceneName() );
        }
        view->Renderer.reset();
        m_ExtraViewports.erase( m_ExtraViewports.begin() + static_cast<ptrdiff_t>( *index ) );

        LOG_INFO( "[Editor] Closed viewport '{}' ({}/{} renderer slots in use)", name,
                  Graphic::SceneRenderer::GetLiveRendererCount(), EngineContext::kMaxRendererSlots );
    }

    std::vector<EditorLayer::RendererSlotConsumer> EditorLayer::RendererSlotCensus() const
    {
        std::vector<RendererSlotConsumer> census;
        census.push_back( { "main viewport", true } ); // the primary scene's renderer exists for the session

        for ( const auto& doc : m_ExtraScenes )
            census.push_back( { "scene view '" + doc->Name + "'", true } );

        // A second ANGLE holds a slot exactly as a second document does — leaving it out of the census
        // would make the budget's own report disagree with SceneRenderer::GetLiveRendererCount().
        for ( const auto& view : m_ExtraViewports )
            census.push_back( { "viewport '" + view->Name + "'", view->Renderer != nullptr } );

        // The Details preview is a TOOL that happens to own a renderer, so it is found among the panels.
        for ( const auto& panel : m_Panels )
            if ( const auto* details = dynamic_cast<const ScenePropertiesPanel*>( panel.get() ) )
                census.push_back( { "Details preview", details->HoldsRendererSlot() } );

        // The documents are asked of their own owner rather than sifted out of the panel list with a
        // dynamic_cast. That cast was the seam an earlier task closed: it only existed because the two
        // kinds shared a container, and every place that had to write it was a place that could forget to.
        for ( const auto& document : m_OpenDocuments )
        {
            // The VISIBLE half of the name. The census tells a user what to close, and they close a window
            // titled "MP_GreenTint", not one titled "MP_GreenTint###docasset:2:3333333333333333333".
            census.push_back( { m_SubjectEditors.TypeName( document->Subject() ) + " document '" +
                                     DocumentDisplayName( document->GetName() ) + "'",
                                document->HoldsRendererSlot(), document->ClaimsRendererSlot(),
                                document->Subject() } );
        }

        return census;
    }

    std::string EditorLayer::SubjectEntityName( const SubjectId& subject, const char* what ) const
    {
        // WHAT THE WINDOW IS CALLED, not what it is. The subject is the identity; this is the label beside
        // it, and it is resolved ONCE, here, at the moment the document is built — the document itself must
        // not need a scene to know its own name, and an entity renamed afterwards does not become a second
        // window (an asset document behaves the same way; see Control::DocumentSnapshot::Subject).
        std::string name = "Entity";
        if ( m_MainScene )
        {
            if ( const auto entOpt = m_MainScene->FindEntityByID( subject.Owner ) )
                name = entOpt->get().GetComponent<ECS::TagComponent>().Tag;
        }
        return name + " \xc2\xb7 " + what;
    }

    void EditorLayer::ServiceSubjectOpenRequests()
    {
        for ( const SubjectId& subject : Core::SubjectOpenRequests::Drain() )
        {
            // OPEN-OR-FOCUS, keyed by the subject, asked of the one owner of open documents. It does NOT
            // set a visibility flag any more: a document that is open is open, and "focus" is the only
            // thing a second request for the same subject can mean.
            if ( ISubjectDocument* open = m_OpenDocuments.Find( subject ) )
            {
                FocusDocument( open->Subject() );
                continue;
            }

            // Checked BEFORE the slot arithmetic below, so a kind with no editor is reported as the missing
            // editor it is rather than as a resource shortage it had nothing to do with.
            if ( !m_SubjectEditors.HasEditorFor( subject.Type() ) )
            {
                LOG_WARN( "[Editor] Nothing edits subject '{}' — no window opened.", subject.ToString() );
                continue;
            }

            // THE SEVENTH CONSUMER IS REFUSED, OUT LOUD. There are six renderer slots. A document admitted
            // past the cap would not fail — SceneRenderer would hand it slot 0 to share with the main view,
            // and the symptom is two surfaces quietly trading each other's per-frame camera some minutes
            // later, with no error anywhere. So the count is checked here and the census is printed with
            // names, because a bare "no slots" leaves the user with nothing to close.
            //
            // Pending demand is counted separately and it is not pedantry: a document that is open but has
            // not drawn yet holds NO slot and has a claim coming, so the live-renderer count alone would
            // admit a document there is no slot for and discover it a frame later.
            //
            // The counting rule itself lives in SubjectEditorRegistry.hpp, not here: this file is compiled
            // by no suite, and a rule written in it is a rule nothing can assert.
            const uint32_t live    = Graphic::SceneRenderer::GetLiveRendererCount();
            const uint32_t pending = PendingRendererSlotDemand( m_OpenDocuments.Documents() );

            if ( live + pending >= EngineContext::kMaxRendererSlots )
            {
                auto        rows = RendererSlotCensus();
                std::string census;
                for ( const auto& consumer : rows )
                {
                    const char* state = consumer.HoldsSlot ? "holds a slot"
                                        : consumer.ClaimsSlot
                                             ? "no slot right now, but will claim one when it draws"
                                             : "holds no slot and never will (drawn on the CPU) — closing "
                                               "it frees nothing";
                    census += "\n    " + consumer.Name + " — " + state;
                }
                LOG_ERROR( "[Editor] Refusing to open a document for subject '{}': {} of {} renderer slots "
                           "are in use and {} more are already committed. Close one of these first:{}",
                           subject.ToString(), live, EngineContext::kMaxRendererSlots, pending, census );

                // AND THE SAME THING WHERE THE USER IS. The census above has always been written; it went
                // to a log the user was not reading, so a double-click on the seventh document did nothing
                // at all as far as the screen was concerned. The dialog carries the identical rows and, for
                // the ones that are documents, a button that acts on them.
                //
                // The subject is named by its FILE NAME or its ENTITY NAME where one is known: "handle
                // 3333333333333333333" is the log's identifier, not the user's.
                m_OpenRefusal = OpenRefusal{ RefusedSubjectName( subject ), m_SubjectEditors.TypeName( subject ),
                                             live, pending, std::move( rows ) };
                m_OpenRefusalPending = true;
                continue;
            }

            auto document = m_SubjectEditors.Create( subject );
            if ( !document )
                continue; // the registry already said why

            const std::string name = document->GetName();
            // Asked BEFORE the move, and counted rather than assumed: a document that will never claim a
            // slot adds nothing to the committed total, and "pending + 1" would have reported every cloud
            // document as a claim on a slot it does not take. See ISubjectDocument::ClaimsRendererSlot.
            const uint32_t committed = pending + ( document->ClaimsRendererSlot() ? 1u : 0u );

            // ASKED THE MOMENT IT IS BUILT, and not left to the sweep a frame later. A document whose
            // subject was already gone would otherwise appear for one frame and vanish, which reads as a
            // window that failed rather than as a thing that is not there. The factory has just resolved
            // the subject, so this costs one more resolution and answers before anything is on screen.
            if ( !document->IsSubjectAlive() )
            {
                LOG_WARN( "[Editor] Refusing to open a document for subject '{}': the {} it names does not "
                          "exist (deleted, or in a scene that is no longer open).",
                          subject.ToString(), m_SubjectEditors.TypeName( subject ) );
                continue;
            }

            // THROUGH THE OWNER'S OWN DOOR, which is what refuses a duplicate rather than appending one.
            // The open-or-focus above already answered for the route this function serves; the refusal
            // here is for the routes that do not exist yet, and it is the container's rule rather than a
            // habit every future call site has to inherit (Editor/Core/OpenDocuments.hpp).
            const DocumentOpenResult opened = m_OpenDocuments.Open( std::move( document ) );
            if ( opened.Outcome == DocumentOpenOutcome::Refused )
            {
                LOG_ERROR( "[Editor] The '{}' editor built a document that names nothing for subject '{}' — "
                           "no window was opened.",
                           m_SubjectEditors.TypeName( subject ), subject.ToString() );
                continue;
            }
            if ( opened.Outcome == DocumentOpenOutcome::AlreadyOpen )
            {
                // Cannot normally happen — the open-or-focus at the top of this loop catches it. Said out
                // loud rather than swallowed, because reaching this line means two requests for one subject
                // survived that check, and the container's refusal is what stops it becoming two documents.
                LOG_WARN( "[Editor] A second document for subject '{}' was built and discarded; the one "
                          "already open was focused instead.",
                          subject.ToString() );
            }

            m_DocumentWell.Touch( subject );
            m_FocusPanel      = name; // brings the new window forward in the document well
            m_FocusedDocument = subject;
            LOG_INFO( "[Editor] Opened a '{}' document '{}' ({} open, {}/{} renderer slots in use, {} "
                      "committed).",
                      m_SubjectEditors.TypeName( subject ), name, m_OpenDocuments.Count(),
                      Graphic::SceneRenderer::GetLiveRendererCount(), EngineContext::kMaxRendererSlots,
                      committed );
        }
    }

    std::string EditorLayer::RefusedSubjectName( const SubjectId& subject ) const
    {
        if ( subject.Domain == SubjectDomain::Asset && m_AssetManager )
        {
            // The UNTYPED metadata lookup, deliberately: the refusal happens before any editor for this
            // type is consulted, so all that is known about the subject is that it is an asset — and a
            // typed lookup would have to guess which class to ask for. Metadata carries no cast, so there
            // is nothing here that could answer with a stranger.
            if ( const auto* metadata =
                      m_AssetManager->FindMetadataByHandle( Assets::AssetHandle( subject.Owner ) ) )
                return metadata->Filepath.stem().string();
        }
        if ( subject.Domain == SubjectDomain::EntityComponent && m_MainScene )
        {
            if ( const auto entOpt = m_MainScene->FindEntityByID( subject.Owner ) )
                return entOpt->get().GetComponent<ECS::TagComponent>().Tag;
        }
        return "this subject";
    }

    void EditorLayer::RequestDocumentClose( const SubjectId& subject, std::string reason )
    {
        if ( !m_OpenDocuments.Find( subject ) )
            return; // already gone, or never open — a second x on one window in one frame is not an error

        const auto queued =
             std::find_if( m_DocumentsToClose.begin(), m_DocumentsToClose.end(),
                           [&subject]( const PendingDocumentClose& p ) { return p.Subject == subject; } );
        if ( queued == m_DocumentsToClose.end() )
            m_DocumentsToClose.push_back( PendingDocumentClose{ subject, std::move( reason ) } );
    }

    void EditorLayer::RequestCloseAllDocuments()
    {
        // Collected first and requested after, rather than requested while iterating: RequestDocumentClose
        // reads the well, and a range-for over a container something else is being asked about is the kind
        // of thing that survives review and then does not survive a refactor.
        std::vector<SubjectId> subjects;
        subjects.reserve( m_OpenDocuments.Count() );
        for ( const auto& document : m_OpenDocuments )
            subjects.push_back( document->Subject() );

        for ( const SubjectId& subject : subjects )
            RequestDocumentClose( subject, "Close All Documents" );
    }

    void EditorLayer::CloseDocumentsWhoseSubjectIsGone()
    {
        // ── A DOCUMENT CLOSES WITH ITS SUBJECT ────────────────────────────────────────────────────────
        //
        // The owner's decision, and the counterpart to the one he refused: a document is NOT closed when
        // it loses the focus, because a layout that rearranges itself reads as an editor that lost your
        // panel. It IS closed when the thing it edits stops existing — delete the entity, remove the
        // component, delete the asset, close the scene — because the alternative is a window editing
        // nothing, and every one of its controls then writes into a resolution that returns null.
        //
        // WITH A NAMED REASON. Three different causes now queue a close, and a user whose window vanished
        // is owed which one it was; ServiceDocumentCloses prints it.
        //
        // ASKED EVERY FRAME, and it has to be. There are FOUR ways a subject dies and no single event
        // covers them: an asset leaves the manager, an entity is destroyed, a COMPONENT is removed from an
        // entity that survives, or the scene a document was opened over is closed. A subscription to one
        // of the four would be worse than none, because the other three would then look handled. The cost
        // is one resolution per open document per frame — the same resolution each document already
        // performs to draw itself, and there are rarely more than six of them.
        std::vector<SubjectId> dead;
        for ( const auto& document : m_OpenDocuments )
            if ( !document->IsSubjectAlive() )
                dead.push_back( document->Subject() );

        for ( const SubjectId& subject : dead )
        {
            RequestDocumentClose( subject, "the " + m_SubjectEditors.TypeName( subject ) +
                                                " it was editing no longer exists" );
        }
    }

    void EditorLayer::ReleaseSlotsOfHiddenDocuments()
    {
        // ── THE SLOT GOES WHEN NOBODY IS LOOKING; THE WINDOW STAYS ────────────────────────────────────
        //
        // Four documents docked as tabs in one node show one tab. The other three were rendering previews
        // nobody could see and holding three of the six renderer slots while they did it, so the fifth
        // document the user opened was refused over resources being spent on hidden windows.
        //
        // EVERY VIEW REPORTS INTO ONE COUNT, and the count lives on the owner (OpenDocuments::NoteDrawn /
        // EndFrame) rather than in this file. It used to be a map written only by the document well's draw
        // loop, which was the whole truth while the well was the only thing that could draw a document —
        // the Clouds window is a second one, and a material shown only in THAT window would otherwise have
        // been counted hidden and had its preview renderer taken away under a pane somebody was using.
        //
        // Called from ServiceDocumentCloses so it runs behind the SAME device-idle wait a close uses —
        // releasing a PreviewViewport destroys a Scene and a SceneRenderer, and the last submitted frame
        // may still be executing against them.
        for ( const auto& document : m_OpenDocuments )
        {
            const uint32_t undrawn = m_OpenDocuments.FramesUndrawn( document->Subject() );
            if ( undrawn < kFramesHiddenBeforeSlotRelease )
                continue;
            if ( !document->HoldsRendererSlot() )
                continue;

            document->ReleaseRendererSlot();

            // VERIFIED, NOT ASSUMED. ReleaseRendererSlot's contract is that HoldsRendererSlot answers false
            // afterwards; a document that inherited the empty default while genuinely holding a slot would
            // otherwise keep it for ever and the census would go on blaming a window the user cannot fix.
            if ( document->HoldsRendererSlot() )
            {
                LOG_ERROR( "[Editor] '{}' was asked to release its renderer slot after {} hidden frames and "
                           "still holds one. ReleaseRendererSlot must make HoldsRendererSlot false — see "
                           "ISubjectDocument.",
                           DocumentDisplayName( document->GetName() ), undrawn );
                continue;
            }

            LOG_INFO( "[Editor] '{}' gave its renderer slot back after {} frames off screen ({}/{} in use). "
                      "It is rebuilt on the first frame the window is drawn again.",
                      DocumentDisplayName( document->GetName() ), undrawn,
                      Graphic::SceneRenderer::GetLiveRendererCount(), EngineContext::kMaxRendererSlots );
        }
    }

    void EditorLayer::ServiceDocumentCloses()
    {
        // The hidden-document sweep shares this function's device-idle wait, so the wait is taken when
        // either has work. Two waits in one frame would be two full pipeline drains for one frame's worth
        // of teardown.
        bool releasePending = false;
        for ( const auto& document : m_OpenDocuments )
        {
            if ( m_OpenDocuments.FramesUndrawn( document->Subject() ) >= kFramesHiddenBeforeSlotRelease &&
                 document->HoldsRendererSlot() )
            {
                releasePending = true;
                break;
            }
        }

        if ( m_DocumentsToClose.empty() && !releasePending )
            return;

        // ONE device-idle wait for the whole batch. Destroying a document destroys its PreviewViewport, and
        // with it the scene, the renderer and the renderer slot; the last submitted frame may still be
        // executing against that renderer's pipelines, framebuffers and descriptor pools. The ordering is
        // the one ~PreviewViewport and CloseSceneView both established, not a precaution invented here.
        Graphic::Renderer::GetInstance().WaitDeviceIdle();

        ReleaseSlotsOfHiddenDocuments();

        for ( const PendingDocumentClose& pending : m_DocumentsToClose )
        {
            // Released, then destroyed HERE. The owner hands ownership back rather than dropping the object
            // itself, because it is this function that knows the device is idle — see OpenDocuments.
            std::unique_ptr<ISubjectDocument> closed = m_OpenDocuments.Release( pending.Subject );
            if ( !closed )
                continue;

            // THE VIEWS ARE TOLD, and told BEFORE the object dies: NoteClosed reads the display name off
            // it. A view cannot discover a departure without keeping a second copy of the open set, which
            // is the duplicated state the ownership split removes.
            m_DocumentWell.NoteClosed( *closed );

            const std::string name = closed->GetName();
            m_OpenDocuments.ForgetDrawHistory( pending.Subject );
            if ( m_FocusedDocument == pending.Subject )
                m_FocusedDocument = SubjectId{};

            closed.reset();

            // The REASON is printed, and it is why this line takes one. "Closed document 'Hero'" leaves a
            // user who did not close it with nothing to go on; "because the AnimationComponent it was
            // editing no longer exists" is the whole answer.
            //
            // The slot count is printed rather than derived for a different reason: a document that failed
            // to return its slot produces no error at all, and this line beside the one in
            // ServiceSubjectOpenRequests is what makes the leak readable.
            LOG_INFO( "[Editor] Closed document '{}' — {} ({} open, {}/{} renderer slots in use after "
                      "release).",
                      DocumentDisplayName( name ), pending.Reason, m_OpenDocuments.Count(),
                      Graphic::SceneRenderer::GetLiveRendererCount(), EngineContext::kMaxRendererSlots );
        }

        m_DocumentsToClose.clear();
    }

    void EditorLayer::FocusDocument( const SubjectId& subject )
    {
        ISubjectDocument* document = m_OpenDocuments.Find( subject );
        if ( !document )
            return;

        m_DocumentWell.Touch( subject );
        m_FocusedDocument = subject;
        m_FocusPanel      = document->GetName(); // brings it forward in whatever dock it lives
    }

    void EditorLayer::CycleDocuments()
    {
        const auto next = m_DocumentWell.NextMostRecent( m_FocusedDocument );
        if ( !next )
            return;

        ISubjectDocument* document = m_OpenDocuments.Find( *next );
        if ( !document )
            return;

        // Focus WITHOUT touching the ring. Committing the new order on every press would make the second
        // Ctrl+Tab return to where the first started, so the order is committed when Ctrl is released —
        // see m_CyclingDocuments in OnUIRender.
        m_FocusedDocument  = *next;
        m_FocusPanel       = document->GetName();
        m_CyclingDocuments = true;
    }

    void EditorLayer::SetActiveScene( uint64_t id )
    {
        if ( id == m_ActiveSceneId )
            return;

        const auto index = IndexOfSceneView(
             m_ExtraScenes, []( const std::unique_ptr<SceneDocument>& doc ) { return doc->Id; }, id );
        if ( id != kPrimarySceneViewId && !index )
        {
            // NAMED rather than ignored. An id that resolves to nothing means a viewport outlived its
            // document, which is a lifetime bug in this file — and the whole reason activation is keyed on an
            // id is that this case can be SEEN. An index would have silently activated a neighbour.
            LOG_ERROR( "[Editor] Scene view #{} asked to become active but no such document is open — the "
                       "active scene is unchanged ('{}').",
                       id, m_MainScene ? m_MainScene->GetSceneName() : "<none>" );
            return;
        }

        m_ActiveSceneId = id;
        m_MainScene     = index ? m_ExtraScenes[*index]->Scene : m_PrimaryScene;

        // Structural undo/redo context + the scene-bound editing panels follow the active document, so the
        // Outliner / Details / Settings / Particle editor all show whichever viewport you are working in.
        Commands::SetContext( m_MainScene.get(), m_AssetManager.get() );
        for ( auto& panel : m_Panels )
            panel->SetScene( m_MainScene );
        // Documents follow the active scene too. The Cloud Layout document READS the focused scene's cloud
        // layer for its preview numbers — the scene is an input, never a second subject — and it stopped
        // following it the moment documents left the panel list, which is exactly the "a middle link drops a
        // property" shape this codebase has paid for seven times.
        for ( auto& document : m_OpenDocuments )
            document->SetScene( m_MainScene );

        // Selection is per-scene (entity UUIDs belong to one registry) — don't carry a stale one across.
        Core::SelectionManager::ClearSelection();

        LOG_INFO( "[Editor] Active scene -> '{}' (view #{})", m_MainScene->GetSceneName(), id );
    }

    Common::BoolResultStr EditorLayer::OnUIRender()
    {
        m_ImGuiLayer->Begin();

        // ImGuizmo is a single global per-frame state — begin it ONCE here, before any panel issues a
        // Manipulate(). The viewport's object gizmo relies on this.
        ImGuizmo::BeginFrame();

        SyncWindowTitle();

        // LOADING FRAMES DRAW NOTHING. They go to a window that is still hidden — the splash is what a
        // person sees until the start is over — and there is no dockspace or panel to draw yet (the
        // viewport panel would touch the not-yet-rendered scene image). The fullscreen ImGui overlay that
        // stood here was replaced by the splash, and deleted with the same change.
        if ( StartupLoading() || ContentSettling() )
        {
            m_ImGuiLayer->End();
            return BOOLSUCCESS;
        }
        // NOT YET REAL WHILE A SCENE LOAD IS STILL QUEUED. The frame right after the last stage draws the
        // editor over an empty scene — the queued load runs in the NEXT OnUpdate and only then starts the
        // settle wait — and revealing on it showed the window 3 s before the content had settled
        // (measured on the first run of this change: "on screen" logged before "[Content] settled").
        if ( !m_SceneLoadRequested )
            m_RealFrameDrawn = true;

        // ---- Global editing shortcuts ----
        // Edit mode only (Play discards its changes on Stop anyway) and never while a text field owns the
        // keyboard. Runs at frame start, before any panel iterates the scene.
        {
            ImGuiIO&   io       = ::ImGui::GetIO();
            const bool editMode = m_MainScene->GetState() == ::Desert::Core::Scene::SceneState::Edit;
            if ( editMode && !io.WantTextInput && io.KeyCtrl )
            {
                if ( ::ImGui::IsKeyPressed( ImGuiKey_Z, false ) )
                {
                    if ( io.KeyShift )
                        CommandHistory::Get().Redo();
                    else
                        CommandHistory::Get().Undo();
                }
                if ( ::ImGui::IsKeyPressed( ImGuiKey_Y, false ) )
                    CommandHistory::Get().Redo();

                if ( ::ImGui::IsKeyPressed( ImGuiKey_D, false ) )
                {
                    if ( Core::SelectionManager::Count() > 0 )
                        if ( auto dups = Commands::DuplicateEntities( Core::SelectionManager::GetSelection() );
                             !dups.empty() )
                            Core::SelectionManager::SetSelection( std::move( dups ) );
                }

                if ( ::ImGui::IsKeyPressed( ImGuiKey_C, false ) && Core::SelectionManager::Count() > 0 )
                    Commands::CopySelectionToClipboard( Core::SelectionManager::GetSelection() );
                if ( ::ImGui::IsKeyPressed( ImGuiKey_V, false ) )
                    if ( auto pasted = Commands::PasteClipboard(); !pasted.empty() )
                        Core::SelectionManager::SetSelection( std::move( pasted ) );

                if ( ::ImGui::IsKeyPressed( ImGuiKey_N, false ) )
                    m_NewSceneRequested = true; // Ctrl+N -> fresh empty scene (deferred, see OnUpdate)

                if ( ::ImGui::IsKeyPressed( ImGuiKey_S, false ) )
                {
                    // Deliberately discarded HERE and only here: Ctrl+S destroys nothing, so there is
                    // no next step to gate. SaveOpenScene has already put the star back on and told the
                    // user why if the write failed.
                    (void)SaveOpenScene();
                }
            }

            // Command palette (Ctrl+P) — works in both edit and play modes, and even over a text field
            // so it stays reachable; the palette grabs the keyboard once open.
            if ( io.KeyCtrl && !io.KeyShift && ::ImGui::IsKeyPressed( ImGuiKey_P, false ) )
                m_CommandPalette.Open();

            // CTRL+TAB THROUGH THE DOCUMENTS, most recently used first. This is what makes ten open
            // documents bearable: past about six the tab you want is off the end of the strip, and the
            // keyboard is the only route to it that does not involve reading a list first.
            //
            // Outside the edit-mode guard on purpose — switching document is not an edit — but not over a
            // text field, where Tab belongs to the field.
            //
            // ImGui BINDS Ctrl+Tab ITSELF (NavUpdateWindowing, enabled by NavEnableKeyboard) and it runs in
            // NewFrame, before this layer draws — so both would fire on one press: ImGui's window-ring
            // overlay AND this. The overlay is cancelled here rather than the key being fought for, and
            // ONLY when there was a document to switch to: with no documents open, Ctrl+Tab keeps ImGui's
            // ordinary window ring, which is a reasonable thing for it to do and not ours to remove.
            if ( io.KeyCtrl && !io.WantTextInput && ::ImGui::IsKeyPressed( ImGuiKey_Tab, false ) )
            {
                const SubjectId before = m_FocusedDocument;
                CycleDocuments();
                if ( m_FocusedDocument != before )
                    ::ImGui::GetCurrentContext()->NavWindowingTarget = nullptr;
            }

            // The ring is committed when Ctrl comes back up, not on each press: see CycleDocuments.
            if ( m_CyclingDocuments && !io.KeyCtrl )
            {
                m_CyclingDocuments = false;
                m_DocumentWell.Touch( m_FocusedDocument );
            }
        }

        static bool               dockspaceOpen  = true;
        static bool               opt_fullscreen = true;
        static ImGuiDockNodeFlags dockspace_flags =
             ImGuiDockNodeFlags_NoWindowMenuButton | ImGuiDockNodeFlags_NoCloseButton;

        // We are using the ImGuiWindowFlags_NoDocking flag to make the parent window not dockable into,
        // because it would be confusing to have two docking targets within each others.
        ImGuiWindowFlags window_flags = ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoDocking;

        // Menu Bar
        ::ImGui::PushStyleVar( ImGuiStyleVar_WindowBorderSize, 0.0f );
        DrawMenuBar();
        ::ImGui::PopStyleVar();

        if ( opt_fullscreen )
        {
            const ImGuiViewport* viewport = ::ImGui::GetMainViewport();

            auto pos     = viewport->Pos;
            auto size    = viewport->Size;
            bool menuBar = true;
            if ( menuBar )
            {
                const float infoBarSize = ::ImGui::GetFrameHeight();
                pos.y += infoBarSize;
                size.y -= infoBarSize;
            }

            ::ImGui::SetNextWindowPos( pos );
            ::ImGui::SetNextWindowSize( size );
            ::ImGui::SetNextWindowViewport( viewport->ID );

            ::ImGui::PushStyleVar( ImGuiStyleVar_WindowRounding, 0.0f );
            ::ImGui::PushStyleVar( ImGuiStyleVar_WindowBorderSize, 0.0f );
            window_flags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                            ImGuiWindowFlags_NoMove;
            window_flags |= ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;
        }

        // When using ImGuiDockNodeFlags_PassthruCentralNode, DockSpace() will render our background
        // and handle the pass-thru hole, so we ask Begin() to not render a background.
        if ( dockspace_flags & ImGuiDockNodeFlags_DockSpace )
            window_flags |= ImGuiWindowFlags_NoBackground;

        // Important: note that we proceed even if Begin() returns false (aka window is collapsed).
        // This is because we want to keep our DockSpace() active. If a DockSpace() is inactive,
        // all active windows docked into it will lose their parent and become undocked.
        // We cannot preserve the docking relationship between an active window and an inactive docking, otherwise
        // any change of dockspace/settings would lead to windows being stuck in limbo and never being visible.
        ::ImGui::PushStyleVar( ImGuiStyleVar_WindowPadding, ImVec2( 0.0f, 0.0f ) );
        ::ImGui::Begin( "DockSpace Demo", &dockspaceOpen, window_flags );
        ::ImGui::PopStyleVar();

        if ( opt_fullscreen )
            ::ImGui::PopStyleVar( 2 );

        // Toolbar strip FIRST so it reserves its height at the top; the DockSpace below then fills the
        // remaining area (drawing it after a full-height DockSpace(0,0) would push the bar off-screen).
        DrawToolbar();

        // Submit the DockSpace
        ImGuiIO& io = ::ImGui::GetIO();

        if ( io.ConfigFlags & ImGuiConfigFlags_DockingEnable )
        {
            // Reserve the bottom status-bar height so the DockSpace fills only the area between the toolbar
            // and the status bar (a full-height DockSpace(0,0) would sit under the status bar).
            const float statusBarHeight = ::ImGui::GetFrameHeight() + 4.0f;
            ImVec2      dockSize        = ::ImGui::GetContentRegionAvail();
            dockSize.y                  = ( dockSize.y > statusBarHeight ) ? dockSize.y - statusBarHeight : 0.0f;

            ImGuiID dockspace_id = ::ImGui::GetID( "MyDockSpace" );

            // One-time auto-relayout: when the default layout's window IDs change (panel-title icons add a
            // ### suffix, changing every window's ImGui ID), old imgui.ini bindings stop matching and panels
            // scatter. Bump kDockLayoutVersion to force a single clean rebuild for everyone, then persist it.
            // 3: the centre is split and documents get a node of their own (layout option B.1).
            constexpr int kDockLayoutVersion = 3;
            if ( EditorPreferences::Get().DockLayoutVersion < kDockLayoutVersion )
            {
                // SAID OUT LOUD. Every existing imgui.ini is rebuilt once here, and a layout that changes
                // in silence is read as the editor having lost the user's panels — which is the same
                // complaint an area that collapses on its own produces, and the reason B.1 does not
                // collapse. One line naming the old and new versions is the difference between "my layout
                // was reset by the update" and "my layout is gone".
                LOG_INFO( "[Editor] Docking layout rebuilt once: saved layout is version {}, this build lays "
                          "out version {} (the centre column now holds the level on the left and a Documents "
                          "area on the right). Your named layouts under View -> Layouts are untouched.",
                          EditorPreferences::Get().DockLayoutVersion, kDockLayoutVersion );

                m_ResetDefaultLayout = true;

                // SaveMigrated, NOT Save: nothing the user did triggered this write. It fires on the
                // first frame after an update, because a stored layout version is being raised to the
                // one this build lays out — the same shape as the metre-era grid snap Load() raises, and
                // the same reason it must be written back (a version that did not persist would rebuild
                // the layout on every launch). Save() means "the user changed a setting" and would have
                // logged this one as if they had.
                const std::string layoutVersions = std::to_string( EditorPreferences::Get().DockLayoutVersion ) +
                                                   " -> " + std::to_string( kDockLayoutVersion );

                EditorPreferences::Get().DockLayoutVersion = kDockLayoutVersion;
                EditorPreferences::SaveMigrated( "docking layout version " + layoutVersions );
            }

            // First run (nothing saved in imgui.ini for this dockspace): lay the panels
            // out into a sensible default instead of leaving them floating in a pile.
            // Checked BEFORE DockSpace() — the call itself creates the node. "Reset to Default
            // Layout" (View -> Layouts) forces the same rebuild on demand.
            const bool buildDefaultLayout =
                 ::ImGui::DockBuilderGetNode( dockspace_id ) == nullptr || m_ResetDefaultLayout;
            m_ResetDefaultLayout = false;

            ::ImGui::DockSpace( dockspace_id, dockSize, dockspace_flags );

            if ( buildDefaultLayout )
            {
                ::ImGui::DockBuilderRemoveNode( dockspace_id );
                ::ImGui::DockBuilderAddNode( dockspace_id, dockspace_flags | ImGuiDockNodeFlags_DockSpace );
                ::ImGui::DockBuilderSetNodeSize( dockspace_id, ( dockSize.x > 0 && dockSize.y > 0 )
                                                                    ? dockSize
                                                                    : ::ImGui::GetMainViewport()->Size );

                //  ┌───────────┬────────────────┬───────────┬──────────────┐
                //  │ Scene     │                │           │ Details      │
                //  │ Outliner  │ Scene(viewport)│ Documents ├──────────────┤
                //  ├───────────┤                │           │ SceneSettings│
                //  │Collections├────────────────┴───────────┤ / Profiler   │
                //  │           │ Assets / Logs              │ / Foliage    │
                //  └───────────┴────────────────────────────┴──────────────┘
                //
                // THE DOCUMENT AREA IS A NODE, NOT A SET OF FLOATING WINDOWS (option B.1). The level never
                // leaves the screen: change a roughness in a material document and the crate in the viewport
                // beside it re-renders. It is paid for out of the centre's width permanently, whether or not
                // anything is open, and that permanence is the feature — an area that appeared and vanished
                // with the last document would resize the viewport under the user's cursor, which is what
                // people report as "the editor lost my panel". The splitter between the two is draggable
                // like every other, so a session that wants the width back can take it.
                ImGuiID center = dockspace_id;
                ImGuiID right  = ::ImGui::DockBuilderSplitNode( center, ImGuiDir_Right, 0.20f, nullptr, &center );
                ImGuiID left   = ::ImGui::DockBuilderSplitNode( center, ImGuiDir_Left, 0.22f, nullptr, &center );
                ImGuiID bottom = ::ImGui::DockBuilderSplitNode( center, ImGuiDir_Down, 0.28f, nullptr, &center );
                m_BottomDockId = bottom; // remembered so the drawer can be collapsed/restored later
                // Split AFTER the bottom drawer, so Assets/Logs still span the whole centre rather than
                // only the level's half of it.
                ImGuiID documents =
                     ::ImGui::DockBuilderSplitNode( center, ImGuiDir_Right, 0.44f, nullptr, &center );
                ImGuiID leftBottom = ::ImGui::DockBuilderSplitNode( left, ImGuiDir_Down, 0.40f, nullptr, &left );
                ImGuiID rightBottom =
                     ::ImGui::DockBuilderSplitNode( right, ImGuiDir_Down, 0.50f, nullptr, &right );

                // Panels routed through the central Begin carry an icon (a ### suffix), so dock them by the
                // SAME composed title — otherwise the icon-changed ImGui ID wouldn't match this assignment.
                // Non-panel windows (Profiler / Foliage / Shader Code) self-Begin with plain names.
                ::ImGui::DockBuilderDockWindow( PanelDisplayTitle( "Scene###scene" ).c_str(), center );
                ::ImGui::DockBuilderDockWindow( PanelDisplayTitle( "Scene Outliner" ).c_str(), left );
                ::ImGui::DockBuilderDockWindow( PanelDisplayTitle( "Collections" ).c_str(), leftBottom );
                ::ImGui::DockBuilderDockWindow( PanelDisplayTitle( "Details" ).c_str(), right );
                ::ImGui::DockBuilderDockWindow( PanelDisplayTitle( "Scene Settings" ).c_str(), rightBottom );
                ::ImGui::DockBuilderDockWindow( "Profiler", rightBottom );
                ::ImGui::DockBuilderDockWindow( "Foliage##FoliagePanel", rightBottom );
                ::ImGui::DockBuilderDockWindow( PanelDisplayTitle( "Assets" ).c_str(), bottom );
                ::ImGui::DockBuilderDockWindow( PanelDisplayTitle( "Logs" ).c_str(), bottom );
                ::ImGui::DockBuilderDockWindow( "Shader Code", bottom );

                // Contextual tools (IPanel::IsContextual) get a home too, so the one that opens itself
                // lands where its work belongs instead of floating over the scene: timelines along the
                // bottom next to Assets/Logs, authoring palettes on the right beside Details.
                ::ImGui::DockBuilderDockWindow( PanelDisplayTitle( "Anim Layers" ).c_str(), bottom );
                // No line for "Anim Graph", "Particle Editor", "UI Editor" or "Sequencer": they are
                // documents, and a document does not have a fixed home in the layout — it docks into the
                // document well beside the others (DrawDocuments sets the dock id), which is the whole point
                // of the well existing. A line here would also name a window that no longer exists under
                // that title: a document's ImGui id is "###doc<subject>", so it could never have matched.
                ::ImGui::DockBuilderDockWindow( PanelDisplayTitle( "Modeling" ).c_str(), left );
                ::ImGui::DockBuilderDockWindow( PanelDisplayTitle( "Landscape" ).c_str(), left );

                // The well itself. It is what makes the document node FINDABLE: a dock node with nothing in
                // it is not drawn at all, so without a permanent occupant the area would exist in the
                // layout and be invisible on screen the whole time no document was open. It is also where
                // every document reads its dock id from at runtime — see DrawDocumentWell.
                ::ImGui::DockBuilderDockWindow( kDocumentWellWindow, documents );

                ::ImGui::DockBuilderFinish( dockspace_id );
            }

            // ── THE FOUR-UP GRID, APPLIED ─────────────────────────────────────────────────────────
            //
            // It splits THE NODE THE MAIN VIEWPORT IS IN, not the whole dockspace: the Outliner, Details
            // and the bottom drawer keep their places, and the grid costs exactly the pixels the single
            // viewport had. That is the difference between "a layout command" and "Reset to Default
            // Layout with four viewports in it", and it is why this is not folded into the block above.
            //
            // A FLOATING main viewport has no node to split (DockId 0); the dockspace is the honest
            // fallback, and it is said out loud because the result then displaces the other panels.
            if ( !m_PendingViewportGrid.empty() )
            {
                ImGuiID node = 0;
                if ( const ::ImGuiWindow* win = ::ImGui::FindWindowByName( m_PendingViewportGrid[0].c_str() ) )
                    node = win->DockId;
                if ( node == 0 || ::ImGui::DockBuilderGetNode( node ) == nullptr )
                {
                    LOG_WARN( "[Editor] the main viewport is not docked; the grid takes the whole "
                              "dockspace, so other panels move." );
                    node = dockspace_id;
                }

                // Quarters, in the order BuildViewportGrid filled the list: top-left, top-right,
                // bottom-left, bottom-right. A list shorter than four (the slot budget refused a pane)
                // simply leaves that quarter to its neighbours, which is what DockBuilder does with an
                // empty node.
                ImGuiID topLeft     = node;
                ImGuiID topRight    = ::ImGui::DockBuilderSplitNode( topLeft, ImGuiDir_Right, 0.5f, nullptr,
                                                                     &topLeft );
                ImGuiID bottomLeft  = ::ImGui::DockBuilderSplitNode( topLeft, ImGuiDir_Down, 0.5f, nullptr,
                                                                     &topLeft );
                ImGuiID bottomRight = ::ImGui::DockBuilderSplitNode( topRight, ImGuiDir_Down, 0.5f, nullptr,
                                                                     &topRight );

                const ImGuiID quarters[4] = { topLeft, topRight, bottomLeft, bottomRight };
                for ( size_t i = 0; i < m_PendingViewportGrid.size() && i < 4; ++i )
                    ::ImGui::DockBuilderDockWindow( m_PendingViewportGrid[i].c_str(), quarters[i] );

                ::ImGui::DockBuilderFinish( dockspace_id );
                m_PendingViewportGrid.clear();
            }
        }

        // THE TOOLS. The document loop is DrawDocuments, below, and the two are separate for the reason the
        // whole task exists: a tool passes &GetVisibility() to Begin, which is right for a setting the user
        // keeps, and a document must not — its false would be read as "destroy this window".
        //
        // The cascade this loop used to carry for documents is gone with them: a document is DOCKED into the
        // well now, so there is no floating window to step down-right from the last one.
        for ( const auto& panel : m_Panels )
        {
            if ( !panel->GetVisibility() )
            {
                continue;
            }

            namespace ImGui = ::ImGui;
            // One padding rule for the whole editor, declared by the panel (the viewport asks for zero).
            // The panel states it as a glm::vec2 -- IPanel.hpp must not name the toolkit -- and this is
            // the line that draws, so this is where it becomes an ImVec2.
            const glm::vec2 padding = panel->GetWindowPadding();
            ImGui::PushStyleVar( ImGuiStyleVar_WindowPadding, ImVec2( padding.x, padding.y ) );

            // First-ever open: give the panel its preferred size, centered on the main viewport —
            // floating tools no longer pop up as tiny windows in a corner. imgui.ini keeps the
            // user's layout afterwards (FirstUseEver never fights it).
            if ( const glm::vec2 defSize = panel->GetDefaultSize(); defSize.x > 0.0f && defSize.y > 0.0f )
            {
                ImGui::SetNextWindowSize( ImVec2( defSize.x, defSize.y ), ImGuiCond_FirstUseEver );
                ImGui::SetNextWindowPos( ImGui::GetMainViewport()->GetCenter(), ImGuiCond_FirstUseEver,
                                         ImVec2( 0.5f, 0.5f ) );
            }

            // p_open: the title-bar X closes the panel and stays in sync with the View menu. The display
            // title carries an icon but keeps the ImGui ID == GetName() (see PanelDisplayTitle).
            // A panel that just auto-opened is brought to the front of its dock, otherwise it would
            // appear as a background tab nobody notices.
            if ( !m_FocusPanel.empty() && panel->GetName() == m_FocusPanel )
            {
                ImGui::SetNextWindowFocus();
                m_FocusPanel.clear();
            }

            ImGui::Begin( PanelDisplayTitle( panel->GetName() ).c_str(), &panel->GetVisibility() );
            ImGui::PopStyleVar(); // right after Begin: the window kept it, child windows must not inherit
            {
                DESERT_PROFILE_SCOPE_DYNAMIC( panel->GetName().c_str() );
                panel->OnUIRender();
            }
            ImGui::End();
        }

        // The well BEFORE the documents: it reads back the dock node id the documents are about to be
        // docked into, and a document opened this frame would otherwise float once and settle next frame.
        DrawDocumentWell();
        DrawDocuments();

        // EVERY VIEW HAS NOW HAD ITS TURN — the tool panels above (the Clouds window is one of them) and
        // the document well's own strip. Only here can "nobody drew this document" be answered, which is
        // why the run of undrawn frames is closed at this point and not inside either draw loop.
        m_OpenDocuments.EndFrame();

        DrawProfilerWindow();

        DrawStatusBar();

        DrawCommandPalette();
        DrawRecoveryPopup();
        DrawLayoutSavePopup();
        DrawOpenRefusedPopup();

        // Transient bottom-right notifications (save/import/validation). Drawn last so they float on top.
        Editor::ToastManager::Get().Draw();

        ::ImGui::End(); // End dockspace

        // The edges the OS frame used to give us. LAST, and outside the dockspace host: these are eight
        // 6px windows of their own, and submitting them here is what puts them above the panels that reach
        // the screen edge. A no-op while the window is maximized, and absent entirely when the OS draws
        // the frame.
        if ( m_WindowChrome )
            m_WindowChrome->DrawResizeBorders();

        m_ImGuiLayer->End();

        // AFTER the interface has been recorded into the swapchain pass and BEFORE the frame is submitted:
        // the only window in which the presented image is legally ours to copy out of. A no-op unless a
        // `shot.window` is waiting on exactly this frame. See RecordWindowCaptureIfDue.
        RecordWindowCaptureIfDue();

        return BOOLSUCCESS;
    }

    // Defined with the Open Scene popup's other helpers, further down this file; declared here because the
    // command palette names its scene entries the same way that popup does, and one naming rule is the
    // point — a level offered as "Arena.desce" in one list and "Levels/Arena.desce" in the other is two
    // names for one thing, and the channel would then have a name the UI never shows.
    static std::string SceneLabel( const Common::Filepath& path );

    // ONE OF A DOCUMENT'S OWN ACTIONS, RUN BY NAME. A named function rather than the lambda body it was:
    // a parameter-less multi-line lambda is the shape `bugprone-exception-escape` fires on in this tree
    // (ScenePropertiesPanel.cpp:92 records the same finding), and it is also the shape clang-format 18 and
    // 22 disagree about. The palette entry is now one line and this is where the work is.
    //
    // THE DOCUMENT IS RE-RESOLVED FROM THE SUBJECT, not captured: a window can be closed between the
    // moment this dictionary was built and the moment an entry runs, and every other document command here
    // re-resolves for that reason. A label that no longer exists is a REFUSAL — a view mode's label changes
    // with the mode it is in, so "show the curves" is gone the moment the curves are showing.
    Common::BoolResultStr EditorLayer::RunDocumentAction( const SubjectId& subject, const std::string& label )
    {
        for ( const auto& open : m_OpenDocuments )
        {
            if ( !( open->Subject() == subject ) )
            {
                continue;
            }
            for ( auto& current : open->Actions() )
            {
                if ( current.Label != label )
                {
                    continue;
                }
                // AN ACTION WITH NO CLOSURE IS NOT A NO-OP: calling an empty std::function throws, and a
                // document that published a label with nothing behind it has a defect worth naming.
                if ( !current.Run )
                {
                    return Common::MakeFormattedError<bool>(
                         "the document offers '{}' with nothing behind it — the label was published without "
                         "an action",
                         label );
                }
                current.Run();
                return PaletteCommandDone();
            }
        }
        return Common::MakeFormattedError<bool>(
             "the document that offered '{}' is gone, or no longer offers it (a view mode's label changes "
             "with the mode it is in)",
             label );
    }

    std::vector<PaletteCommand> EditorLayer::BuildPaletteCommands()
    {
        std::vector<PaletteCommand> commands;
        commands.reserve( m_Panels.Size() + m_OpenDocuments.Count() + 32 );

        // Panels — jump to / reveal any tool window. TOOLS ONLY, and by construction rather than by a
        // filter: m_Panels is a PanelRegistry, which cannot hold a document. Before the split this loop
        // offered "Open M_Crate_Painted###assetdoc..." as a panel, and running it set a visibility flag that
        // the close pass then read as "the user dismissed this window".
        for ( const auto& panel : m_Panels )
        {
            IPanel*     p    = panel.get();
            std::string name = p->GetName();
            if ( const auto hash = name.find( "##" ); hash != std::string::npos )
                name.erase( hash ); // drop the "###id" ImGui suffix for display
            commands.push_back( { "Panel", "Open " + name, [p]
                                  {
                                      p->GetVisibility() = true;
                                      p->Pinned()        = true; // asked for explicitly: keep it open
                                      return PaletteCommandDone();
                                  } } );
        }

        // THE SIX STAGES OF THE SKY, each as a command that opens the Clouds window ON that stage.
        //
        // Generated from the enum rather than typed, so a seventh stage is offered here the moment it
        // exists and cannot be forgotten — CloudStageName has no `default:`, which is what makes that safe.
        // They are the same CloudsPanel::OpenAt the Details panel's two buttons call, so a person with
        // Ctrl+P and a client on the control channel reach the window exactly the way the button does.
        for ( uint32_t i = 0; i < kCloudStageCount; ++i )
        {
            const auto stage = static_cast<CloudStage>( i );
            commands.push_back( { "Clouds", std::to_string( i + 1 ) + " " + CloudStageName( stage ), [stage]
                                  {
                                      CloudsPanel::OpenAt( stage );
                                      return PaletteCommandDone();
                                  } } );
        }

        // LANGUAGE — one entry per language the compiled locale table knows, generated from the table so
        // a language added there is offered here the moment it exists and cannot be forgotten.
        //
        // OFFERED FOR EVERY KNOWN LANGUAGE, not only the ones this project has strings in, and that is
        // deliberate: switching to a language with no translations is how an author SEES what is missing
        // (every keyed label draws its own key, and the log names each one). Hiding the entry would hide
        // the hole.
        //
        // It is also the only way a language change can be photographed on this machine, where synthetic
        // input is closed at the OS: the control channel runs these entries, so `run` then `shot.window`
        // captures the switched interface in one session.
        for ( const Localization::LocaleRow& row : Localization::Locales() )
        {
            const std::string tag = std::string( row.Tag );
            commands.push_back( { "Language", tag + " - " + std::string( row.Endonym ),
                                  [tag] { return Localization::Localization::Get().SetLanguage( tag ); } } );
        }

        // Documents — FOCUS an open one. A separate category because the verb is different and the
        // difference is the point of this task: a tool is opened, a document is switched to. Nothing here
        // creates or destroys a window, so a mistyped search cannot cost the user one.
        for ( const auto& document : m_OpenDocuments )
        {
            const SubjectId subject = document->Subject();
            commands.push_back( { "Document", "Go to " + DocumentDisplayName( document->GetName() ),
                                  [this, subject]
                                  {
                                      FocusDocument( subject );
                                      return PaletteCommandDone();
                                  } } );
        }

        // AND WHAT AN OPEN DOCUMENT CAN DO, which until now was nothing the palette knew about. A view mode
        // inside a window lives on a button, and a button is the gesture an unattended run cannot make —
        // so "the Sequencer can show its keys as curves" was a claim with no way to photograph it.
        //
        // The document's own name prefixes the label, because two Sequencers over two rigs would otherwise
        // offer two identical entries and the palette matches on the label exactly.
        for ( const auto& document : m_OpenDocuments )
        {
            const SubjectId   subject = document->Subject();
            const std::string name    = DocumentDisplayName( document->GetName() );
            for ( auto& action : document->Actions() )
            {
                // The LABEL is captured, not the action: a document can be destroyed between building this
                // list and running an entry, and re-resolving by subject is what every other document
                // command here does for the same reason.
                const std::string label = action.Label;
                // NOT A LAMBDA, and that is a finding rather than a style: `bugprone-exception-escape`
                // fires on a parameter-less lambda in this tree (ScenePropertiesPanel.cpp:92 records the
                // same one), and `PaletteCommand::Run` takes no parameters, so there is no version of a
                // lambda here that the check accepts. `bind_front` binds the member function directly and
                // there is nothing for it to analyse.
                commands.push_back( { "Document", name + ": " + label,
                                      std::bind_front( &EditorLayer::RunDocumentAction, this, subject, label ) } );
            }
        }

        // Closing one, by name. Never offered before, because a person closes a window with the x on it —
        // which is exactly the gesture no unattended run can make, and therefore the reason "close a
        // document and show what the well offers back" was a claim nobody could photograph. It goes
        // through RequestDocumentClose like the x does, so the destruction still happens between frames
        // behind the device-idle wait.
        for ( const auto& document : m_OpenDocuments )
        {
            const SubjectId subject = document->Subject();
            commands.push_back( { "Document", "Close " + DocumentDisplayName( document->GetName() ),
                                  [this, subject]
                                  {
                                      RequestDocumentClose( subject, "closed from the command "
                                                                     "palette" );
                                      return PaletteCommandDone();
                                  } } );
        }

        // Ctrl+Tab, as a command. The key is bound in OnUIRender and a key is not available to a
        // client either; this is the same CycleDocuments the keystroke calls, so the ring the two walk
        // cannot differ.
        if ( m_OpenDocuments.Count() > 1 )
        {
            commands.push_back( { "Document", "Cycle to the next most recently used", [this]
                                  {
                                      CycleDocuments();
                                      return PaletteCommandDone();
                                  } } );
        }

        // Reopening one that was closed. The list the empty well shows, reachable without a mouse — and
        // it is the same Core::SubjectOpenRequests the Selectable there uses, so a reopen is refused by the
        // six-slot cap exactly like any other open rather than becoming a second way in.
        for ( const ClosedDocument& closed : m_DocumentWell.RecentlyClosed() )
        {
            const SubjectId subject = closed.Subject;
            commands.push_back( { "Document", "Reopen " + closed.DisplayName, [subject]
                                  {
                                      Core::SubjectOpenRequests::Request( subject );
                                      return PaletteCommandDone();
                                  } } );
        }

        // Entities — select any object in the open scene.
        if ( m_MainScene )
        {
            for ( const auto& entity : m_MainScene->GetAllEntities() )
            {
                if ( !entity.HasComponent<ECS::UUIDComponent>() )
                    continue;
                const Common::UUID uuid = entity.GetComponent<ECS::UUIDComponent>().UUID;
                std::string        name = entity.HasComponent<ECS::TagComponent>()
                                               ? entity.GetComponent<ECS::TagComponent>().Tag
                                               : std::string( "Entity" );
                commands.push_back( { "Entity", name, [uuid]
                                      {
                                          Core::SelectionManager::SetSelected( uuid );
                                          return PaletteCommandDone();
                                      } } );

                // AND ITS EDITORS, because until now there was NO WAY TO OPEN ONE without a mouse. A
                // subject document — the Sequencer, the AnimGraph — is opened by a button in the Details
                // panel, and a button is the one gesture an unattended run cannot make. So every claim
                // about those windows was unphotographable, including the one this task exists to make.
                //
                // Generated from the registry rather than listed, so an editor registered tomorrow is
                // offered here the moment it exists. The two filters are the registry's own: the digest of
                // the registered type name has to BE the key it is registered under (which is what excludes
                // the asset editors, whose subject is a file and not a component), and `Exists` has to say
                // there is something on this entity to open — the same predicate the Details button asks.
                for ( const SubjectTypeKey type : m_SubjectEditors.RegisteredTypes() )
                {
                    const std::string typeName = m_SubjectEditors.TypeName( type );
                    if ( ComponentSubjectType( typeName ) != type )
                    {
                        continue;
                    }
                    const SubjectId subject = ComponentSubject( uuid, typeName );
                    if ( !m_SubjectEditors.Exists( subject ) )
                    {
                        continue;
                    }
                    commands.push_back( { "Open", "Editor: " + typeName + " on " + name, [subject]
                                          {
                                              Core::SubjectOpenRequests::Request( subject );
                                              return PaletteCommandDone();
                                          } } );
                }

                // DELETING ONE IS ALSO SOMETHING A PERSON DOES, and until now the palette could only
                // SELECT. The Outliner's context menu and the Delete key both reach
                // Commands::DeleteEntity — the same undoable command this runs — so the capability
                // was always there and only the dictionary entry was missing.
                //
                // FOUND BY NEEDING IT. Verifying "a document closes with its subject" through the control
                // channel means killing a subject through the control channel, and there was no way to
                // destroy an entity without a mouse: the channel runs these closures and nothing else. A
                // gap in the palette is a gap in what an agent can do at all, which is the one claim the
                // palette exists to make good on.
                commands.push_back( { "Entity", "Delete " + name, [uuid]
                                      {
                                          Commands::DeleteEntity( uuid );
                                          return PaletteCommandDone();
                                      } } );

                // Pilot, the Outliner's and the Details panel's third door: the one an unattended run can
                // open, so "the viewport shows what the camera sees" can be photographed at all.
                if ( entity.HasComponent<ECS::CameraComponent>() )
                {
                    commands.push_back(
                         { "Camera", "Pilot " + name, [uuid] { return Editor::PilotCameraEntity( uuid ); } } );
                }

                // LOCKING ONE IS TOO, and by the paragraph directly above it has to be here. The padlock
                // in the Outliner's gutter and the row's context menu are both a MOUSE, and the lock's
                // whole subject is what the viewport will and will not let you touch — so a channel that
                // cannot set it cannot check it either. Same recursive setter both of those call.
                {
                    // Captures the UUID and re-resolves at RUN time, exactly as Delete above does, rather
                    // than holding a Scene* and an entt handle from build time. The list is rebuilt per
                    // use, so a stale pointer is not reachable today — but "not reachable today" is a
                    // lifetime argument the next reader has to reconstruct, and a UUID lookup that simply
                    // finds nothing needs no argument at all. Asked through the shared predicate, so this
                    // label cannot disagree with the padlock the Outliner draws for the same entity.
                    const bool locked = ECS::IsLocked( m_MainScene->GetRegistry(), entity.GetHandle() );
                    commands.push_back( { "Entity", ( locked ? "Unlock " : "Lock " ) + name, [this, uuid, locked]
                                          {
                                              if ( !m_MainScene )
                                                  return PaletteCommandDone();
                                              if ( auto ref = m_MainScene->FindEntityByID( uuid ) )
                                                  ECS::SetLockedRecursive( m_MainScene->GetRegistry(),
                                                                           ref->get().GetHandle(), !locked );
                                              return PaletteCommandDone();
                                          } } );
                }

                // ── AND WHAT CAN BE OPENED *FROM* THIS ENTITY ─────────────────────────────────────────
                //
                // The other half of U7, and the half that makes a component document reachable at all
                // without a mouse. The Details panel's button is how a person opens one; this is the same
                // request under a name, which is what puts it in THE DICTIONARY — the palette, and
                // therefore the control channel, which runs these same closures.
                //
                // A DOCUMENT REACHABLE ONLY BY CLICKING A BUTTON IS MISSING FROM THAT DICTIONARY, and the
                // dictionary is this editor's one claim that "anything a person can do, an agent can do".
                // The asset documents already had their entry (the Open group below, over the registered
                // assets); a subject that is not a file had none, because there was no file to enumerate.
                // Enumerating the ENTITIES against the registered COMPONENT kinds is the same loop over
                // the other domain.
                //
                // DERIVED FROM THE REGISTRY, never a hand-written list of the two kinds that exist today:
                // a third component document appears here the moment its factory is registered, which is
                // the census this task exists to stop anybody having to refill.
                for ( const SubjectTypeKey& type : m_SubjectEditors.RegisteredTypes() )
                {
                    if ( type.Domain != SubjectDomain::EntityComponent )
                        continue;

                    const SubjectId subject{ type.Domain, type.Facet, uuid };
                    if ( !m_SubjectEditors.Exists( subject ) )
                        continue;

                    commands.push_back( { "Open", name + " \xc2\xb7 " + m_SubjectEditors.TypeName( type ),
                                          [subject]
                                          {
                                              Core::SubjectOpenRequests::Request( subject );
                                              return PaletteCommandDone();
                                          } } );
                }
            }
        }

        // SELECT EVERY PROP THAT MATCHES THIS ONE — UE's "Select > Matching", and the step without which
        // the collapse below has no input. A five-hundred-entity selection is five hundred ctrl-clicks,
        // which is also a gesture no unattended run can make; this turns "pick one crate" into "pick every
        // crate like it". The match is the FOLD'S OWN identity rule, so a selection this builds is never a
        // selection the fold then refuses for a reason nobody can see.
        if ( m_MainScene )
        {
            if ( const auto& primary = Core::SelectionManager::GetSelected(); primary.has_value() )
            {
                const Common::UUID seed = *primary;
                commands.push_back( { "Entity", "Select all with the same static mesh", [seed]
                                      {
                                          const size_t selected = Commands::SelectMatchingStaticMeshes( seed );
                                          return PaletteCommandOutcome(
                                               selected > 0,
                                               "The selected entity carries no static mesh to match." );
                                      } } );
            }
        }

        // ── THE CONTROL RIG, UNDER NAMES ─────────────────────────────────────────────────────────────
        //
        // FOUND BY NEEDING IT, exactly as the snap steps and the transform tools above were. Every gesture
        // in `ControlManipulator` is a MOUSE gesture -- enter Control mode with a checkbox, click a shape,
        // drag it -- and synthetic input is closed on this machine at both doors. So `ControlDrag`, which
        // a suite and a census both cover, had never appeared in a single frame: there was no way to put
        // it on screen without a human. Three entries close that, and none of them is a second
        // implementation: the mode goes through the authoring context's own gate, the selection through
        // `SetSelectedControl`, and the nudge through the very `ControlDrag` object the mouse grabs.
        //
        // THE PALETTE IS AN OWNER LIKE ANY OTHER. It takes the authoring context as `Kind::Panel`, so a
        // viewport or a Sequencer that wants it back takes it the same way they take it from each other,
        // and the refusals name who holds it.
        if ( m_MainScene )
        {
            if ( const auto& primary = Core::SelectionManager::GetSelected(); primary.has_value() )
            {
                const Common::UUID subject = *primary;
                commands.push_back( { "Control Rig", "Author the control rig on the selection", [this, subject]
                                      {
                                          auto& host                = Core::ActiveAuthoringContext();
                                          m_PaletteAuthoring.Entity = subject;
                                          // Focus FIRST and set the mode after: Focus adopts the published context
                                          // when the entity matches, so a mode written into `mine` beforehand is
                                          // overwritten by whatever the previous holder was in.
                                          (void)host.Focus( m_PaletteAuthoringOwner, m_PaletteAuthoring );
                                          return host.SetMode( m_PaletteAuthoringOwner, m_PaletteAuthoring,
                                                               Core::AuthoringMode::Control );
                                      } } );

                // ONE ENTRY PER CONTROL, built from the rig the selection actually carries -- the same
                // shape the per-entity and per-document entries above use. A single "select control by
                // index" entry could not be offered, because `PaletteCommand::Run` takes no arguments;
                // and a client that cannot see the viewport needs to ask for a control BY NAME anyway.
                if ( const auto& found = m_MainScene->FindEntityByID( subject ) )
                {
                    const ECS::Entity& entity = found->get();
                    if ( entity.HasComponent<ECS::AnimationComponent>() )
                    {
                        const auto& animation = entity.GetComponent<ECS::AnimationComponent>();
                        if ( animation.Animator && animation.Animator->GetRig() != nullptr )
                        {
                            const Animation::ControlHierarchy& hierarchy =
                                 animation.Animator->GetRig()->GetHierarchy();
                            for ( uint32_t control = 0; control < static_cast<uint32_t>( hierarchy.Size() );
                                  ++control )
                            {
                                const std::string name = hierarchy.Get( control ).Name;
                                commands.push_back( { "Control Rig", "Select control " + name, [this, control]
                                                      {
                                                          auto& host = Core::ActiveAuthoringContext();
                                                          return host.SetSelectedControl( m_PaletteAuthoringOwner,
                                                                                          m_PaletteAuthoring,
                                                                                          control );
                                                      } } );
                            }
                        }
                    }
                }
            }
        }

        // THE DRAG ITSELF. Offered unconditionally, like the snap steps: the refusal a client gets when
        // no control is selected is more useful than an entry that quietly is not in the dictionary, and
        // the dictionary is rebuilt per query anyway so a conditional one would come and go.
        //
        // PIXELS, NOT WORLD UNITS, and that is not a shortcut: `ControlDrag` converts a POINTER OFFSET
        // into the control's parent space, so the honest parameter of the gesture is the one the gesture
        // takes. A "move 10 cm" entry would have to invent the projection the drag exists to do, and the
        // two would disagree at every zoom but one.
        {
            constexpr struct
            {
                const char* Label;
                float       X;
                float       Y;
            } kNudges[] = {
                 { "Nudge the selected control 20 px right", 20.0f, 0.0f },
                 { "Nudge the selected control 20 px left", -20.0f, 0.0f },
                 { "Nudge the selected control 20 px up", 0.0f, -20.0f },
                 { "Nudge the selected control 20 px down", 0.0f, 20.0f },
                 { "Nudge the selected control 80 px right", 80.0f, 0.0f },
                 { "Nudge the selected control 80 px left", -80.0f, 0.0f },
                 { "Nudge the selected control 80 px up", 0.0f, -80.0f },
                 { "Nudge the selected control 80 px down", 0.0f, 80.0f },
            };
            for ( const auto& nudge : kNudges )
            {
                // Y GROWS DOWNWARD -- ImGui's convention and therefore the viewport's
                // (ControlManipulator.hpp), which is why "up" is negative here.
                const glm::vec2 delta( nudge.X, nudge.Y );
                commands.push_back( { "Control Rig", nudge.Label,
                                      [delta] { return Core::ControlNudgeRequests::Request( delta ); } } );
            }
        }

        // COLLAPSE THE SELECTION INTO ONE INSTANCED DRAW. Offered ONCE, not per entity, because its
        // subject is the selection and not an entity — the same reason the snap entries below are not
        // repeated per viewport.
        //
        // WHY IT IS A COMMAND AND NOT ONLY A BUTTON. Five hundred transforms are not typed by hand, so
        // the Details panel's instance list has no author without this; and a control that exists only
        // as a mouse click cannot be photographed or checked on this machine, where synthetic input is
        // closed at the OS. Save, "+ State" and the warning-strip rows are here for the same reason.
        //
        // It returns the planner's own refusal rather than PaletteCommandDone: a fold that would have
        // destroyed a collider must say so to whoever asked, on the channel and in the toast alike.
        // Placement by ray: the one door to it that does not need a mouse drag, so the control channel can
        // put an object on a hill and shoot the result. The ray is the active viewport's line of sight, and
        // the surface is whatever Scene::Raycast meets first — the landscape included.
        commands.push_back( { "Entity", "Place a cube on the surface at the viewport centre", [this]
                              {
                                  ::Desert::Core::EditorCamera* camera = ActiveEditorCamera();
                                  if ( !camera || !m_MainScene )
                                      return PaletteCommandOutcome( false, "no viewport camera or no scene" );
                                  const Common::Math::Ray    ray( camera->GetPosition(), camera->GetDirection() );
                                  ::Desert::Core::RaycastHit hit;
                                  if ( !m_MainScene->Raycast( ray, hit ) )
                                      return PaletteCommandOutcome( false,
                                                                    "the viewport centre looks at no surface" );
                                  auto& e       = m_MainScene->CreateNewEntity( "Cube" );
                                  auto& smc     = e.AddComponent<ECS::StaticMeshComponent>();
                                  smc.Primitive = Geometry::PrimitiveType::Cube;
                                  // The primitive cube is one metre, centred on its pivot: lift it by half
                                  // along the surface normal so it stands on the surface, not in it.
                                  e.GetComponent<ECS::TransformComponent>().Translation =
                                       hit.Point + hit.Normal * ( 0.5f * Common::Units::UnitsPerMetre );
                                  const auto uuid = e.GetComponent<ECS::UUIDComponent>().UUID;
                                  Core::SelectionManager::SetSelected( uuid );
                                  Commands::NotifyCreated( { uuid } );
                                  return PaletteCommandDone();
                              } } );
        // UE's Convert to Static Mesh: the selected EditMesh entity's geometry becomes a new .stmesh asset,
        // written where the modeling tools' Output settings say (Modeling panel, "Output Type").
        commands.push_back(
             { "Entity", "Convert to Static Mesh", []
               {
                   const auto& selection = Core::SelectionManager::GetSelection();
                   if ( selection.size() != 1 )
                       return Common::MakeFormattedError<bool>(
                            "select exactly one object to convert ({} selected)", selection.size() );
                   const auto& out     = Core::ModelingState::Get().Output;
                   const auto  written = Commands::ConvertToStaticMesh( selection.front(), out.Folder, out.Name );
                   if ( !written.IsSuccess() )
                       return Common::MakeError<bool>( written.GetError() );
                   LOG_INFO( "[Modeling] converted to static mesh '{}'", written.GetValue().generic_string() );
                   return Common::MakeSuccess( true );
               } } );
        commands.push_back( { "Entity", "Collapse selection into Instanced Static Mesh", []
                              {
                                  const auto folded = Commands::CollapseIntoInstancedMesh(
                                       Core::SelectionManager::GetSelection() );
                                  if ( !folded.IsSuccess() )
                                      return Common::MakeError<bool>( folded.GetError() );
                                  return Common::MakeSuccess( true );
                              } } );

        // THE MENU BAR. `--open-menu` is gone and this is where its capability went: a menu can be opened,
        // photographed and closed again, as many times as a session likes, instead of being pinned open
        // for a whole run by a flag with no way to say "now let go".
        for ( const char* menu : kMenuBarMenus )
        {
            const std::string name = menu;
            commands.push_back( { "Menu", "Open the " + name + " menu", [this, name]
                                  {
                                      m_HeldOpenMenu = name;
                                      return PaletteCommandDone();
                                  } } );
        }
        commands.push_back( { "Menu", "Close the open menu", [this]
                              {
                                  m_HeldOpenMenu.clear();
                                  return PaletteCommandDone();
                              } } );

        // THE SNAP, AND THE PERF HUD. Both are things a person does with a single click and neither had a
        // name, so neither could be done unattended — and a gap in this dictionary is a gap in what an
        // agent can do at all, which is the claim the palette exists to make good on. Found by needing
        // them: К6 moved the snap step to one owner and then could not photograph the defect it fixed,
        // because the sequence is "set a step, do something unrelated, look" and the channel could reach
        // neither half. The three toolbar popups and the View -> Show menu were the only ways in.
        //
        // THE STEPS ARE THE TOOLBAR'S OWN LISTS, not a copy: kGridSteps and kAngleSteps are declared once
        // at the top of this file and read by DrawSnapControl as well, so a step added there appears here
        // and the two can never offer different menus.
        //
        // Labels are ASCII on purpose. A client addresses a command by its exact label over the control
        // channel (`desertctl run Snap "Angle snap 15 deg"`), and the degree sign the toolbar button draws
        // is two UTF-8 bytes that a shell argument carries badly.
        for ( const float step : kGridSteps )
        {
            char label[48];
            if ( step >= 100.0f )
                std::snprintf( label, sizeof( label ), "Grid snap %.0f m", step / 100.0f );
            else
                std::snprintf( label, sizeof( label ), "Grid snap %.0f cm", step );
            commands.push_back( { "Snap", label, [step]
                                  {
                                      Core::GizmoState::SetTranslateSnap( step );
                                      return PaletteCommandDone();
                                  } } );
        }
        for ( const float step : kAngleSteps )
        {
            char label[48];
            std::snprintf( label, sizeof( label ), "Angle snap %.0f deg", step );
            commands.push_back( { "Snap", label, [step]
                                  {
                                      Core::GizmoState::SetRotateSnapDegrees( step );
                                      return PaletteCommandDone();
                                  } } );
        }
        commands.push_back( { "Snap", "Toggle snapping", []
                              {
                                  Core::GizmoState::SetPersistentSnap( !Core::GizmoState::PersistentSnap() );
                                  return PaletteCommandDone();
                              } } );

        // ── THE TRANSFORM TOOLS AND THE SPACE THEY WORK IN ───────────────────────────────────────────
        //
        // FOUND BY NEEDING IT, exactly as the Delete-entity entry above was. Every one of these is a
        // toolbar button and a W/E/R keystroke, and both of those are a HUMAN — so the space toggle could
        // be photographed in one of its two states and the "a locked entity draws no gizmo" claim could
        // not be photographed at all, because nothing without a mouse could put a gizmo on screen first.
        //
        // The same Core::GizmoState setters the buttons call, so these are a second SPELLING of the
        // request and never a second copy of the state.
        {
            using Gz = Core::GizmoState;

            constexpr struct
            {
                const char*   Label;
                Gz::Operation Op;
            } kTools[] = {
                 { "Select (no gizmo)", Gz::Operation::None },
                 { "Move", Gz::Operation::Translate },
                 { "Rotate", Gz::Operation::Rotate },
                 { "Scale", Gz::Operation::Scale },
            };
            for ( const auto& tool : kTools )
            {
                const auto op = tool.Op;
                commands.push_back( { "Transform", tool.Label, [op]
                                      {
                                          Gz::Set( op );
                                          return PaletteCommandDone();
                                      } } );
            }

            // Both spaces are offered by name rather than as one "toggle", because a client that cannot
            // see the button needs to be able to ASK for a state instead of flipping an unknown one.
            constexpr struct
            {
                const char* Label;
                Gz::Space   Space;
            } kSpaces[] = {
                 { "Space: World", Gz::Space::World },
                 { "Space: Local", Gz::Space::Local },
            };
            for ( const auto& choice : kSpaces )
            {
                const auto space = choice.Space;
                commands.push_back( { "Transform", choice.Label, [space]
                                      {
                                          Gz::SetSpace( space );
                                          return PaletteCommandDone();
                                      } } );
            }
        }

        // The View -> Show item, under a name. It is the cheapest action in the editor that saves the
        // preferences file while having nothing whatever to do with the gizmo, which is exactly what makes
        // it the other half of К6's scenario — and it is a dictionary entry in its own right, since
        // "turn the frame timings on" is something a person asks for by name.
        commands.push_back( { "Action", "Toggle the Perf HUD", []
                              {
                                  EditorPreferences::Get().ShowPerfHud = !EditorPreferences::Get().ShowPerfHud;
                                  EditorPreferences::Save();
                                  return PaletteCommandDone();
                              } } );

        commands.push_back( { "Camera", "Eject (stop piloting)", [] { return Editor::EjectPilot(); } } );

        // THE TWO ENDS OF К10's SCENARIO, UNDER NAMES, for the reason К6 named the snap steps and the item
        // above: a scenario whose steps can only be reached by clicking is a scenario no unattended run can
        // walk, and a claim about it is therefore unphotographable. Both are dictionary entries in their own
        // right — "show me the grid" and "switch to 2D" are things a person asks for by name, and UE's own
        // Show > Grid is searchable for the same reason.
        //
        // Note which one saves and which one does not, because that IS К10: the grid is the USER'S ANSWER
        // and persists on the click; 2D UI mode is a VIEWPORT MODE and persists nowhere at all.
        commands.push_back( { "View", "Toggle the grid", []
                              {
                                  auto& view    = EditorPreferences::Get().DebugView;
                                  view.ShowGrid = !view.ShowGrid;
                                  EditorPreferences::Save();
                                  return PaletteCommandDone();
                              } } );
        // THE VIEWPORT'S FOUR AUTHORING MODES (07 §14.2), and the reason they are palette entries rather
        // than only toolbar segments is Г14's rule applied to this tier: a capability reachable only by a
        // mouse click does not exist for the control channel, so no unattended run could ever photograph
        // the bone overlay or the control shapes — and an overlay whose appearance cannot be checked is
        // exactly the "built, tested and unseen" shape this project keeps paying for. They are VIEWPORT
        // MODES and persist nowhere, like 2D UI mode above and unlike the grid.
        //
        // GENERATED FROM THE MODE TABLE, not typed out: a fifth mode reaches the channel by existing.
        // This replaced one entry, "Toggle the control rig overlay", which flipped a process-wide static
        // directly — it could name no owner, so over the channel it could not be refused and could not
        // say which character it had just started posing.
        for ( const Core::AuthoringMode mode : Core::kAuthoringModes )
        {
            commands.push_back( { "View", std::string( "Viewport mode: " ) + Core::AuthoringModeName( mode ),
                                  [mode] { return Editor::ViewportPanel::RequestAuthoringMode( mode ); } } );
        }
        // MESH ELEMENT SELECTION (Modeling Mode). Palette entries for the same reason as the modes above: the
        // selection a later Extrude / Delete works on must be reachable - and photographable - unattended.
        commands.push_back( { "Modeling", "Select Elements tool", []
                              {
                                  Core::ModelingState::Get().ActiveTool = Core::ModelingState::Tool::ElementSelect;
                                  Core::ViewportMode::Set( Core::EditorMode::Modeling );
                                  return PaletteCommandDone();
                              } } );
        // LANDSCAPE SCULPT. Every setting of the Landscape panel is stepped by a row of LandscapeToolControls(),
        // offered here from that same table; the stroke itself is the click a hand would make at the viewport
        // centre, because PaletteCommand::Run takes no coordinates -- aim the camera, then stroke.
        commands.push_back( { "Landscape", "Sculpt mode", []
                              {
                                  Core::ViewportMode::Set( Core::EditorMode::Landscape );
                                  Core::LandscapeSculptState::Get().Mode = Core::LandscapeEdMode::Sculpt;
                                  return PaletteCommandDone();
                              } } );
        // LANDSCAPE PAINT (UE's Paint tab): the mode, its one tool, the target layer and the "+" of the Target
        // Layers list, so a frame can show a list and a stroke unattended.
        for ( const char* label : { "Paint mode", "Tool: Paint" } )
        {
            commands.push_back( { "Landscape", label, []
                                  {
                                      Core::ViewportMode::Set( Core::EditorMode::Landscape );
                                      Core::LandscapeSculptState::Get().Mode = Core::LandscapeEdMode::Paint;
                                      return PaletteCommandDone();
                                  } } );
        }
        commands.push_back( { "Landscape", "Add layer", [this]
                              {
                                  auto added = Commands::AddLandscapeLayer( m_MainScene );
                                  if ( !added.IsSuccess() )
                                      return PaletteCommandOutcome( false, added.GetError() );
                                  auto& paint = Core::LandscapeSculptState::Get().Paint;
                                  if ( paint.Layer.empty() )
                                      paint.Layer = added.GetValue();
                                  return PaletteCommandDone();
                              } } );
        if ( m_MainScene )
        {
            auto&      registry  = m_MainScene->GetRegistry();
            const auto landscape = ECS::FirstLandscape( registry );
            const auto root =
                 landscape ? ECS::FindLandscapeRootEntity( registry, *landscape ) : entt::entity( entt::null );
            if ( root != entt::null )
            {
                for ( const auto& layer : registry.get<ECS::LandscapeComponent>( root ).Layers )
                {
                    commands.push_back( { "Landscape", "Target layer: " + layer.Name, [this, name = layer.Name]
                                          {
                                              auto&      reg   = m_MainScene->GetRegistry();
                                              const auto id    = ECS::FirstLandscape( reg );
                                              const auto r     = id ? ECS::FindLandscapeRootEntity( reg, *id )
                                                                    : entt::entity( entt::null );
                                              bool       found = false;
                                              if ( r != entt::null )
                                                  for ( const auto& l :
                                                        reg.get<ECS::LandscapeComponent>( r ).Layers )
                                                      found = found || l.Name == name;
                                              if ( !found )
                                                  return PaletteCommandOutcome( false, "landscape layer '" + name +
                                                                                            "' no longer exists" );
                                              Core::LandscapeSculptState::Get().Paint.Layer = name;
                                              return PaletteCommandDone();
                                          } } );
                }
            }
        }
        for ( auto& control : Core::LandscapeToolControls() )
        {
            if ( control.Request != Core::LandscapeStrokeRequest::None )
            {
                commands.push_back( { "Landscape", control.Label, [request = control.Request]
                                      {
                                          if ( Core::ViewportMode::Get() != Core::EditorMode::Landscape )
                                              return PaletteCommandOutcome( false,
                                                                            "the Landscape mode is not active; "
                                                                            "run 'Landscape: Sculpt mode' first" );
                                          Core::LandscapeSculptState::Get().Request = request;
                                          return PaletteCommandDone();
                                      } } );
                continue;
            }
            commands.push_back( { "Landscape", control.Label, [apply = control.Apply]
                                  {
                                      apply( Core::LandscapeSculptState::Get().Settings );
                                      return PaletteCommandDone();
                                  } } );
        }
        for ( const bool lower : { false, true } )
        {
            commands.push_back(
                 { "Landscape",
                   lower ? "Stroke at the viewport centre, lowering" : "Stroke at the viewport centre", [lower]
                   {
                       if ( Core::ViewportMode::Get() != Core::EditorMode::Landscape )
                           return PaletteCommandOutcome( false, "the Landscape mode is not active; "
                                                                "run 'Landscape: Sculpt mode' first" );
                       Core::LandscapeSculptState::Get().Request =
                            lower ? Core::LandscapeStrokeRequest::Lower : Core::LandscapeStrokeRequest::Raise;
                       return PaletteCommandDone();
                   } } );
        }
        // CREATE SHAPE (Modeling Mode -> Create). One entry per shape, and the placement a click makes, at the
        // viewport centre: placing is the whole tool, and a capability the channel cannot reach does not exist
        // for an unattended check.
        for ( const Core::ModelingState::Shape shape : Core::ModelingState::kShapes )
        {
            commands.push_back( { "Modeling",
                                  std::string( "Create shape tool: " ) + Core::ModelingState::ShapeName( shape ),
                                  [shape]
                                  {
                                      auto& ms            = Core::ModelingState::Get();
                                      ms.ActiveTool       = Core::ModelingState::Tool::CreateShape;
                                      ms.CreateShape.Kind = shape;
                                      Core::ViewportMode::Set( Core::EditorMode::Modeling );
                                      return PaletteCommandDone();
                                  } } );
        }
        commands.push_back( { "Modeling", "Create shape: place at the viewport centre", []
                              {
                                  auto& ms = Core::ModelingState::Get();
                                  if ( ms.ActiveTool != Core::ModelingState::Tool::CreateShape )
                                      return PaletteCommandOutcome( false, "the Create Shape tool is not active" );
                                  ms.ReqPlaceCentre = true;
                                  return PaletteCommandDone();
                              } } );
        // ADD SHAPE: the outliner's Add > Shapes, one entry per authorable primitive, through the same spawn.
        for ( const Geometry::PrimitiveType type : Geometry::kAuthorablePrimitives )
        {
            commands.push_back( { "Scene", std::string( "Add shape: " ) + Geometry::PrimitiveTypeName( type ),
                                  [this, type]
                                  {
                                      if ( !m_MainScene )
                                          return PaletteCommandOutcome( false, "no scene is open" );
                                      Editor::SceneHierarchyPanel::SpawnPrimitive( *m_MainScene, type );
                                      return PaletteCommandDone();
                                  } } );
        }
        for ( const Geometry::ElementMode mode :
              { Geometry::ElementMode::Vertex, Geometry::ElementMode::Edge, Geometry::ElementMode::Triangle,
                Geometry::ElementMode::PolyGroup } )
        {
            commands.push_back( { "Modeling", std::string( "Mesh selection mode: " ) + Geometry::ToString( mode ),
                                  [mode] { return Core::MeshElementSelection::Get().SetMode( mode ); } } );
        }
        using SelectionOp = Core::MeshElementSelection::Op;
        for ( const SelectionOp op : { SelectionOp::SelectAll, SelectionOp::SelectConnected, SelectionOp::Grow,
                                       SelectionOp::Shrink, SelectionOp::Clear } )
        {
            commands.push_back( { "Modeling",
                                  std::string( "Mesh selection: " ) + Core::MeshElementSelection::ToString( op ),
                                  [op] { return Core::MeshElementSelection::Get().Apply( op ); } } );
        }
        commands.push_back(
             { "Modeling", "Mesh selection: pick at the viewport centre", []
               {
                   auto& state = Core::MeshElementSelection::Get();
                   if ( Core::ModelingState::Get().ActiveTool != Core::ModelingState::Tool::ElementSelect ||
                        !state.HasMesh() )
                       return PaletteCommandOutcome(
                            false, "the Select Elements tool is not on an entity with an editable mesh" );
                   state.ReqPickCentre = true;
                   return PaletteCommandDone();
               } } );
        // The operations on that selection, at the panel's values (ModelingState). Cut is not here: it needs
        // a line drawn in the viewport (the knife, Alt+K).
        for ( const Core::MeshOperation op :
              { Core::MeshOperation::Delete, Core::MeshOperation::Extrude, Core::MeshOperation::PushPull,
                Core::MeshOperation::Offset, Core::MeshOperation::Inset, Core::MeshOperation::Outset,
                Core::MeshOperation::Bevel, Core::MeshOperation::InsertEdgeLoop, Core::MeshOperation::Clean,
                Core::MeshOperation::Subdivide, Core::MeshOperation::Mirror, Core::MeshOperation::PlaneCut,
                Core::MeshOperation::Trim } )
        {
            commands.push_back( { "Modeling", std::string( "Mesh operation: " ) + Core::ToString( op ), [this, op]
                                  {
                                      if ( !m_MainScene )
                                          return PaletteCommandOutcome( false, "no scene is open" );
                                      return Core::ApplyMeshOperation( *m_MainScene, op,
                                                                       Core::ArgsFromModelingState() );
                                  } } );
        }
        commands.push_back( { "View", "Toggle 2D UI mode", [this]
                              {
                                  // REFUSES RATHER THAN DOING NOTHING when there is no scene. The mode is
                                  // a property OF a scene, so "there is none" is a fact the caller has to
                                  // hear — over the channel it used to come back as a plain success.
                                  if ( !m_MainScene )
                                      return Common::MakeError<bool>( "there is no open scene to switch "
                                                                      "into 2D UI mode." );
                                  Editor::ViewportPanel::ToggleUIMode( *m_MainScene );
                                  return PaletteCommandDone();
                              } } );

        // THE PALETTE'S OWN DOOR. Ctrl+P is the only other way to it and a keystroke is not available to
        // this machine, so the command palette was the single window in this editor that no unattended run
        // could put on screen — and therefore the one whose appearance no change to it could ever be
        // checked against. Г14's rule reaches its own instrument: a capability reachable only by hand does
        // not exist for the channel. Found by needing it, exactly as the snap steps and the entity delete
        // were: A6-1 changed WHEN this list is built and could not photograph the result.
        commands.push_back( { "View", "Open the command palette", [this]
                              {
                                  m_OpenPaletteRequested = true;
                                  return PaletteCommandDone();
                              } } );

        // OPENABLE ASSETS. This is where `--open-panel <path-to-asset>` went — the half of that flag that
        // opened a DOCUMENT rather than a tool, and the only way a document has ever been put on screen
        // unattended, since a document does not exist until something opens its asset and therefore has
        // no name to be reached by.
        //
        // ENUMERATED FROM THE PROJECT'S FILES, NOT FROM THE ASSET MANAGER'S CACHE. This loop used to walk
        // `RegisteredAssets()` — whatever the startup preloader had got round to registering — which is a
        // container whose contents are derived from the same source as the question being asked of it.
        // Measured through the control channel, once per frame: the group goes 0 -> 106 -> 130 entries,
        // because FIVE separate startup stages fill that cache, so for 3.3 s of every boot the palette
        // successfully offered every material in this project and none of its twenty-four cloud assets.
        //
        // The entity half above never had that problem, and the reason is the shape: it walks the SCENE,
        // which is what says which entities exist. The equivalent for files is the content enumeration —
        // ListFilesRecursive, which is also what the preloader walks to build the cache in the first
        // place, and which covers a mounted .dpak as well as loose files. Reading it one step earlier
        // removes the window rather than shortening it.
        //
        // Nothing is loaded to build this list, which is the other half of the argument: a project with
        // ten thousand materials costs one directory walk here, and the file is parsed by the OPENER, on
        // the frame somebody actually asks for it.
        //
        // See Editor/Core/OpenableAssets.hpp for the labelling rule and the three `model.demat` that
        // motivated it.
        for ( const OpenableAsset& asset : CollectOpenableAssets(
                   Common::Utils::FileSystem::ListFilesRecursive( Common::Constants::Path::ASSETS_PATH ),
                   m_SubjectEditors.ClaimedExtensions(), Common::Constants::Path::ASSETS_PATH ) )
        {
            const std::string path = asset.Path;
            commands.push_back( { "Open", asset.Label, [this, path]
                                  {
                                      // THROUGH THE PATH OPENERS, the same route the asset browser's
                                      // double-click takes. Resolving a path to a subject here would be a
                                      // second copy of the find-or-create-and-load chain — the exact
                                      // duplication SubjectEditorRegistry::RegisterPathOpener was
                                      // introduced to delete, when the browser and EditorLayer each
                                      // carried one.
                                      //
                                      // AND THE OUTCOME IS ANSWERED, WHICH IS A6-2 POINT 1. `(void)` stood
                                      // here: a `.demat` that would not resolve logged its reason and came
                                      // back over the channel as `{"ok":true}`, so a script opened nothing
                                      // and carried on. The opener has already said WHY in the log, with
                                      // the path — this refuses without repeating a guess at the cause,
                                      // exactly as PathOpenOutcome::Failed is documented to mean.
                                      switch ( m_SubjectEditors.OpenPath( path ) )
                                      {
                                          case SubjectEditorRegistry::PathOpenOutcome::Requested:
                                              return PaletteCommandDone();
                                          case SubjectEditorRegistry::PathOpenOutcome::Failed:
                                              return Common::MakeFormattedError<bool>(
                                                   "'{}' is a document this editor opens and it would not "
                                                   "resolve; the log line above names the reason.",
                                                   path );
                                          case SubjectEditorRegistry::PathOpenOutcome::NotMine:
                                              // The palette offered it, so an opener claimed its
                                              // extension — NotMine here means the file has GONE since
                                              // the dictionary was built, which is a fact and not a
                                              // no-op.
                                              return Common::MakeFormattedError<bool>(
                                                   "'{}' is no longer there — no registered opener claims "
                                                   "it now. It existed when this list was built.",
                                                   path );
                                      }
                                      return Common::MakeFormattedError<bool>(
                                           "opening '{}' produced an outcome this build does not handle; "
                                           "that is a defect in the palette, not in the request.",
                                           path );
                                  } } );
        }

        // THE LEVELS, which every other kind of document could already be opened by name from here and a
        // level could not — the one thing an editor exists to open was the one thing the palette had no
        // entry for, and therefore the one thing the control channel could not ask for either (the
        // channel's vocabulary IS this list). A separate group from "Open" above because these are not
        // documents: opening one REPLACES the world rather than adding a tab.
        //
        // Routed through SceneOpenRequest, not through LoadScene, on purpose: that is the path that runs
        // the unsaved-changes gate, and a palette entry is at least as easy to hit by accident as the
        // drag-and-drop it was written for.
        for ( const Common::Filepath& scene : CollectAvailableScenes() )
        {
            const std::string path = scene.string();
            commands.push_back( { "Scene", "Open Scene " + SceneLabel( scene ), [path]
                                  {
                                      Editor::Core::SceneOpenRequest::Request( path );
                                      return PaletteCommandDone();
                                  } } );
        }

        // A SECOND LIVE SCENE, for the same reason the levels above are here: the channel's vocabulary IS
        // this list, and "New Scene View" was reachable only from Window -> Viewports. That made the one
        // configuration where a renderer can bleed into another renderer — two SceneRenderers recording in
        // one frame against the same shared materials — the one configuration nothing could verify
        // unattended. Г14 needed exactly that check.
        //
        // Sets the same deferred flag the menu item does rather than calling AddSceneView(): it allocates a
        // renderer slot and GPU resources, which must not happen inside the ImGui pass.
        commands.push_back( { "Scene", "New Scene View", [this]
                              {
                                  m_AddSceneViewRequested = true;
                                  return PaletteCommandDone();
                              } } );

        // A SECOND ANGLE ON THE SAME WORLD, which is what "New Scene View" above sounds like and is not:
        // that one opens a second empty DOCUMENT. This one adds a view to the active scene — same
        // entities, same edits, a different camera — and is the entry the owner's request names.
        commands.push_back( { "Scene", "New Viewport (same scene)", [this]
                              {
                                  m_AddSceneViewportRequested = true;
                                  return PaletteCommandDone();
                              } } );

        // FOUR ANGLES IN ONE ACTION. Opening three viewports by hand and dragging each into a quarter is
        // eleven gestures, none of which a headless run can make (synthetic input is closed on this
        // machine) — so without this entry the arrangement the owner asked for could never be
        // photographed, and an arrangement nobody can see is an arrangement nobody can check.
        commands.push_back( { "Scene", "Four-Up Viewports", [this]
                              {
                                  m_ViewportGridRequested = true;
                                  return PaletteCommandDone();
                              } } );

        // The named angles, for the viewport the user is working in. Same argument as the four-up entry
        // above: the only other door is a combo inside a popup behind a toolbar button.
        for ( const ViewportCameraPresetRow& preset : kViewportCameraPresets )
        {
            const ViewportCameraPreset p = preset.Preset;
            commands.push_back( { "Scene", std::string( "Viewport Camera: " ) + preset.Name,
                                  [p]() -> Common::BoolResultStr
                                  { return Editor::ViewportPanel::RequestCameraPreset( p ); } } );
        }

        for ( const auto& view : m_ExtraViewports )
        {
            const uint64_t id = view->Id;
            commands.push_back( { "Scene", "Close Viewport " + view->Name, [this, id]() -> Common::BoolResultStr
                                  {
                                      const auto index = IndexOfSceneView(
                                           m_ExtraViewports,
                                           []( const std::unique_ptr<SceneViewport>& v ) { return v->Id; }, id );
                                      if ( !index || !m_ExtraViewports[*index]->Viewport )
                                      {
                                          return Common::MakeFormattedError<bool>(
                                               "viewport #{} is already closed; nothing to close.", id );
                                      }
                                      // Hidden rather than destroyed here, exactly as the scene-view entry above
                                      // does: the teardown waits on the device and must not run inside the ImGui
                                      // pass.
                                      m_ExtraViewports[*index]->Viewport->GetVisibility() = false;
                                      return PaletteCommandDone();
                                  } } );
        }

        // AND THE WAY BACK, which did not exist. Opening a scene view was in the palette; closing one was
        // reachable only through the window's X — and the X needs a mouse, which this machine cannot
        // synthesise. So the half of the renderer-slot budget that MATTERS was unverifiable: a slot is
        // returned by the view being DESTROYED (see CloseSceneView's ordering note), and nothing
        // unattended could destroy one. A check that can open six views and never close one measures the
        // leak it is supposed to catch as the normal state.
        //
        // Hides the panel rather than calling CloseSceneView directly: that is the SAME route the X takes
        // — CloseDismissedSceneViews collects invisible views at the top of the next OnUpdate — and the
        // teardown waits on the device, which must not happen inside the ImGui pass.
        // The ID is captured, never the panel pointer or the index — the same rule
        // Editor/Core/SceneViewIdentity.hpp states and the Preview entries below follow: a view can be
        // closed between this list being built and an entry being run, and a captured pointer would then
        // be dangling while a captured index would address somebody else's view.
        for ( const auto& doc : m_ExtraScenes )
        {
            const uint64_t id = doc->Id;
            commands.push_back( { "Scene", "Close Scene View " + doc->Name, [this, id]() -> Common::BoolResultStr
                                  {
                                      const auto index = IndexOfSceneView(
                                           m_ExtraScenes,
                                           []( const std::unique_ptr<SceneDocument>& d ) { return d->Id; }, id );
                                      if ( !index || !m_ExtraScenes[*index]->Viewport )
                                      {
                                          return Common::MakeFormattedError<bool>(
                                               "scene view #{} is already closed; nothing to close.", id );
                                      }
                                      m_ExtraScenes[*index]->Viewport->GetVisibility() = false;
                                      return PaletteCommandDone();
                                  } } );
        }

        // NAMED VIEWPOINTS for the focused document's preview — the replacement for `--preview-orbit
        // yaw,pitch`, whose continuous angle pair a palette entry has nowhere to carry. See
        // Editor/Core/PreviewViewpoints.hpp for why names are MORE reproducible than numbers, not less.
        //
        // Offered for the FOCUSED document only, because that is the one a person means by "the preview"
        // and because seven entries per open document would bury everything else in the list.
        if ( ISubjectDocument* focused = m_OpenDocuments.Find( m_FocusedDocument );
             focused && focused->HasPreview() )
        {
            for ( const PreviewViewpoint& viewpoint : kPreviewViewpoints )
            {
                const PreviewViewpoint* aim = &viewpoint;
                commands.push_back( { "Preview", std::string( viewpoint.Name ), [this, aim]
                                      {
                                          // Re-resolved rather than captured: the focus can move, and the
                                          // document can be destroyed, between this list being built and
                                          // the entry being run.
                                          ISubjectDocument* target = m_OpenDocuments.Find( m_FocusedDocument );
                                          if ( !target || !target->HasPreview() )
                                          {
                                              // REFUSES INSTEAD OF SLIPPING PAST. That re-resolution is
                                              // exactly a case that can come back empty, and the `if`
                                              // used to swallow it: the command answered success having
                                              // aimed nothing at anything.
                                              return Common::MakeError<bool>(
                                                   "the document this viewpoint was offered for no longer "
                                                   "has a preview; the focus moved between the list being "
                                                   "built and this command running." );
                                          }
                                          target->SetPreviewViewpoint( *aim );
                                          return PaletteCommandDone();
                                      } } );
            }
        }

        // THE THREE STATES OF THE FOCUSED DOCUMENT, as ordinary commands.
        //
        // Apply, Discard and Save are ACTIONS with names — they belong in the palette by the same rule
        // that put "Save Scene" there, and putting them here rather than inventing channel operations for
        // them is what keeps the channel's vocabulary the palette's vocabulary. The artist gets them on
        // the keyboard as a side effect, which is the argument for the palette in the first place.
        //
        // APPLY AND DISCARD ARE OFFERED ONLY WHILE THERE IS SOMETHING TO APPLY. The palette lists what is
        // available THIS INSTANT, exactly as the toolbar disables the two buttons in the same state; an
        // entry that ran and did nothing would be a silent no-op reported as a success, and a client
        // would read it as "the scene now has my edit".
        if ( ISubjectDocument* focused = m_OpenDocuments.Find( m_FocusedDocument ) )
        {
            const SubjectId subject = m_FocusedDocument;

            if ( focused->GetEditModel() == ISubjectDocument::EditModel::Staged && focused->HasUnappliedEdits() )
            {
                // Re-resolved inside, not captured: the focus can move and the document can be destroyed
                // between this list being built and the entry being run — the same rule the Preview
                // viewpoints above follow, for the same reason.
                // THE THREE `(void)` CASTS THAT USED TO BE HERE ARE A6-2 POINT 1 IN ONE PLACE. Each of
                // these operations already returns "did anything actually move" — ISubjectDocument says
                // so at length, and says WHY: "a caller must not report a save that did not happen". The
                // palette then threw the answer away, so over the channel an Apply that published nothing
                // and a Save that wrote no file both came back `{"ok":true}`.
                //
                // The reason cannot be richer than this, and that is a limit worth naming rather than
                // dressing up: those three virtuals return a bare `bool` and carry no message, so what
                // the editor honestly knows is THAT the document declined. Turning the interface into
                // BoolResultStr would touch every document type and belongs to whoever owns them.
                commands.push_back( { "Document", "Apply this document's edits to the scene", [this, subject]
                                      {
                                          ISubjectDocument* target = m_OpenDocuments.Find( subject );
                                          if ( !target )
                                              return Common::MakeError<bool>(
                                                   "the document that had these edits is no longer open." );
                                          return PaletteCommandOutcome(
                                               target->ApplyEdits(),
                                               "the document published nothing: it had no outstanding edit "
                                               "by the time the command ran, so the scene is unchanged." );
                                      } } );
                commands.push_back( { "Document", "Discard this document's unapplied edits", [this, subject]
                                      {
                                          ISubjectDocument* target = m_OpenDocuments.Find( subject );
                                          if ( !target )
                                              return Common::MakeError<bool>(
                                                   "the document that had these edits is no longer open." );
                                          return PaletteCommandOutcome(
                                               target->DiscardEdits(),
                                               "the document discarded nothing: it had no outstanding edit "
                                               "by the time the command ran." );
                                      } } );
            }

            commands.push_back( { "Document", "Save this document", [this, subject]
                                  {
                                      ISubjectDocument* target = m_OpenDocuments.Find( subject );
                                      if ( !target )
                                          return Common::MakeError<bool>( "the document to save is no longer "
                                                                          "open." );
                                      return PaletteCommandOutcome(
                                           target->SaveDocument(),
                                           "the document was NOT written. Its own log line says why; this "
                                           "command only knows that no file was produced." );
                                  } } );
        }

        // Actions.
        //
        // RELEASING UNUSED ASSETS BY NAME. The sweep runs by itself on every scene change, which is the
        // policy (Engine/Assets/AssetEviction.hpp) — this is the same request under a name, so that a
        // person profiling a level can ask for it without changing scene, and so that the control channel
        // can. A capability reachable only as a side effect of something else is missing from THE
        // DICTIONARY, and the dictionary is this editor's claim that anything a person can do an agent can
        // do. It goes through the schedule rather than calling Run directly, so a manual sweep lands at the
        // same safe point in the frame as an automatic one.
        commands.push_back( { "Action", "Release unused assets", []
                              {
                                  Assets::AssetEvictionSchedule::Request( "asked for from the command "
                                                                          "palette" );
                                  return PaletteCommandDone();
                              } } );

        // REBUILD CONTENT REGISTRY — the remedy every refusal in this subsystem names, reachable
        // without restarting.
        //
        // Since T2.4 neither host scans the content roots at boot: `Cooked/AssetRegistry.dreg` is the
        // list of what the project has. Content authored IN this editor enters it the moment
        // `AssetManager::CreateAsset` sees the file, and content the cook writes enters it at the
        // write — but a file that arrived on disk with nobody looking (a `git pull`, a drop into the
        // folder while the editor was closed) has no row until something walks. The boot deliberately
        // does not walk; this is what does, on demand.
        //
        // IT IS IN THE DICTIONARY AND NOT ONLY IN A MENU, for the reason "Release unused assets" is:
        // a capability reachable only as a side effect of something else is missing from the palette,
        // and the palette is this editor's claim that anything a person can do an agent can do. The
        // packager refuses to build against a stale registry and its message names this command; a
        // named remedy that cannot be run is worse than no message.
        commands.push_back( { "Action", "Rebuild Content Registry", [this]
                              {
                                  const auto cooked = Assets::ContentRegistry::Refresh( *m_AssetManager );
                                  if ( !cooked )
                                      return Common::MakeFormattedError( "the content registry: {}",
                                                                         cooked.GetError() );
                                  LOG_INFO( "[ContentRegistry] {}", cooked.GetValue().Describe() );
                                  return PaletteCommandDone();
                              } } );

        // SAVE SCENE ANSWERS WHETHER IT SAVED. `(void)SaveOpenScene()` stood here against a
        // `[[nodiscard]] bool` — the attribute was on the declaration and the cast silenced it — so a
        // scene that could not be written came back over the channel as a success. This is the same
        // family as the toast that once said "Saved 'X'" for a file that had not been written
        // (FileSystem.hpp's note on the write primitive that is gone).
        commands.push_back( { "Action", "Save Scene", [this]
                              {
                                  return PaletteCommandOutcome(
                                       SaveOpenScene(), "the scene was NOT saved; the log line above says "
                                                        "why, and the unsaved-changes mark is still set." );
                              } } );

        // PLAY AND STOP, the toolbar button's two halves. Without them a Stop restore could not be timed
        // or exercised from the control channel at all - only a mouse could reach it - and the restore is
        // the second of the two paths a 50 000-record world pays a full load on. Two entries rather than
        // one toggle, so a script that asks for Stop is told when there was nothing playing instead of
        // starting a session it did not want. Stop is deferred to OnUpdate exactly as the button defers it.
        commands.push_back( { "Action", "Play", [this]
                              {
                                  using SceneState   = ::Desert::Core::Scene::SceneState;
                                  const bool editing = m_MainScene->GetState() == SceneState::Edit;
                                  if ( editing )
                                      OnScenePlay();
                                  return PaletteCommandOutcome( editing &&
                                                                     m_MainScene->GetState() != SceneState::Edit,
                                                                "the scene is not playing; either it was "
                                                                "already playing or Play refused (the log "
                                                                "says why)." );
                              } } );
        commands.push_back( { "Action", "Stop", [this]
                              {
                                  using SceneState   = ::Desert::Core::Scene::SceneState;
                                  const bool playing = m_MainScene->GetState() != SceneState::Edit;
                                  if ( playing )
                                      m_PendingSceneStop = true;
                                  return PaletteCommandOutcome( playing, "nothing is playing, so there is "
                                                                         "nothing to stop." );
                              } } );

        // UNDO AND REDO ALREADY ANSWERED "was there anything to undo" and the answer went nowhere.
        // Nothing to undo is not a failure of the editor, but it IS the difference between a script that
        // walked the history back one step and a script that believes it did.
        commands.push_back( { "Action", "Undo", [] {
                                 return PaletteCommandOutcome( CommandHistory::Get().Undo(),
                                                               "there was nothing left to undo." );
                             } } );
        commands.push_back( { "Action", "Redo", [] {
                                 return PaletteCommandOutcome( CommandHistory::Get().Redo(),
                                                               "there was nothing to redo." );
                             } } );
        commands.push_back( { "Action", "Close All Documents", [this]
                              {
                                  RequestCloseAllDocuments();
                                  return PaletteCommandDone();
                              } } );

        // THE WINDOW'S OWN COMMANDS, offered only when this editor owns its frame — with a system frame
        // they would be a second set of buttons for three things the OS already does, and the palette would
        // be offering to press a button that is right there.
        //
        // They are here for the reason Г14 put the palette itself on the channel: a capability reachable
        // only by a mouse does not exist for an unattended run. The title bar's buttons and its
        // double-click call exactly these two window methods, so a client that cannot click can still put
        // the window through maximize and restore and photograph what came out — which is the ONLY way the
        // maximize path in this build has been executed at all, the gesture itself being unsynthesisable
        // on this machine.
        if ( m_WindowChrome )
        {
            const auto& window = m_Application->GetWindow();
            commands.push_back( { "Window", "Maximize", [window]
                                  {
                                      window->Maximize();
                                      return PaletteCommandDone();
                                  } } );
            commands.push_back( { "Window", "Restore", [window]
                                  {
                                      window->Restore();
                                      return PaletteCommandDone();
                                  } } );
            commands.push_back( { "Window", "Minimize", [window]
                                  {
                                      window->Minimize();
                                      return PaletteCommandDone();
                                  } } );
        }

        // ── BUILD: THE ONE ACTION WHOSE PRODUCT A STRANGER RUNS, AND IT WAS UNREACHABLE ────────────────
        //
        // `PackageGame` had exactly one caller in this repository — a button in the Build Settings panel
        // — so the only way to produce a game was a mouse click. On this machine synthetic input is
        // closed at both doors (osascript and CGEventPost, measured), and the control channel's own
        // promise is that "anything a human can do, it can do": packaging was the counter-example. The
        // consequence was not theoretical. The packager has been in the tree for weeks, its output has
        // unit tests over temp fixtures, and NOBODY HAD EVER STARTED THE GAME IT PRODUCES — every
        // verification of the runtime, twenty-five runs of it, was done against loose files on disk,
        // which is the developer's path and not the player's.
        //
        // SYNCHRONOUS, unlike the panel's button, which submits to a JobSystem worker and paints a
        // spinner. A palette entry's contract is that its `Run` RETURNS the outcome (see
        // CommandPalette.hpp): the channel turns that into a refusal a script can stop on. Handing the
        // work to a worker would mean returning success the instant it was queued — the exact
        // "failure reads as success" the return type was introduced to end. The cost is a frame that
        // lasts as long as the cook does, which for an explicit "build me a game" is the honest
        // behaviour rather than a surprise.
        //
        // THE THIRD STATE IS A REFUSAL HERE, and that is a choice this entry is allowed to make where
        // `PackageResult` is not. `Complete()` exists because a package with unbaked content is neither
        // success nor failure (GamePackager.hpp says why at length, and the panel paints it amber). A
        // palette command has two outcomes and no third colour, so an incomplete package reports the
        // counts as an error string: an unattended caller that got "ok" for a package with a missing
        // font would ship it.
        commands.push_back(
             { "Build", "Package Game", []() -> Common::BoolResultStr
               {
                   const auto&    prefs = EditorPreferences::Get();
                   PackageOptions options;
                   options.OutputDir    = prefs.PackageOutputDir;
                   options.Config       = prefs.PackageConfig;
                   options.MacAppBundle = prefs.PackageAppBundle;

                   const PackageResult result = PackageGame( options );
                   if ( !result.Success )
                       return Common::MakeError<bool>( result.Message );
                   if ( !result.Complete() )
                   {
                       return Common::MakeFormattedError<bool>(
                            "packaged to '{}', but {} item(s) would not cook and {} artifact(s) never "
                            "reached the disk — the game will rebuild them on the player's machine at "
                            "every start.",
                            result.PackageDir, result.CookFailures, result.CookUnwritten );
                   }
                   return Common::MakeSuccess( true );
               } } );

        return commands;
    }

    void EditorLayer::DrawCommandPalette()
    {
        // THE OVERLAY ITSELF, ASKED FOR BY NAME. Ctrl+P is the only other way in, and a keystroke is not
        // available to this machine — so the command palette was the one window in this editor that no
        // unattended run could photograph, which made every change to it unverifiable. Г14's rule applied
        // to the palette's own door: a capability reachable only by hand does not exist for the channel.
        //
        // A DEFERRED FLAG rather than calling Open() in the closure, and the reason is the one asymmetry
        // that would otherwise make this a knob that does nothing. CommandPalette::Draw runs the chosen
        // entry and then sets m_Open = false on the very next line, so an entry that opened the palette
        // from inside the palette would be closed again before the frame ended — working over the socket
        // and doing nothing under a person's hand. Consumed below, in this same frame, so the channel's
        // ordering guarantee still holds: the frame that answers the command is the frame that shows it.
        if ( m_OpenPaletteRequested )
        {
            m_OpenPaletteRequested = false;
            m_CommandPalette.Open();
        }

        // BUILT ON THE FRAME IT OPENS, AND NOT ON EVERY FRAME IT IS OPEN.
        //
        // This used to call BuildPaletteCommands() unconditionally, sixty times a second for as long as
        // the overlay was up — while EditorLayer.hpp said, one line above the declaration, "Built on
        // demand — when the palette opens, or when a request arrives — never per frame." The comment was
        // the design; the code was not doing it, and nothing said so.
        //
        // It became load-bearing with A6-1: the `Open` group is now enumerated from the project's FILES
        // rather than from the asset manager's cache, so a per-frame rebuild is a recursive walk of the
        // content tree sixty times a second while somebody types a query. (The scene list beside it,
        // CollectAvailableScenes, has always walked a directory tree here too, so the rebuild was already
        // doing disk work per frame — the file half simply made it bigger and more obvious.)
        //
        // Rebuilding on OPEN is not a snapshot going stale, and that is why this is the fix rather than a
        // cache: the palette takes the keyboard while it is up, so nothing can open a document, load a
        // scene or delete an entity between the build and the choice. Running an entry closes it, and the
        // next Ctrl+P builds again.
        if ( m_CommandPalette.TakeJustOpened() )
            m_CommandPalette.SetCommands( BuildPaletteCommands() );

        if ( !m_CommandPalette.IsOpen() )
            return;

        // AND THE PERSON WHO CLICKED HEARS IT TOO. The palette hands back what the chosen entry answered
        // (A6-2 point 1); before that, a command picked from Ctrl+P that failed simply closed the overlay
        // and left the editor looking as though it had obeyed. The toast is raised HERE and not inside
        // CommandPalette so that class keeps one UI dependency instead of two — the layer that owns the
        // toasts owns how a refusal is shown.
        if ( const auto chosen = m_CommandPalette.Draw(); !chosen )
            Editor::ToastManager::Push( chosen.GetError(), Editor::ToastLevel::Error );
    }

    void EditorLayer::DrawDocumentWell()
    {
        namespace ImGui = ::ImGui;

        // No p_open: THE AREA DOES NOT CLOSE AND DOES NOT COLLAPSE WHEN IT EMPTIES. The alternative was
        // drawn and rejected — a node that appears and disappears gives the viewport its width back and
        // takes it away again, resizing the level view under the user's cursor, and a layout that moves on
        // its own is what people report as "the editor lost my panel". The splitter is draggable: a session
        // that wants the pixels can take them, deliberately and once.
        ImGui::Begin( kDocumentWellWindow, nullptr, ImGuiWindowFlags_NoCollapse );

        // READ BACK, not remembered. The id is only known at DockBuilder time in the ONE session that built
        // the layout; every later session loads it from imgui.ini and a captured value would be 0 — which is
        // the bug the bottom drawer's own m_BottomDockId still has. Asking the window where it is docked
        // gives the same answer in every session, including after the user drags the well somewhere else.
        m_DocumentDockId = ImGui::GetWindowDockID();

        if ( m_OpenDocuments.Empty() )
        {
            // THE EMPTY STATE SAYS WHAT THE AREA IS FOR. A reserved column that is blank most of the time
            // is a column nobody learns the purpose of; this is the price B.1 pays for stable geometry and
            // it is paid in words rather than in pixels.
            const float avail = ImGui::GetContentRegionAvail().x;

            ImGui::Dummy( ImVec2( 0.0f, 24.0f ) );
            {
                // The DEFAULT font, not the bold one: the icon range is merged into the default face only,
                // so the same glyph drawn in bold comes out as the missing-glyph box. (Measured — the first
                // capture of this empty state had a "?" where the document icon belongs.)
                const char* icon = ICON_MDI_FILE_DOCUMENT_OUTLINE;
                ImGui::SetCursorPosX( ImGui::GetCursorPosX() + ( avail - ImGui::CalcTextSize( icon ).x ) * 0.5f );
                ImGui::TextDisabled( "%s", icon );
            }

            ImGui::Dummy( ImVec2( 0.0f, 8.0f ) );
            {
                const char* title = "No document open";
                ImGui::PushFont( EditorResources::GetBoldFont() );
                ImGui::SetCursorPosX( ImGui::GetCursorPosX() + ( avail - ImGui::CalcTextSize( title ).x ) * 0.5f );
                ImGui::TextUnformatted( title );
                ImGui::PopFont();
            }

            ImGui::Dummy( ImVec2( 0.0f, 6.0f ) );
            {
                // EVERY door named, because none is discoverable from an empty area. There were two while
                // every document was a FILE; the third arrived with the component documents (U7, U7-2) and
                // is not an asset slot at all — the anim graph, the emitter, the UI canvas and the two
                // timelines are opened by a button beside the component that holds them, and a user
                // reading this list would otherwise have gone looking in the Content Browser for a file
                // that does not exist.
                const char* body = "Double-click a material, a cloud type, a noise volume or a layout in the "
                                   "Content Browser \xe2\x80\x94 press the pencil on any asset slot in "
                                   "Details \xe2\x80\x94 or, for an anim graph, an emitter, a UI canvas or "
                                   "a timeline, the button beside that component in Details.";
                ImGui::PushTextWrapPos( ImGui::GetCursorPosX() + avail );
                ImGui::PushStyleColor( ImGuiCol_Text, ImGui::GetStyleColorVec4( ImGuiCol_TextDisabled ) );
                ImGui::TextUnformatted( body );
                ImGui::PopStyleColor();
                ImGui::PopTextWrapPos();
            }

            ImGui::Dummy( ImVec2( 0.0f, 10.0f ) );
            {
                const char*  label = ICON_MDI_FOLDER_MULTIPLE_OUTLINE "  Browse assets";
                const ImVec2 size( ImGui::CalcTextSize( label ).x + ImGui::GetStyle().FramePadding.x * 2.0f,
                                   0.0f );
                ImGui::SetCursorPosX( ImGui::GetCursorPosX() + ( avail - size.x ) * 0.5f );
                if ( ImGui::Button( label, size ) )
                    Core::PanelRequests::Open( "Assets" );
            }

            // RECENTLY CLOSED: the one thing an area that stays can offer that a vanishing one cannot.
            // Reopening goes through the ordinary open request, so it is refused by the slot cap exactly
            // like any other open and cannot become a second way in.
            if ( !m_DocumentWell.RecentlyClosed().empty() )
            {
                ImGui::Dummy( ImVec2( 0.0f, 12.0f ) );
                ImGui::Separator();
                ImGui::TextDisabled( "RECENTLY CLOSED" );
                for ( const ClosedDocument& closed : m_DocumentWell.RecentlyClosed() )
                {
                    ImGui::PushID( static_cast<int>( std::hash<SubjectId>{}( closed.Subject ) & 0x7fffffff ) );
                    const std::string row =
                         std::string( m_SubjectEditors.Icon( closed.Subject, kUnknownDocumentIcon ) ) + "  " +
                         closed.DisplayName;
                    if ( ImGui::Selectable( row.c_str() ) )
                        Core::SubjectOpenRequests::Request( closed.Subject );
                    if ( ImGui::IsItemHovered() )
                        ImGui::SetTooltip( "Reopen this %s document",
                                           m_SubjectEditors.TypeName( closed.Subject ).c_str() );
                    ImGui::PopID();
                }
            }

            ImGui::End();
            return;
        }

        // SOMETHING IS OPEN: the well becomes the INDEX of the area it names. Past about six documents the
        // tab strip has the one you want off its end, so a list is not a fallback here — it is the primary
        // way to switch, and it carries the two facts a tab cannot: which type each document is, and
        // whether it is holding one of the six renderer slots.
        ImGui::TextDisabled( "OPEN DOCUMENTS \xe2\x80\x94 %zu", m_OpenDocuments.Count() );
        ImGui::Separator();

        // Most recently used first, the same order Ctrl+Tab walks — one order, read in two places, so the
        // list cannot teach a different sequence from the key.
        std::vector<SubjectId> closeRequests;
        for ( const SubjectId& subject : m_DocumentWell.MostRecentOrder() )
        {
            const ISubjectDocument* document = m_OpenDocuments.Find( subject );
            if ( !document )
                continue;

            ImGui::PushID( static_cast<int>( std::hash<SubjectId>{}( subject ) & 0x7fffffff ) );

            const std::string row =
                 std::string( m_SubjectEditors.Icon( document->Subject(), kUnknownDocumentIcon ) ) + "  " +
                 DocumentDisplayName( document->GetName() );
            if ( ImGui::Selectable( row.c_str(), subject == m_FocusedDocument,
                                    ImGuiSelectableFlags_AllowItemOverlap ) )
                FocusDocument( subject );

            // The slot column. "Cloud - no slot" is not trivia: it is the answer to "I closed four windows
            // and it still will not open", because closing a CPU-drawn document frees nothing.
            const char*       slot = document->HoldsRendererSlot()    ? "1 slot"
                                     : document->ClaimsRendererSlot() ? "claiming"
                                                                      : "no slot";
            const std::string right  = m_SubjectEditors.TypeName( document->Subject() ) + " \xc2\xb7 " + slot;
            const float rightW = ImGui::CalcTextSize( right.c_str() ).x;
            ImGui::SameLine( ImGui::GetContentRegionMax().x - rightW - 28.0f );
            ImGui::TextDisabled( "%s", right.c_str() );

            ImGui::SameLine( ImGui::GetContentRegionMax().x - 18.0f );
            if ( ImGui::SmallButton( ICON_MDI_CLOSE ) )
                closeRequests.push_back( subject );

            ImGui::PopID();
        }

        ImGui::End();

        // Requested after the loop: RequestDocumentClose only queues, but collecting first keeps the rule
        // that nothing mutates a container while it is being walked.
        for ( const SubjectId& subject : closeRequests )
            RequestDocumentClose( subject, "closed from the Documents index" );
    }

    void EditorLayer::DrawDocuments()
    {
        namespace ImGui = ::ImGui;

        std::vector<SubjectId> closeRequests;
        SubjectId              focused;

        for ( const auto& document : m_OpenDocuments )
        {
            const SubjectId subject = document->Subject();

            // A DOCKED DOCUMENT, not a floating one. Before this they opened as a cascade of floating
            // windows stepped 32 px down-right from each other, which is what an application does when it
            // has nowhere to put them; option B.1 gives them somewhere. FirstUseEver, so a document the user
            // has since dragged out stays where they put it.
            if ( m_DocumentDockId != 0 )
                ImGui::SetNextWindowDockID( m_DocumentDockId, ImGuiCond_FirstUseEver );
            if ( const glm::vec2 defSize = document->GetDefaultSize(); defSize.x > 0.0f && defSize.y > 0.0f )
                ImGui::SetNextWindowSize( ImVec2( defSize.x, defSize.y ), ImGuiCond_FirstUseEver );

            if ( !m_FocusPanel.empty() && document->GetName() == m_FocusPanel )
            {
                ImGui::SetNextWindowFocus();
                m_FocusPanel.clear();
            }

            // THE CLOSE BOX WRITES TO A FRAME-LOCAL BOOL, NOT TO THE PANEL'S VISIBILITY.
            //
            // This one line is the defect, fixed. While documents lived in the panel list they were drawn
            // with `&panel->GetVisibility()` like every tool, so one bool meant "hidden" for a tool and
            // "destroy me" for a document — and the View menu, which wrote that same bool, could therefore
            // destroy a document with a tick and had no way to bring it back. A document has no visibility:
            // it is open, or it does not exist.
            bool open = true;
            const glm::vec2 docPadding = document->GetWindowPadding();
            ImGui::PushStyleVar( ImGuiStyleVar_WindowPadding, ImVec2( docPadding.x, docPadding.y ) );
            // BEGIN'S RETURN VALUE IS "IS THIS DOCUMENT ON SCREEN", and it was being thrown away. It is
            // false for a window that is collapsed and for one whose dock tab is not the active one — so
            // four documents in one dock node were all drawing their contents every frame while one of
            // them was visible, and the three that were not were also holding renderer slots for it. The
            // content is skipped, which is ImGui's own idiom, and the frames off screen are counted so the
            // slot can go back (ReleaseSlotsOfHiddenDocuments).
            const bool visible = ImGui::Begin( DocumentDisplayTitle( *document ).c_str(), &open );
            ImGui::PopStyleVar();
            if ( visible )
            {
                if ( ImGui::IsWindowFocused( ImGuiFocusedFlags_RootAndChildWindows ) )
                    focused = subject;
                {
                    DESERT_PROFILE_SCOPE_DYNAMIC( document->GetName().c_str() );
                    document->OnUIRender();
                }
                // REPORTED, NOT DECIDED HERE. This view says only "I drew it"; whether NOBODY drew it is a
                // question about all the views at once and is settled by OpenDocuments::EndFrame after
                // every one of them has run — see the note there. Written as a report rather than as an
                // erase because the Clouds window draws the same documents and the two answers must not
                // race on the order the views happen to run in.
                m_OpenDocuments.NoteDrawn( subject );
            }
            ImGui::End();

            if ( !open )
                closeRequests.push_back( subject );
        }

        // The focus is only MOVED by a document that actually has it. A frame in which the keyboard is on a
        // tool leaves the last focused document standing, so Ctrl+Tab resumes from where the user was
        // editing rather than from nothing.
        if ( !focused.IsNull() )
        {
            // CLICKING A DOCUMENT COMMITS THE RING, cycling to one does not. Both are "focus", so without
            // this distinction one of the two rules would be wrong: either a mouse click would leave
            // Ctrl+Tab walking an order the user has since abandoned, or the second Ctrl+Tab would return to
            // where the first one started. The cycling flag is cleared when Ctrl comes up, and the ring is
            // committed there — see the shortcut block in OnUIRender.
            if ( focused != m_FocusedDocument && !m_CyclingDocuments )
                m_DocumentWell.Touch( focused );

            m_FocusedDocument = focused;
        }

        for ( const SubjectId& subject : closeRequests )
            RequestDocumentClose( subject, "you closed the window" );
    }

    void EditorLayer::DrawOpenRefusedPopup()
    {
        namespace ImGui = ::ImGui;

        constexpr const char* kTitle = "Cannot open this document";

        if ( m_OpenRefusalPending )
        {
            ImGui::OpenPopup( kTitle );
            m_OpenRefusalPending = false;
        }

        ImGui::SetNextWindowPos( ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing,
                                 ImVec2( 0.5f, 0.5f ) );

        if ( !ImGui::BeginPopupModal( kTitle, nullptr, ImGuiWindowFlags_AlwaysAutoResize ) )
            return;

        if ( !m_OpenRefusal )
        {
            // Cannot normally happen; the modal is only ever opened with a refusal in hand. Closing rather
            // than drawing an empty dialog, because an empty dialog with no way out is worse than none.
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            return;
        }

        ImGui::PushStyleColor( ImGuiCol_Text, ThemeManager::GetErrorColor() );
        ImGui::TextUnformatted( ICON_MDI_ALERT_CIRCLE_OUTLINE );
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::PushFont( EditorResources::GetBoldFont() );
        ImGui::Text( "Cannot open %s", m_OpenRefusal->AssetName.c_str() );
        ImGui::PopFont();

        ImGui::TextDisabled( "All %u renderer slots are in use%s. Close one of these to free one:",
                             EngineContext::kMaxRendererSlots,
                             m_OpenRefusal->Pending > 0 ? " or already committed" : "" );
        ImGui::Separator();

        std::vector<SubjectId> closeRequests;
        for ( const RendererSlotConsumer& consumer : m_OpenRefusal->Census )
        {
            ImGui::TextUnformatted( consumer.Name.c_str() );

            // A row the user can act on gets a button; the main viewport and the Details preview do not,
            // because neither is a window a person closes to make room. Saying nothing on those rows is
            // the honest version: they are named because they explain where the slots went.
            if ( consumer.Document && m_OpenDocuments.Find( *consumer.Document ) )
            {
                ImGui::SameLine( ImGui::GetContentRegionMax().x - 64.0f );
                ImGui::PushID( static_cast<int>( std::hash<SubjectId>{}( *consumer.Document ) & 0x7fffffff ) );
                if ( ImGui::SmallButton( "Close" ) )
                    closeRequests.push_back( *consumer.Document );
                ImGui::PopID();
            }

            // The CPU-drawn documents say so, for the reason the log line already did: closing one frees
            // nothing, and a census that let the user close four of them and still be refused would be a
            // longer way of saying nothing.
            if ( !consumer.HoldsSlot && !consumer.ClaimsSlot )
            {
                ImGui::Indent( 18.0f );
                ImGui::TextDisabled( "drawn on the CPU \xe2\x80\x94 closing it frees nothing" );
                ImGui::Unindent( 18.0f );
            }
        }

        ImGui::Separator();
        if ( ImGui::Button( "Close this message", ImVec2( 180.0f, 0.0f ) ) )
        {
            m_OpenRefusal.reset();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        ImGui::TextDisabled( "The same census is in the log." );

        ImGui::EndPopup();

        for ( const SubjectId& subject : closeRequests )
            RequestDocumentClose( subject, "closed to free a renderer slot" );
    }

    void EditorLayer::DrawRecoveryPopup()
    {
        namespace ImGui = ::ImGui;

        if ( !m_ShowRecoveryPrompt )
            return;

        constexpr const char* kId = "Recover unsaved work?##recovery";
        ImGui::OpenPopup( kId );

        const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos( center, ImGuiCond_Appearing, ImVec2( 0.5f, 0.5f ) );

        if ( ImGui::BeginPopupModal( kId, nullptr, ImGuiWindowFlags_AlwaysAutoResize ) )
        {
            ImGui::TextUnformatted( "The previous session ended unexpectedly." );
            ImGui::Spacing();
            ImGui::Text( "Reopen the latest autosave?\n%s", m_RecoveryAutosave.filename().string().c_str() );
            ImGui::Spacing();
            ImGui::TextDisabled( "It opens as an unsaved scene — Save to keep it." );
            ImGui::Separator();

            if ( ImGui::Button( "Reopen autosave", ImVec2( 150.0f, 0.0f ) ) )
            {
                LoadScene( m_RecoveryAutosave );
                m_ShowRecoveryPrompt = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if ( ImGui::Button( "Ignore", ImVec2( 100.0f, 0.0f ) ) )
            {
                m_ShowRecoveryPrompt = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }

    void EditorLayer::DrawLayoutSavePopup()
    {
        namespace ImGui = ::ImGui;

        if ( !m_ShowSaveLayoutPopup )
            return;

        constexpr const char* kId = "Save Layout##saveLayout";
        ImGui::OpenPopup( kId );

        const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos( center, ImGuiCond_Appearing, ImVec2( 0.5f, 0.5f ) );

        if ( ImGui::BeginPopupModal( kId, nullptr, ImGuiWindowFlags_AlwaysAutoResize ) )
        {
            ImGui::TextUnformatted( "Layout name:" );
            ImGui::SetNextItemWidth( 260.0f );
            const bool submit = ImGui::InputText( "##layoutName", m_LayoutNameBuf, sizeof( m_LayoutNameBuf ),
                                                  ImGuiInputTextFlags_EnterReturnsTrue );

            const bool valid = !LayoutManager::Sanitize( m_LayoutNameBuf ).empty();
            ImGui::BeginDisabled( !valid );
            if ( ( ImGui::Button( "Save", ImVec2( 110.0f, 0.0f ) ) || submit ) && valid )
            {
                if ( !LayoutManager::Save( m_LayoutNameBuf ) )
                    Editor::ToastManager::Push( "The layout was not saved (see the log)",
                                                Editor::ToastLevel::Error );
                m_ShowSaveLayoutPopup = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if ( ImGui::Button( "Cancel", ImVec2( 110.0f, 0.0f ) ) )
            {
                m_ShowSaveLayoutPopup = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }

    void EditorLayer::DrawMenuBar()
    {
        namespace ImGui = ::ImGui;

        if ( !ImGui::BeginMainMenuBar() )
            return;

        // THE MENU HELD OPEN, if the control channel asked for one. `--open-menu <name>` stood here and
        // it could hold a menu open for the whole run and never let go, because a flag has no later
        // moment at which to be told otherwise. "Menu" / "Open the View menu" is now an ordinary palette
        // entry, so a session can photograph a menu and then close it and carry on.
        //
        // OpenPopup here and BeginMenu below derive the same id from the same label in the same window
        // (BeginMenu: window->GetID(label); OpenPopup: CurrentWindow->GetID(str_id)), which is what makes
        // this the menu's own opening rather than a second popup wearing its name. Re-issued every frame
        // because a menu closes as soon as focus leaves it and a shot may land on any frame.
        //
        // The name was validated against kMenuBarMenus when the command was built, so there is no unknown
        // name to reject here: the palette cannot offer one.
        if ( !m_HeldOpenMenu.empty() )
            ImGui::OpenPopup( m_HeldOpenMenu.c_str() );

        DrawFileMenu();
        DrawEditMenu();
        DrawViewMenu();
        DrawWindowMenu();
        DrawScenesMenu();
        DrawGraphicsMenu();
        DrawAboutMenu();

        DrawProjectSection();
        DrawSceneRenameSection();
        // Play/Pause/Stop now live in the toolbar strip (DrawToolbar), not the menu bar.
        //
        // THIS BAR IS THE WINDOW'S TITLE BAR NOW. It already carried the project, the level, the menus and
        // the stats while the system frame sat above it drawing a second one; the editor asks for a window
        // without a frame (Sandbox.hpp), so the three window commands and the bar's own gestures come here.
        // Both are conditional on the window actually being frameless — with a system frame they would be a
        // second set of buttons for the same three actions.
        const float chromeWidth = m_WindowChrome ? UI::WindowChrome::WindowButtonsWidth() : 0.0f;
        DrawEngineStats( chromeWidth );
        if ( m_WindowChrome )
        {
            m_WindowChrome->DrawWindowButtons();
            // LAST inside the bar, after every item: "over the bar and over nothing on it" is only a
            // question with an answer once everything on it has been submitted.
            m_WindowChrome->HandleTitleBarGestures();
        }

        ImGui::EndMainMenuBar();

        DrawPopups();
    }

    void EditorLayer::DrawFileMenu()
    {
        namespace ImGui = ::ImGui;

        if ( !ImGui::BeginMenu( "File" ) )
        {
            return;
        }

        // The editor is bound to ONE project per run (all content paths are remapped at startup).
        // Switching projects = relaunching through the Project Hub, so the menu only SHOWS the project.
        ImGui::MenuItem(
             ( std::string( ICON_MDI_PACKAGE_VARIANT " " ) + Editor::ProjectContext::Current().Name ).c_str(),
             nullptr, false, false );
        ImGui::TextDisabled( "  switch projects via the Project Hub" );
        ImGui::Separator();

        if ( ImGui::MenuItem( "Open File" ) )
        {
        }
        ImGui::Separator();

        if ( ImGui::MenuItem( "New Scene", "CTRL+N" ) )
        {
            m_NewSceneRequested = true;
        }
        if ( ImGui::MenuItem( "Save Scene", "CTRL+S" ) )
        {
            m_SaveSceneRequested = true;
        }
        if ( ImGui::MenuItem( "Reload Scene", "CTRL+R" ) )
        {
        }

        DrawOpenSceneMenuItem();
        DrawStyleSubmenu();

        ImGui::Separator();

        if ( ImGui::MenuItem( "Rebuild Cooked Assets" ) )
        {
            RebuildCookedAssets();
        }

        ImGui::Separator();

        // IT HAD AN EMPTY BODY. Found while У9 was giving the close button one: File ▸ Exit has been a
        // menu entry that does nothing since it was written, and with the system frame gone it would have
        // been the only way out of the editor other than killing the process. Same ordered close as the
        // title bar's x and the control channel's `quit`.
        if ( ImGui::MenuItem( "Exit" ) )
            const_cast<Engine::Application*>( m_Application )->Close( 0 );

        ImGui::EndMenu();
    }

    void EditorLayer::RebuildCookedAssets()
    {
        // Idle first: re-registering rebuilds GPU textures/materials.
        Graphic::Renderer::GetInstance().WaitDeviceIdle();

        if ( m_ImportManager )
        {
            m_ImportManager->ImportAllFromDirectory( Common::Constants::Path::MESH_PATH, /*force=*/true );
            m_ImportManager->ImportAllFromDirectory( Common::Constants::Path::COLLECTIONS_PATH, /*force=*/true );
            // AND THE TEXTURES DIRECTORY, WHICH THIS COMMAND DID NOT REACH. Loose textures are cooked
            // only as a mesh's dependency or by a drag-and-drop, so `Assets/Textures/` — the checker
            // floor's texture and the sky panoramas — was the one place "Rebuild
            // Cooked Assets" could not rebuild. Found the day the container's version moved: the menu
            // entry whose whole job is "the cooked form is stale, make it again" left `T_Checker.tex`
            // stale, and the only remedy left was to drag the file back into the editor.
            (void)m_ImportManager->CookLooseTextures();
        }

        if ( m_AssetPreloader )
            m_AssetPreloader->ReloadCooked();

        // Drop cached per-entity material instances so MeshECSSystem rebuilds them from the freshly
        // re-registered runtime materials (which now reference the reloaded texture images).
        if ( m_MainScene )
        {
            auto& reg = m_MainScene->GetRegistry();
            reg.view<ECS::StaticMeshComponent>().each( []( auto, ECS::StaticMeshComponent& c )
                                                       { c.RuntimeMaterialInstances.clear(); } );
            reg.view<ECS::SkinnedMeshComponent>().each( []( auto, ECS::SkinnedMeshComponent& c )
                                                        { c.RuntimeMaterialInstances.clear(); } );
        }

        if ( m_FileExplorerPanel )
            m_FileExplorerPanel->QueueRefresh();

        LOG_INFO( "[Editor] Rebuilt cooked assets" );
    }

    void EditorLayer::DrawStyleSubmenu()
    {
        namespace ImGui = ::ImGui;

        if ( !ImGui::BeginMenu( "Style" ) )
        {
            return;
        }

        if ( ImGui::MenuItem( "Dark" ) )
        {
            ThemeManager::SetDarkTheme();
        }

        if ( ImGui::MenuItem( "Black" ) )
        {
            ThemeManager::SetBlackTheme();
        }

        ImGui::EndMenu();
    }

    void EditorLayer::DrawOpenSceneMenuItem()
    {
        namespace ImGui = ::ImGui;

        if ( ImGui::MenuItem( "Open Scene" ) )
        {
            PrepareScenePopup();
            m_OpenScenePopup = true;
        }
    }

    // Case-insensitive matching for the scene filter (ASCII: scene paths on disk are ASCII).
    static std::string Lowercased( const std::string& text )
    {
        std::string out = text;
        std::transform( out.begin(), out.end(), out.begin(),
                        []( unsigned char c ) { return static_cast<char>( std::tolower( c ) ); } );
        return out;
    }

    // How a scene is NAMED in the pickers: its path relative to the scenes root ("Levels/Arena.desce"),
    // not the bare filename. With subfolders in play, filenames alone are both ambiguous (two "Test.desce"
    // in different folders read identically) and lose the only structure the user gave their scenes.
    static std::string SceneLabel( const Common::Filepath& path )
    {
        std::error_code   ec;
        const std::string rel =
             std::filesystem::relative( path, Common::Constants::Path::SCENE_PATH, ec ).generic_string();

        // Outside the scenes root (a recent scene from elsewhere): a "../../.." chain says nothing.
        if ( ec || rel.empty() || rel.rfind( "..", 0 ) == 0 )
            return path.filename().string();
        return rel;
    }

    std::vector<Common::Filepath> EditorLayer::CollectAvailableScenes()
    {
        std::vector<Common::Filepath> scenes;

        // THROUGH THE ONE CONTENT ENUMERATION, and this used to be a raw recursive_directory_iterator.
        //
        // FileSystem.hpp states the rule over ListFilesRecursive in as many words — "every scanner that
        // enumerates content must go through this: the font and icon services each used to walk only the
        // disk half, so a packaged game — where the loose directories do not exist at all — scanned
        // nothing and no text could resolve its font." This was the same defect in the same shape, one
        // list over: a project whose content is mounted from a .dpak had NO levels in the Open Scene
        // popup, in the Scene group of the command palette, or on the control channel, because the only
        // half this loop could see was the loose one.
        //
        // Latent today, because the editor never mounts a pak — and latency is not a mitigation. The
        // shared function exists precisely so that the day it stops being latent is not the day somebody
        // discovers it: a scanner that walks the disk itself is a second answer to "what content is
        // there", and this is the second one found. A6-2 point 2. `Desert/Tests/Engine/ContentScanners`
        // now holds the register, so a third has to be argued for rather than merely written.
        //
        // RECURSION AND THE MISSING-DIRECTORY CASE COME WITH IT: scenes live in subfolders (Levels/,
        // Autosave/, per-feature folders), which a flat scan simply did not list, and a missing scenes
        // directory contributes nothing rather than throwing.
        for ( const std::filesystem::path& file :
              Common::Utils::FileSystem::ListFilesRecursive( Common::Constants::Path::SCENE_PATH ) )
        {
            if ( file.extension() == Common::Constants::Extensions::SCENE_EXTENSION )
                scenes.push_back( file );
        }

        // Sorted by the label the list shows, which keeps every folder's scenes contiguous (they share the
        // "Folder/" prefix) — that is what the folder headers in the popup rely on.
        std::sort( scenes.begin(), scenes.end(), []( const Common::Filepath& a, const Common::Filepath& b )
                   { return SceneLabel( a ) < SceneLabel( b ); } );
        return scenes;
    }

    void EditorLayer::PrepareScenePopup()
    {
        m_AvailableScenes    = CollectAvailableScenes();
        m_SelectedSceneIndex = -1;
        m_SceneFilter[0]     = '\0';
    }

    void EditorLayer::BeginContentSettle()
    {
        m_Content.BeginWorld( Assets::AsyncAssetLoader::Get().StartedCount() );
    }

    void EditorLayer::ReportSplashStep( const std::string& label, const size_t step )
    {
        if ( m_Splash )
            m_Splash->SetStatus( label, step, SplashStepCount() );
    }

    // SHOWN AFTER THE FIRST REAL FRAME IS PRESENTED, NOT BEFORE IT IS DRAWN. The window has been presented
    // loading frames the whole time it was hidden, and a window shown ahead of the first real present would
    // put the last of those — an empty frame — on screen for as long as that frame takes. Shown here, the
    // surface it reveals already holds the editor, and the splash crossfades into it from this instant.
    void EditorLayer::RevealWhenReady()
    {
        if ( m_Revealed || !m_Splash || !m_RealFrameDrawn )
            return;
        m_Revealed = true;
        if ( const auto& window = m_Application->GetWindow() )
            window->Show();
        // Starts the crossfade and returns; the splash object stays until this layer is destroyed.
        m_Splash->Close();
        LOG_INFO( "[Startup] the editor is on screen and the splash is closed" );
    }

    void EditorLayer::UpdateContentSettling()
    {
        const auto& loader = Assets::AsyncAssetLoader::Get();
        if ( !m_Content.Tick( loader.Outstanding(), loader.StartedCount() ) )
            return;

        LOG_INFO( "[Content] settled after {} frame(s) in {:.1f} ms; {} read(s) have gone to a worker "
                  "this session. This is the cost that used to be a boot stage, and a scene that asks "
                  "for nothing pays none of it.",
                  m_Content.FramesWaited(), m_Content.ElapsedMs(), loader.StartedCount() );
    }

    // THE ONLY PLACE THE OS STILL SHOWS THIS WINDOW'S NAME. With the system frame gone the title is no
    // longer painted anywhere on screen, but the Dock, Mission Control, the taskbar and every window
    // switcher still read it — and the window's own name was "Desert Engine — <project>" for the whole
    // session, so those lists could not tell two editors on two levels apart.
    //
    // Compared against Window::GetTitle rather than against a copy of what was last pushed here: the window
    // owns that string, and a second copy in this file would be the same one-fact-two-owners shape as a
    // remembered "is it maximized". The comparison is what keeps this to one glfwSetWindowTitle per change
    // rather than sixty a second.
    void EditorLayer::SyncWindowTitle()
    {
        const auto& window = m_Application->GetWindow();
        if ( !window || !m_MainScene )
            return;

        const std::string title =
             "Desert Engine — " + Editor::ProjectContext::Current().Name + " — " + m_MainScene->GetSceneName();
        if ( window->GetTitle() != title )
            window->SetTitle( title );
    }

    void EditorLayer::DrawProjectSection()
    {
        namespace ImGui = ::ImGui;

        ImGui::PushFont( Editor::EditorResources::GetBoldFont() );

        ImGui::SameLine( ImGui::GetCursorPosX() + 40.0f );
        ImGui::Separator();
        ImGui::SameLine();

        // The PROJECT name (from the .deproj), not the working directory ("Editor" told you nothing).
        ImGui::TextUnformatted( Editor::ProjectContext::Current().Name.c_str() );
        Utils::ImGuiUtilities::Tooltip( Editor::ProjectContext::FilePath().c_str() );

        // Build configuration badge — you always want to know which binary you are looking at.
#ifdef DESERT_CONFIG_DEBUG
        constexpr const char* kConfig      = "DEBUG";
        const ImVec4          configColour = ImVec4( 0.95f, 0.65f, 0.25f, 1.0f );
#else
        constexpr const char* kConfig      = "RELEASE";
        const ImVec4          configColour = ImVec4( 0.35f, 0.85f, 0.45f, 1.0f );
#endif
        ImGui::SameLine();
        ImGui::TextColored( configColour, "[%s]", kConfig );

        ImGui::SameLine();
        ImGui::Separator();

        ImGui::PopFont();
    }

    void EditorLayer::DrawSceneRenameSection()
    {
        namespace ImGui = ::ImGui;

        static bool        renameScene = false;
        static std::string sceneNameBuffer;

        ImGui::SameLine( ImGui::GetCursorPosX() + 32.0f );

        if ( !renameScene )
        {
            // A SELECTABLE, NOT TEXT, and the difference is not cosmetic. ImGui gives a plain text item the
            // id 0, so IsAnyItemHovered() is FALSE while the cursor is over it — and the bar is now the
            // window's title bar, whose empty space is "drag the window" and whose empty space double-
            // clicked is "maximize". Left as text, this name would have been empty space: a double click
            // meant to rename the level would have renamed it AND maximized the window at the same time,
            // and a drag from it would have carried the window off. An id also buys the hover highlight,
            // which is the affordance the tooltip was standing in for.
            const std::string& sceneName = m_MainScene->GetSceneName();
            const ImVec2       nameSize  = ImGui::CalcTextSize( sceneName.c_str() );
            ImGui::Selectable( sceneName.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick,
                               ImVec2( nameSize.x, 0.0f ) );

            if ( ImGui::IsItemHovered() )
            {
                ImGui::SetTooltip( "Double-click to rename the scene" );
                if ( ImGui::IsMouseDoubleClicked( ImGuiMouseButton_Left ) )
                {
                    renameScene     = true;
                    sceneNameBuffer = sceneName;
                }
            }
        }
        else
        {
            ImGui::SetNextItemWidth( 200.0f );
            Utils::ImGuiUtilities::InputText( sceneNameBuffer, "##SceneRename" );

            if ( ImGui::IsItemDeactivatedAfterEdit() )
            {
                if ( !sceneNameBuffer.empty() )
                    m_MainScene->SetSceneName( sceneNameBuffer );

                renameScene = false;
            }

            if ( ImGui::IsKeyPressed( ImGuiKey_Escape ) )
            {
                renameScene = false;
            }
        }
    }

    // Collapse/restore the bottom drawer (the dock node holding Assets / Logs / Shader Code).
    //
    // ImGui has no "collapse a dock node" call — a docked window trades its collapse arrow for a tab.
    // So collapsing is done by SIZE: the node is squeezed down to its tab bar and restored to the height
    // it had before. That keeps the tabs on screen, which is the whole point of collapsing rather than
    // closing, and it leaves the user's own resize intact because the height is re-read at collapse time.
    void EditorLayer::DrawBottomDrawerToggle()
    {
        namespace ImGui = ::ImGui;

        // Resolve the drawer node from the Assets window's ACTUAL dock node, not from the id captured while
        // building the default layout: that branch only runs for a fresh layout, so with a restored
        // imgui.ini the id stayed 0 and this control was permanently dead.
        ImGuiDockNode* node = m_BottomDockId ? ImGui::DockBuilderGetNode( m_BottomDockId ) : nullptr;
        if ( !node )
        {
            if ( ImGuiWindow* assets = ImGui::FindWindowByName( PanelDisplayTitle( "Assets" ).c_str() );
                 assets && assets->DockNode )
            {
                node           = assets->DockNode;
                m_BottomDockId = node->ID;
            }
        }
        if ( !node )
        {
            ImGui::TextDisabled( ICON_MDI_CHEVRON_DOWN );
            return;
        }

        // Tab bar height + the node's own padding — what "collapsed" means for this node.
        const float collapsedHeight = ImGui::GetFrameHeight() + ImGui::GetStyle().WindowPadding.y * 2.0f;

        const char* icon = m_BottomCollapsed ? ICON_MDI_CHEVRON_UP : ICON_MDI_CHEVRON_DOWN;
        if ( ImGui::SmallButton( icon ) )
        {
            m_BottomCollapsed = !m_BottomCollapsed;
            if ( m_BottomCollapsed )
            {
                // Remember the CURRENT height, not the default: the user may have dragged the splitter.
                m_BottomHeight = node->Size.y;
                ImGui::DockBuilderSetNodeSize( m_BottomDockId, ImVec2( node->Size.x, collapsedHeight ) );
            }
            else
            {
                const float restore = m_BottomHeight > collapsedHeight
                                           ? m_BottomHeight
                                           : ImGui::GetMainViewport()->Size.y * 0.28f; // the layout default
                ImGui::DockBuilderSetNodeSize( m_BottomDockId, ImVec2( node->Size.x, restore ) );
            }
            ImGui::DockBuilderFinish( m_BottomDockId );
        }
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( m_BottomCollapsed ? "Expand the bottom drawer (Assets / Logs)"
                                                 : "Collapse the bottom drawer (Assets / Logs)" );
    }

    uint64_t EditorLayer::SceneTriangleCount()
    {
        const uint64_t revision    = CommandHistory::Get().Revision();
        const size_t   entityCount = m_MainScene->GetAllEntities().size();

        // Recompute on an edit, on the population changing, or every ~120 frames — the last one because an
        // async mesh load completes without touching either of the other two, and a status bar stuck on
        // "0 tris" while the scene is visibly full would be worse than showing no number at all.
        constexpr int kMaxCacheAgeFrames = 120;
        if ( revision == m_TriangleCacheRev && entityCount == m_TriangleCacheCount &&
             ++m_TriangleCacheAge < kMaxCacheAgeFrames )
        {
            return m_TriangleCache;
        }

        uint64_t total = 0;
        for ( const ECS::Entity& entity : m_MainScene->GetAllEntities() )
        {
            // HIDDEN entities are excluded: the number sits beside the entity count in a bar that answers
            // "what is on screen", and a hidden mesh is not.
            if ( entity.HasComponent<ECS::VisibilityComponent>() &&
                 !entity.GetComponent<ECS::VisibilityComponent>().Visible )
            {
                continue;
            }
            // ResolveDrawnMesh, not the mesh handle: a primitive draws the process-wide shared mesh and has
            // no handle at all, and counting only handles reports zero for a scene of cubes (the exact trap
            // that helper documents).
            if ( const ::Desert::Mesh* mesh = ResolveDrawnMesh( entity ) )
                total += Geometry::ComputeMeshStats( mesh->GetSubmeshes() ).Triangles;
        }

        m_TriangleCache      = total;
        m_TriangleCacheRev   = revision;
        m_TriangleCacheCount = entityCount;
        m_TriangleCacheAge   = 0;
        return total;
    }

    void EditorLayer::DrawStatusBar()
    {
        namespace ImGui  = ::ImGui;
        using SceneState = ::Desert::Core::Scene::SceneState;

        const auto   state     = m_MainScene->GetState();
        const char*  stateText = ( state == SceneState::Play )     ? ICON_MDI_PLAY " Play"
                                 : ( state == SceneState::Paused ) ? ICON_MDI_PAUSE " Paused"
                                                                   : ICON_MDI_PENCIL " Edit";
        const ImVec4 stateColor =
             ( state == SceneState::Edit ) ? ThemeManager::GetIconColor() : ThemeManager::GetSelectedColor();

        ImGui::PushStyleColor( ImGuiCol_ChildBg, ImVec4( 0.086f, 0.086f, 0.086f, 1.0f ) );
        ImGui::PushStyleVar( ImGuiStyleVar_WindowPadding, ImVec2( 8.0f, 2.0f ) );
        ImGui::BeginChild( "##StatusBar", ImVec2( 0.0f, 0.0f ), false,
                           ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse );

        // The "Content Drawer" / "Output Log" buttons that used to live here are gone. They duplicated
        // the Assets and Logs panels that are already docked along the bottom — two ways to reach one
        // thing, and the button version could only toggle a panel out of existence. What is left is a
        // single chevron that COLLAPSES that bottom drawer instead: a closed panel has to be
        // rediscovered from a menu, a collapsed one is still right there with its tabs visible.
        DrawBottomDrawerToggle();
        ImGui::SameLine( 0.0f, 12.0f );

        // Cmd: one line of Lua against the live scene, the same engine the Lua Console runs. UE puts a
        // console here for the same reason — a question about the running world should not need a panel.
        ImGui::TextDisabled( ICON_MDI_CONSOLE );
        ImGui::SameLine( 0.0f, 4.0f );
        ImGui::SetNextItemWidth( 220.0f );
        if ( ImGui::InputTextWithHint( "##StatusCmd", "Enter Console Command", m_StatusCmd, sizeof( m_StatusCmd ),
                                       ImGuiInputTextFlags_EnterReturnsTrue ) )
        {
            if ( m_StatusCmd[0] != '\0' )
            {
                Core::PanelRequests::Open( "Lua Console" );
                LuaConsolePanel::Submit( m_StatusCmd );
                m_StatusCmd[0] = '\0';
            }
        }
        ImGui::SameLine( 0.0f, 16.0f );

        // Then: scene state + current selection.
        ImGui::PushStyleColor( ImGuiCol_Text, stateColor );
        ImGui::TextUnformatted( stateText );
        ImGui::PopStyleColor();

        // (The scene name + dirty marker moved UP into the window toolbar breadcrumb.)
        ImGui::SameLine( 0.0f, 16.0f );
        if ( const size_t selCount = Core::SelectionManager::Count(); selCount > 1 )
        {
            ImGui::TextDisabled( ICON_MDI_CURSOR_DEFAULT_OUTLINE " %zu selected", selCount );
        }
        else if ( const auto sel = Core::SelectionManager::GetSelected() )
        {
            std::string selName = "Entity";
            if ( auto e = m_MainScene->FindEntityByID( *sel ) )
                selName = e->get().GetComponent<ECS::TagComponent>().Tag;
            ImGui::TextDisabled( ICON_MDI_CURSOR_DEFAULT_OUTLINE " %s", selName.c_str() );
        }
        else
        {
            ImGui::TextDisabled( "No selection" );
        }

        ImGui::SameLine( 0.0f, 16.0f );
        ImGui::TextDisabled( ICON_MDI_SHAPE " %zu entities", m_MainScene->GetAllEntities().size() );

        // What the scene costs to draw, beside what it contains. Two numbers that belong together: an
        // entity count says how much there is to manage, a triangle count says how much there is to
        // render, and only the second one explains a frame time.
        ImGui::SameLine( 0.0f, 16.0f );
        {
            const uint64_t tris = SceneTriangleCount();
            ImGui::TextDisabled( ICON_MDI_TRIANGLE_OUTLINE " %s tris", FormatThousands( tris ).c_str() );
            if ( ImGui::IsItemHovered() )
                ImGui::SetTooltip( "Triangles in the VISIBLE meshes of this scene (LOD 0)." );
        }

        // HOW MANY DOCUMENTS, AND HOW MANY OF THE SIX SLOTS ARE GONE. Both numbers already existed in the
        // code — GetLiveRendererCount and PendingRendererSlotDemand — and neither had anywhere to appear,
        // so the first a user heard of the cap was a click that did nothing. A count of documents is not
        // the number that matters; the slot census is, which is why they are shown together: three
        // documents can be three slots or none, depending on which three.
        ImGui::SameLine( 0.0f, 16.0f );
        {
            const uint32_t live    = Graphic::SceneRenderer::GetLiveRendererCount();
            const uint32_t pending = PendingRendererSlotDemand( m_OpenDocuments.Documents() );

            // ImGuiCol_TextDisabled, not ImGuiCol_Text: the line below is drawn with TextDisabled like the
            // rest of the bar, and pushing the wrong colour would leave it grey with a colour nobody sees.
            const bool tight = live + pending >= EngineContext::kMaxRendererSlots;
            if ( tight )
                ImGui::PushStyleColor( ImGuiCol_TextDisabled, ThemeManager::GetWarningColor() );
            ImGui::TextDisabled( ICON_MDI_FILE_DOCUMENT_MULTIPLE_OUTLINE " %zu document%s \xc2\xb7 %u/%u slots",
                                 m_OpenDocuments.Count(), m_OpenDocuments.Count() == 1 ? "" : "s", live,
                                 EngineContext::kMaxRendererSlots );
            if ( tight )
                ImGui::PopStyleColor();

            if ( ImGui::IsItemHovered() )
                ImGui::SetTooltip( "%zu open document(s). %u of the %u renderer slots are in use and %u more "
                                   "are committed to documents that have not drawn yet; a document that "
                                   "needs one is refused when they are all spoken for.",
                                   m_OpenDocuments.Count(), live, EngineContext::kMaxRendererSlots, pending );
        }

        // Active snap state: off, or the step of the CURRENT transform tool — answers "why did it
        // jump?" without opening the snap popup.
        ImGui::SameLine( 0.0f, 16.0f );
        {
            using Gz = ::Desert::Editor::Core::GizmoState;
            if ( !Gz::PersistentSnap() )
                ImGui::TextDisabled( ICON_MDI_MAGNET " off" );
            else
                switch ( Gz::Get() )
                {
                    case Gz::Operation::Rotate:
                        ImGui::TextDisabled( ICON_MDI_MAGNET " %.1f\xC2\xB0", Gz::RotateSnapDegrees() );
                        break;
                    case Gz::Operation::Scale:
                        ImGui::TextDisabled( ICON_MDI_MAGNET " x%.2f", Gz::ScaleSnap() );
                        break;
                    default:
                        // CENTIMETRES, and metres only past a metre — the same rule DrawSnapControl
                        // formats the toolbar button with, and it has to be the same rule because the two
                        // labels sit on one screen reading one value. This said "%.2fm" over a value that
                        // is in world units (1 unit = 1 cm), so a 5 m step read "500.00m" three inches
                        // from a button reading "5 m". Third sighting of У5's metre-era label: the field's
                        // default, the Preferences slider, and now the status bar.
                        if ( Gz::TranslateSnap() >= 100.0f )
                            ImGui::TextDisabled( ICON_MDI_MAGNET " %.0f m", Gz::TranslateSnap() / 100.0f );
                        else
                            ImGui::TextDisabled( ICON_MDI_MAGNET " %.0f cm", Gz::TranslateSnap() );
                        break;
                }
            if ( ImGui::IsItemHovered() )
                ImGui::SetTooltip( "Snap (toggle in the viewport toolbar; Ctrl inverts while dragging)" );
        }

        // Right: how much the log is complaining, then which build this is.
#ifdef DESERT_CONFIG_DEBUG
        constexpr const char* kBuildConfig = "Debug";
#else
        constexpr const char* kBuildConfig = "Release";
#endif
        const float fps = ImGui::GetIO().Framerate;
        // The BRANCH beside the version: this project runs eight worktrees at once, and "which of these
        // windows is my build" was previously answerable only from the commit hash. It comes from the same
        // build-time git identity the version does, so it cannot disagree with the hash beside it.
        char stats[220];
        std::snprintf( stats, sizeof( stats ),
                       ICON_MDI_SOURCE_BRANCH " %s   %s  %s   " ICON_MDI_SPEEDOMETER " %.0f FPS   %.2f ms",
                       Common::Version::Branch(), Common::Version::Full(), kBuildConfig, fps,
                       fps > 0.0f ? 1000.0f / fps : 0.0f );

        // "Are there warnings?" answered where you are already looking, without opening the log. The count
        // comes from the Logs panel's parse of the file — the one place that has read it — so the chip in
        // that panel and this number cannot disagree.
        const std::size_t warnings   = LogsPanel::WarningCount();
        const std::size_t errors     = LogsPanel::ErrorCount();
        char              alerts[96] = {};
        if ( errors > 0 )
            std::snprintf( alerts, sizeof( alerts ), ICON_MDI_CLOSE_CIRCLE_OUTLINE " %zu   " ICON_MDI_ALERT " %zu",
                           errors, warnings );
        else if ( warnings > 0 )
            std::snprintf( alerts, sizeof( alerts ), ICON_MDI_ALERT " %zu warnings", warnings );

        const bool  dirty   = CommandHistory::Get().Revision() != s_SavedRevision;
        const float starW   = dirty ? ImGui::CalcTextSize( "* " ).x : 0.0f;
        const float statsW  = ImGui::CalcTextSize( stats ).x;
        const float alertsW = alerts[0] ? ImGui::CalcTextSize( alerts ).x + 16.0f : 0.0f;
        ImGui::SameLine( ImGui::GetWindowContentRegionMax().x - statsW - starW - alertsW );

        if ( alerts[0] )
        {
            // Errors outrank warnings in the colour as well as in the text: one red count is the whole
            // signal, and painting it amber because warnings are also present would bury it.
            ImGui::TextColored( errors > 0 ? ThemeManager::GetErrorColor() : ThemeManager::GetWarningColor(), "%s",
                                alerts );
            if ( ImGui::IsItemHovered() )
                ImGui::SetTooltip( "%zu error(s), %zu warning(s) in this session's log — click to open it.",
                                   errors, warnings );
            if ( ImGui::IsItemClicked() )
                Core::PanelRequests::Open( "Logs" );
            ImGui::SameLine( 0.0f, 16.0f );
        }

        if ( dirty )
        {
            // Amber star next to the version/config block = unsaved scene changes.
            ImGui::TextColored( ThemeManager::GetWarningColor(), "*" );
            if ( ImGui::IsItemHovered() )
                ImGui::SetTooltip( "Unsaved changes (Ctrl+S to save)" );
            ImGui::SameLine( 0.0f, ImGui::CalcTextSize( " " ).x );
        }
        ImGui::TextDisabled( "%s", stats );

        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
    }

    // One toolbar button: an icon, an optional label, and an "armed" state that is drawn as a tinted fill
    // plus a 2px underline. The underline matters — a tint alone is ambiguous against a hover, and the
    // question "which mode am I in" has to be answerable from across the room.
    bool EditorLayer::ToolbarButton( const char* icon, const char* label, bool active, const char* tooltip,
                                     bool enabled )
    {
        namespace ImGui = ::ImGui;

        char text[192];
        if ( label && *label )
            std::snprintf( text, sizeof( text ), "%s  %s", icon, label );
        else
            std::snprintf( text, sizeof( text ), "%s", icon );

        const ImVec4 accent = ThemeManager::GetSelectedColor();
        ImGui::PushStyleColor( ImGuiCol_Button, active ? ImVec4( accent.x, accent.y, accent.z, 0.30f )
                                                       : ImVec4( 0.0f, 0.0f, 0.0f, 0.0f ) );
        ImGui::PushStyleColor( ImGuiCol_ButtonHovered, ImVec4( 1.0f, 1.0f, 1.0f, 0.09f ) );
        ImGui::PushStyleColor( ImGuiCol_ButtonActive, ImVec4( 1.0f, 1.0f, 1.0f, 0.16f ) );
        ImGui::PushStyleColor( ImGuiCol_Text,
                               active ? ImGui::GetStyleColorVec4( ImGuiCol_Text ) : ThemeManager::GetIconColor() );
        if ( !enabled )
            ImGui::BeginDisabled();

        const bool clicked = ImGui::Button( text );

        if ( active )
        {
            const ImVec2 mn = ImGui::GetItemRectMin();
            const ImVec2 mx = ImGui::GetItemRectMax();
            ImGui::GetWindowDrawList()->AddRectFilled( ImVec2( mn.x, mx.y - 2.0f ), mx,
                                                       ImGui::GetColorU32( accent ) );
        }
        if ( !enabled )
            ImGui::EndDisabled();
        ImGui::PopStyleColor( 4 );

        if ( tooltip && ImGui::IsItemHovered( ImGuiHoveredFlags_AllowWhenDisabled ) )
            ImGui::SetTooltip( "%s", tooltip );
        return clicked;
    }

    void EditorLayer::ToolbarSeparator()
    {
        namespace ImGui = ::ImGui;
        ImGui::SameLine( 0.0f, 8.0f );
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float  h = ImGui::GetFrameHeight();
        ImGui::GetWindowDrawList()->AddLine( ImVec2( p.x, p.y + 3.0f ), ImVec2( p.x, p.y + h - 3.0f ),
                                             IM_COL32( 70, 70, 70, 255 ) );
        ImGui::SameLine( 0.0f, 9.0f );
    }

    void EditorLayer::DrawToolbar()
    {
        namespace ImGui = ::ImGui;
        using Gz        = ::Desert::Editor::Core::GizmoState;
        using Mode      = ::Desert::Editor::Core::ViewportMode;
        using EMode     = ::Desert::Editor::Core::EditorMode;

        // THE STRIP HAS WORK NOW.
        //
        // It used to hold two playback buttons hard against the right edge and about 900px of nothing, and
        // the comment here argued that a second row of commands was "more chrome between the menu and the
        // picture". That was true of a DUPLICATE row. What the owner approved instead is the row UE
        // actually ships: the four things that are true of the whole editor rather than of one panel —
        // what you can undo, what mode you are in, how the gizmo behaves, and whether the world is
        // running — none of which had a home. Editor MODES in particular could only be reached from a
        // combo inside the viewport's own strip, which is the one place you cannot see while looking at
        // another panel.
        //
        // Everything here drives state that already exists and already has exactly one owner: CommandHistory,
        // ViewportMode, GizmoState, Scene::GetState. No control on this bar holds a value of its own.
        const float barHeight = ImGui::GetFrameHeight() + 12.0f;

        ImGui::PushStyleColor( ImGuiCol_ChildBg, ImVec4( 0.086f, 0.086f, 0.086f, 1.0f ) ); // #161616 strip
        ImGui::PushStyleVar( ImGuiStyleVar_WindowPadding, ImVec2( 8.0f, 4.0f ) );
        ImGui::PushStyleVar( ImGuiStyleVar_ItemSpacing, ImVec2( 2.0f, 0.0f ) );
        ImGui::PushStyleVar( ImGuiStyleVar_FramePadding, ImVec2( 8.0f, 5.0f ) );
        ImGui::PushStyleVar( ImGuiStyleVar_FrameRounding, 4.0f );
        ImGui::BeginChild( "##Toolbar", ImVec2( 0.0f, barHeight ), false,
                           ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse );

        const bool editMode = m_MainScene->GetState() == ::Desert::Core::Scene::SceneState::Edit;

        // ---- Left: the file/history group -------------------------------------------------------
        const bool dirty = CommandHistory::Get().Revision() != s_SavedRevision;
        if ( ToolbarButton( ICON_MDI_CONTENT_SAVE, "Save", false,
                            dirty ? "Save the scene (Ctrl+S) — there are unsaved changes"
                                  : "Save the scene (Ctrl+S)" ) )
        {
            // The SAME deferred flag the File menu sets, not a second call to Serialize: saving mid-frame
            // from a toolbar and saving from a menu must be one code path, or one of them will grow a
            // step (the revision marker, a toast) the other forgets.
            m_SaveSceneRequested = true;
        }
        ImGui::SameLine();

        const auto& undoStack = CommandHistory::Get().UndoStack();
        const auto& redoStack = CommandHistory::Get().RedoStack();
        // The tooltip NAMES the edit, which is the difference between an undo button and a dare.
        const std::string undoTip =
             undoStack.empty() ? "Nothing to undo" : "Undo " + undoStack.back()->GetLabel() + " (Ctrl+Z)";
        const std::string redoTip =
             redoStack.empty() ? "Nothing to redo" : "Redo " + redoStack.back()->GetLabel() + " (Ctrl+Shift+Z)";
        if ( ToolbarButton( ICON_MDI_UNDO, "", false, undoTip.c_str(), editMode && !undoStack.empty() ) )
            CommandHistory::Get().Undo();
        ImGui::SameLine();
        if ( ToolbarButton( ICON_MDI_REDO, "", false, redoTip.c_str(), editMode && !redoStack.empty() ) )
            CommandHistory::Get().Redo();
        ToolbarSeparator();

        // ---- Editor modes -----------------------------------------------------------------------
        // Exactly the three EditorMode values the engine HAS. The mock also drew Landscape and Paint;
        // those modes do not exist, and a button that switches to nothing is a dead setting whichever
        // picture it came from.
        const EMode mode = Mode::Get();
        if ( ToolbarButton( ICON_MDI_CURSOR_DEFAULT_OUTLINE, "Select", mode == EMode::Select,
                            "Selection and transform tools" ) )
            Mode::Set( EMode::Select );
        ImGui::SameLine();
        if ( ToolbarButton( ICON_MDI_CUBE_OUTLINE, "Modeling", mode == EMode::Modeling,
                            "Geometry tools (CubeGrid blockout)" ) )
            Mode::Set( EMode::Modeling );
        ImGui::SameLine();
        if ( ToolbarButton( ICON_MDI_GRASS, "Foliage", mode == EMode::Foliage, "Paint instanced vegetation" ) )
            Mode::Set( EMode::Foliage );
        ToolbarSeparator();

        // ---- Transform tools --------------------------------------------------------------------
        //
        // THE KEYS NAMED HERE ARE THE KEYS THAT WORK. These three tooltips read "(W)", "(E)" and "(R)"
        // — UE's bindings — while the only handler in the editor binds T, R and C
        // (ViewportPanel::OnKeyPressedEvent). So the rail advertised three shortcuts that did nothing,
        // and the viewport strip's own tooltips (Move (T) / Rotate (R) / Scale (C)) said the true thing
        // eight inches away. A UI string is a promise about the tree, and this one was not kept.
        //
        // Corrected toward the CODE rather than toward UE, deliberately: adopting W/E/R is a shortcut
        // decision with a Foliage/Modeling conflict to weigh and belongs to whoever owns the keymap, not
        // to a tooltip edit. Naming the working key costs nothing and is true today either way.
        const Gz::Operation op = Gz::Get();
        if ( ToolbarButton( ICON_MDI_CURSOR_MOVE, "", op == Gz::Operation::Translate, "Translate (T)" ) )
            Gz::Set( Gz::Operation::Translate );
        ImGui::SameLine();
        if ( ToolbarButton( ICON_MDI_ROTATE_ORBIT, "", op == Gz::Operation::Rotate, "Rotate (R)" ) )
            Gz::Set( Gz::Operation::Rotate );
        ImGui::SameLine();
        if ( ToolbarButton( ICON_MDI_ARROW_EXPAND_ALL, "", op == Gz::Operation::Scale, "Scale (C)" ) )
            Gz::Set( Gz::Operation::Scale );
        ImGui::SameLine();

        // ---- Transform space -------------------------------------------------------------------
        // One button that both REPORTS the space and flips it, the same bargain the snap controls make
        // below. It asks EffectiveSpace(), not GetSpace(), because ImGuizmo throws the mode away while
        // scaling (ImGuizmo.cpp:2653) — so during a Scale the honest thing to show is Local, disabled,
        // rather than a "World" the handles will not honour. The button that lies is worse than the
        // button that is greyed out, and this is the only place the two could have drifted apart.
        {
            const bool      forced  = Gz::SpaceIsForced( op );
            const Gz::Space space   = Gz::EffectiveSpace( op );
            const bool      isLocal = space == Gz::Space::Local;

            const char* tip = forced ? "Scaling is always along the object's own axes — a world-axis "
                                       "scale of a rotated object is a shear, which a transform cannot hold"
                              : isLocal
                                   ? "Transform space: Local — drag along the object's own axes (click for World)"
                                   : "Transform space: World — drag along the world axes (click for Local)";

            if ( ToolbarButton( isLocal ? ICON_MDI_AXIS_ARROW : ICON_MDI_EARTH, isLocal ? "Local" : "World",
                                isLocal, tip, /*enabled=*/!forced ) )
                Gz::SetSpace( isLocal ? Gz::Space::World : Gz::Space::Local );
        }
        ToolbarSeparator();

        // ---- The two snap values ----------------------------------------------------------------
        // Each button both REPORTS its step and opens the list that changes it, and the shared magnet
        // toggle sits at the top of both lists rather than becoming a third button: snapping is one state,
        // and two buttons for it would be two places to read a single yes/no.
        DrawSnapControl( /*rotation=*/false );
        ImGui::SameLine();
        DrawSnapControl( /*rotation=*/true );

        // ---- Centre: playback -------------------------------------------------------------------
        // Centred, deliberately. Playback is the only control here that says what the WORLD is doing
        // rather than what the editor is doing, and in the right-hand corner it read as one more tool.
        // Clamped so it never lands on the mode rail on a narrow window — it slides right instead of
        // overlapping, because a Play button under another button is worse than an off-centre one.
        {
            const float  leftEnd = ImGui::GetItemRectMax().x;
            const float  btnH    = ImGui::GetFrameHeight();
            const ImVec2 btnSize( btnH * 1.6f, btnH );
            const float  playW     = 84.0f;
            const float  groupW    = playW + ( btnSize.x + 2.0f ) * 2.0f;
            const float  windowMid = ImGui::GetWindowPos().x + ImGui::GetWindowSize().x * 0.5f;
            const float  startX    = std::max( windowMid - groupW * 0.5f, leftEnd + 24.0f );

            ImGui::SameLine();
            ImGui::SetCursorScreenPos( ImVec2( startX, ImGui::GetCursorScreenPos().y ) );

            // Play is the bar's PRIMARY action and gets the accent; Pause is a modifier of a state that
            // is already running and stays neutral. Undifferentiated, the pair reads as two equal
            // buttons and the eye has to read the glyphs to find the one it wants.
            const ImVec4 accent = ThemeManager::GetSelectedColor();
            ImGui::PushStyleColor( ImGuiCol_Button, accent );
            ImGui::PushStyleColor( ImGuiCol_ButtonHovered,
                                   ImVec4( accent.x + 0.10f, accent.y + 0.08f, accent.z + 0.06f, 1.0f ) );
            ImGui::PushStyleColor( ImGuiCol_ButtonActive,
                                   ImVec4( accent.x * 0.8f, accent.y * 0.8f, accent.z * 0.8f, 1.0f ) );
            ImGui::PushStyleColor( ImGuiCol_Text, ImVec4( 1.0f, 1.0f, 1.0f, 1.0f ) );
            DrawPlayButton( ImVec2( playW, btnH ) );
            ImGui::PopStyleColor( 4 );

            ImGui::SameLine();
            DrawPauseButton( btnSize );
        }

        // ---- Right: the things you leave the editor through --------------------------------------
        {
            ImGui::SameLine();
            const float rightGroupW = 330.0f;
            const float x           = std::max( ImGui::GetItemRectMax().x + 24.0f,
                                                ImGui::GetWindowPos().x + ImGui::GetWindowSize().x - rightGroupW );
            ImGui::SetCursorScreenPos( ImVec2( x, ImGui::GetCursorScreenPos().y ) );

            if ( ToolbarButton( ICON_MDI_PACKAGE_VARIANT_CLOSED, "Package", false,
                                "Build and package the project" ) )
                Core::PanelRequests::Open( "Build Settings" );
            ImGui::SameLine();
            if ( ToolbarButton( ICON_MDI_MONITOR_DASHBOARD, "Profiler", m_ShowProfiler,
                                "Per-pass CPU and GPU timings" ) )
                m_ShowProfiler = !m_ShowProfiler;
            ImGui::SameLine();
            if ( ToolbarButton( ICON_MDI_COG, "Settings", s_ShowPreferences, "Editor preferences" ) )
                s_ShowPreferences = !s_ShowPreferences;
        }

        ImGui::EndChild();
        ImGui::PopStyleVar( 4 );
        ImGui::PopStyleColor();
    }

    void EditorLayer::DrawSnapControl( bool rotation )
    {
        namespace ImGui = ::ImGui;
        using Gz        = ::Desert::Editor::Core::GizmoState;

        // The steps are declared once at the top of this file, because the command palette offers exactly
        // these and a second copy here is how the two lists would drift apart.

        char label[64];
        if ( rotation )
            std::snprintf( label, sizeof( label ), "%.0f\xC2\xB0", Gz::RotateSnapDegrees() );
        else if ( Gz::TranslateSnap() >= 100.0f )
            std::snprintf( label, sizeof( label ), "%.0f m", Gz::TranslateSnap() / 100.0f );
        else
            std::snprintf( label, sizeof( label ), "%.0f cm", Gz::TranslateSnap() );

        const char* icon = Gz::PersistentSnap() ? ICON_MDI_MAGNET_ON : ICON_MDI_MAGNET;
        const char* tip  = rotation ? "Angle snap — click to change the step or toggle snapping"
                                    : "Grid snap — click to change the step or toggle snapping";
        if ( ToolbarButton( rotation ? ICON_MDI_ANGLE_ACUTE : icon, label, Gz::PersistentSnap(), tip ) )
            ImGui::OpenPopup( rotation ? "##AngleSnapPopup" : "##GridSnapPopup" );

        if ( ImGui::BeginPopup( rotation ? "##AngleSnapPopup" : "##GridSnapPopup" ) )
        {
            bool snapping = Gz::PersistentSnap();
            if ( ImGui::Checkbox( "Snapping", &snapping ) )
                Gz::SetPersistentSnap( snapping );
            if ( ImGui::IsItemHovered() )
                ImGui::SetTooltip( "Holding Ctrl while dragging inverts this." );
            ImGui::Separator();

            if ( rotation )
            {
                for ( const float step : kAngleSteps )
                {
                    char item[32];
                    std::snprintf( item, sizeof( item ), "%.0f\xC2\xB0", step );
                    if ( ImGui::Selectable( item, Gz::RotateSnapDegrees() == step ) )
                        Gz::SetRotateSnapDegrees( step );
                }
            }
            else
            {
                for ( const float step : kGridSteps )
                {
                    char item[32];
                    if ( step >= 100.0f )
                        std::snprintf( item, sizeof( item ), "%.0f m", step / 100.0f );
                    else
                        std::snprintf( item, sizeof( item ), "%.0f cm", step );
                    if ( ImGui::Selectable( item, Gz::TranslateSnap() == step ) )
                        Gz::SetTranslateSnap( step );
                }
            }
            ImGui::EndPopup();
        }
    }

    // The profiler table as text. Used by the panel's button AND by --gpu-profile, because a headless shot
    // draws no ImGui and the panel is the only other way these numbers are readable.
    //
    // The GPU column comes from the backend's timestamp queries, so it is device time, not the CPU's wait
    // for it; the two columns disagreeing is the interesting case rather than a fault.
    void EditorLayer::RecordFlightFrame( bool counted )
    {
        const ShotOptions&           shot     = ShotOptions::Get();
        Common::Profiling::Profiler& profiler = Common::Profiling::Profiler::Get();

        double gpuMs    = Flight::kNotMeasured;
        double streamMs = 0.0;
        for ( const Common::Profiling::ScopeResult& scope : profiler.LastFrame() )
        {
            if ( scope.Name == Common::Profiling::kGpuFrameTotalScope && profiler.GpuEnabled() )
                gpuMs = scope.GpuMs;
            else if ( scope.Name == "WorldStreamer::Tick" )
                streamMs = scope.TotalMs;
        }
        m_FlightLog.TimeLast( profiler.LastFrameMs(), gpuMs, streamMs );

        Flight::FrameRow row;
        row.Frame    = m_ShotFrame;
        row.Kind     = m_ShotFrame < Flight::kWarmupFrames ? Flight::Phase::Warmup
                       : counted                           ? Flight::Phase::Flight
                                                           : Flight::Phase::Settling;
        row.Distance = Flight::DistanceAt( m_ShotFrame, shot.FlightSpeed, ShotOptions::PlayStepSeconds );
        row.Position = Flight::PoseAt( *shot.FlightRoute, row.Distance ).Position;
        row.Entities = m_MainScene->GetAllEntities().size();
        if ( m_WorldStreamer && m_WorldStreamer->Streams( *m_MainScene ) )
        {
            const auto& report   = m_WorldStreamer->LastTick();
            row.ResidentRecords  = report.LiveRecords;
            row.UnitsActivated   = report.Tick.UnitsActivated;
            row.UnitsDeactivated = report.Tick.UnitsDeactivated;
            row.RecordsActivated = report.Tick.RecordsActivated;
            row.RecordsDestroyed = report.Tick.RecordsDestroyed;
            row.ActivationMs     = report.ActivationMs;
            row.ActivatedUnits   = report.ActivatedUnits;
        }
        m_FlightLog.Append( std::move( row ) );
    }

    bool EditorLayer::FinishFlight()
    {
        const ShotOptions& shot = ShotOptions::Get();
        const auto         rows = m_FlightLog.Rows();
        const auto         written =
             Common::Utils::FileSystem::WriteContentToFileAtomic( shot.FlightCsv, Flight::Csv( rows ) );
        if ( !written.IsSuccess() )
        {
            LOG_ERROR( "[Flight] the CSV was not written to '{}': {}", shot.FlightCsv, written.GetError() );
            return false;
        }
        const auto summary = Flight::Summarise( rows );
        if ( !summary.IsSuccess() )
        {
            LOG_ERROR( "[Flight] '{}': {}", shot.FlightRoute->Spec, summary.GetError() );
            return false;
        }
        LOG_INFO( "[Flight] '{}' at {:.0f} cm/s, {} row(s) in '{}': {}", shot.FlightRoute->Spec, shot.FlightSpeed,
                  rows.size(), shot.FlightCsv, Flight::Describe( summary.GetValue(), rows ) );
        return true;
    }

    void EditorLayer::DumpProfilerToLog()
    {
        auto& prof = ::Common::Profiling::Profiler::Get();

        const double frameMs = prof.LastFrameMs();
        const double fps     = frameMs > 0.0001 ? 1000.0 / frameMs : 0.0;

        const std::string frameTotalScope = ::Common::Profiling::kGpuFrameTotalScope;

        double gpuFrameMs = 0.0;
        double gpuSumMs   = 0.0;
        for ( const auto& s : prof.LastFrame() )
        {
            if ( s.Name == frameTotalScope )
                gpuFrameMs = s.GpuMs;
        }

        LOG_INFO( "[Profiler] ---- per-pass breakdown (averaged over {:.1f} s of frames) ----",
                  prof.AvgWindowSeconds() );
        LOG_INFO( "[Profiler] Frame (wall) {:.3f} ms ({:.0f} FPS), GPU frame {:.3f} ms", frameMs, fps,
                  gpuFrameMs );
        LOG_INFO( "[Profiler] {:<34} {:>10} {:>6} {:>10} {:>10} {:>6}", "scope", "cpu ms", "x", "gpu ms",
                  "gpu self", "x" );

        for ( const auto& s : prof.LastFrame() )
        {
            LOG_INFO( "[Profiler] {:<34} {:>10.3f} {:>6} {:>10.3f} {:>10.3f} {:>6}", s.Name, s.TotalMs, s.Calls,
                      s.GpuMs, s.GpuSelfMs, s.GpuCalls );
            // SELF time is the only summable column — the inclusive one counts a parent's microseconds
            // again in each child. The frame bracket is the denominator, not a pass, so it stays out.
            if ( s.GpuCalls > 0 && s.Name != frameTotalScope )
                gpuSumMs += s.GpuSelfMs;
        }

        LOG_INFO( "[Profiler] GPU self times sum to {:.3f} ms of a {:.3f} ms GPU frame ({:.1f} %); the "
                  "remainder is device work no pass is marked around.",
                  gpuSumMs, gpuFrameMs, gpuFrameMs > 0.0001 ? gpuSumMs / gpuFrameMs * 100.0 : 0.0 );

        // THE DRAW-CALL DETECTOR, READ WITHOUT A WINDOW. The counter itself landed with step 2 of
        // `Docs/World/PROGRAMME.md`, and its only reader was the viewport's perf HUD — which is ImGui,
        // which is drawn into the swapchain, which `--shot` does not read. So the one number step 3 is
        // judged on was unreadable in exactly the mode a measurement is taken in. It joins the profiler
        // dump rather than getting a flag of its own because it answers the same question the dump does
        // — what did this frame cost — and because a second flag would be a second thing to remember.
        const Graphic::DrawCounters drawCounters = Graphic::DrawCounter::LastFrame();
        LOG_INFO( "[Profiler] draws {} / instances {} (an instanced batch is ONE draw and many instances; "
                  "the two are printed apart because culling moves them in different proportions)",
                  drawCounters.Draws, drawCounters.Instances );
        LOG_INFO( "[Profiler] ---- end ----" );
    }

    void EditorLayer::DrawProfilerWindow()
    {
        namespace ImGui = ::ImGui;
        auto& prof      = ::Common::Profiling::Profiler::Get();

        if ( !m_ShowProfiler )
            return;

        const double frameMs = prof.LastFrameMs();
        const double fps     = frameMs > 0.0001 ? 1000.0 / frameMs : 0.0;

        ImGui::SetNextWindowSize( ImVec2( 420, 460 ), ImGuiCond_FirstUseEver );
        ImGui::SetNextWindowPos( ImVec2( 700, 120 ), ImGuiCond_FirstUseEver );
        if ( !ImGui::Begin( "Profiler", &m_ShowProfiler ) ) // X button clears m_ShowProfiler
        {
            ImGui::End();
            return;
        }

        ImGui::Checkbox( "Enabled", &prof.Enabled() );
        ImGui::SameLine();
        ImGui::Checkbox( "Sort by time", &prof.SortByTime() );
        ImGui::SameLine();
        // GPU timestamps are OFF by default: they cost ~8 % of a debug frame on MoltenVK, and an
        // always-on instrument means every later measurement carries the tax. Turning this on is a
        // deliberate act. See Docs/GPU_TIMESTAMPS.md for the measured price.
        ImGui::BeginDisabled( prof.GetGpuSink() == nullptr );
        ImGui::Checkbox( "GPU", &prof.GpuEnabled() );
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "Device timestamps around every pass.\n"
                               "Costs about 8%% of the frame it measures, so it is off by default." );
        ImGui::SameLine();
        ImGui::BeginDisabled( !prof.GpuEnabled() );
        ImGui::Checkbox( "per-pass", &prof.GpuPassScopes() );
        ImGui::EndDisabled();
        if ( ImGui::IsItemHovered( ImGuiHoveredFlags_AllowWhenDisabled ) )
            ImGui::SetTooltip( "Off: time the whole frame only (two timestamps, near-free).\n"
                               "On: also time every pass, which is what costs." );
        ImGui::EndDisabled();
        if ( prof.GetGpuSink() == nullptr && ImGui::IsItemHovered( ImGuiHoveredFlags_AllowWhenDisabled ) )
            ImGui::SetTooltip( "This device reports no usable timestamp queries — CPU columns only." );

        ImGui::SetNextItemWidth( 160.0f );
        ImGui::SliderFloat( "Avg window (s)", &prof.AvgWindowSeconds(), 0.1f, 2.0f, "%.1f" );

        // The whole-frame GPU bracket the backend records around the command buffer. It is the denominator
        // the per-pass GPU column is checked against: the passes should tile it, not exceed it.
        double gpuFrameMs = 0.0;
        for ( const auto& s : prof.LastFrame() )
            if ( s.Name == ::Common::Profiling::kGpuFrameTotalScope )
                gpuFrameMs = s.GpuMs;

        ImGui::Text( "Frame: %.3f ms  (%.0f FPS)   [avg]", frameMs, fps );
        if ( gpuFrameMs > 0.0 )
        {
            ImGui::SameLine();
            ImGui::TextColored( ImVec4( 0.55f, 0.80f, 1.0f, 1.0f ), "GPU: %.3f ms", gpuFrameMs );
        }
        ImGui::SameLine();
        if ( ImGui::Button( "Dump to Log" ) )
            DumpProfilerToLog();

        ImGui::Separator();

        if ( ImGui::BeginTable( "##prof", 6,
                                ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
                                     ImGuiTableFlags_SizingStretchProp ) )
        {
            // The numeric columns are FIXED width and the name stretches. With six columns sharing the
            // width proportionally, the panel docked at its usual size truncated every header to
            // "cp... gp... gpu..." — unreadable, and the two GPU columns are the ones a reader has to
            // tell apart. A millisecond figure needs a known number of characters, not a share of the
            // panel, so it gets one.
            const float kNumWidth = ImGui::CalcTextSize( "0000.000" ).x;
            ImGui::TableSetupColumn( "scope", ImGuiTableColumnFlags_WidthStretch );
            ImGui::TableSetupColumn( "cpu", ImGuiTableColumnFlags_WidthFixed, kNumWidth );
            ImGui::TableSetupColumn( "gpu", ImGuiTableColumnFlags_WidthFixed, kNumWidth );
            ImGui::TableSetupColumn( "self", ImGuiTableColumnFlags_WidthFixed, kNumWidth );
            ImGui::TableSetupColumn( "%", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize( "000" ).x );
            ImGui::TableSetupColumn( "x", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize( "000" ).x );
            ImGui::TableHeadersRow();

            for ( const auto& s : prof.LastFrame() )
            {
                const double pct = frameMs > 0.0001 ? ( s.TotalMs / frameMs ) * 100.0 : 0.0;
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted( s.Name.c_str() );
                // Docked at its usual width the name column clips, and "Clouds: Sha" / "Clouds: Exe" are
                // two different passes. The full name on hover costs nothing and settles it.
                if ( ImGui::IsItemHovered() )
                    ImGui::SetTooltip( "%s", s.Name.c_str() );
                ImGui::TableNextColumn();
                ImGui::Text( "%.3f", s.TotalMs );
                ImGui::TableNextColumn();
                // A dash, not 0.000: a scope that records no GPU work and a scope the GPU timer could not
                // reach are different states, and printing zero for both invents a measurement.
                if ( s.GpuCalls > 0 )
                    ImGui::TextColored( ImVec4( 0.55f, 0.80f, 1.0f, 1.0f ), "%.3f", s.GpuMs );
                else
                    ImGui::TextDisabled( "-" );
                ImGui::TableNextColumn();
                // Nested passes subtracted — the column that can be added up.
                if ( s.GpuCalls > 0 )
                    ImGui::TextColored( ImVec4( 0.45f, 0.70f, 0.95f, 1.0f ), "%.3f", s.GpuSelfMs );
                else
                    ImGui::TextDisabled( "-" );
                ImGui::TableNextColumn();
                // Tint hot scopes (>25% of the frame) red.
                if ( pct > 25.0 )
                    ImGui::TextColored( ImVec4( 1.0f, 0.45f, 0.35f, 1.0f ), "%.1f", pct );
                else
                    ImGui::Text( "%.1f", pct );
                ImGui::TableNextColumn();
                ImGui::Text( "%u", s.Calls );
            }
            ImGui::EndTable();
        }

        ImGui::End();
    }

    void EditorLayer::DrawEngineStats( float rightMargin )
    {
        namespace ImGui = ::ImGui;

        const auto text = m_Application->GetEngineStats().GetFormattedStats();
        auto       size = ImGui::CalcTextSize( text.c_str() );

        ImGui::SameLine( ImGui::GetWindowContentRegionMax().x - rightMargin - size.x -
                         ImGui::GetStyle().ItemSpacing.x * 2.0f );

        // TextUnformatted, not Text: ImGui::Text takes a printf FORMAT, so this passed runtime-built
        // engine stats as the format string. Today GetFormattedStats() can only produce
        // "FPS: 60 | Frame: 16.67ms" and contains no '%', so nothing has gone wrong — but the day any
        // percentage is added to that line (a GPU utilisation, a budget fraction — the obvious next
        // additions) ImGui's vsnprintf reads a vararg that was never passed.
        //
        // The example above said "16.6ms" while the function was printing SIX decimals — the argument
        // was right and the sample output was a different program's. It is two decimals now because
        // GetFormattedStats was fixed, not because the comment was made to agree with it.
        ImGui::TextUnformatted( text.c_str() );
    }

    void EditorLayer::DrawPopups()
    {
        DrawOpenScenePopup();
        DrawConfirmOpenScenePopup();
        DrawSaveScenePopup();
        DrawNewScenePopup();
        DrawReloadScenePopup();
        DrawProjectPopup();
        DrawPreferencesWindow();
    }

    void EditorLayer::DrawEditMenu()
    {
        namespace ImGui = ::ImGui;

        if ( !ImGui::BeginMenu( "Edit" ) )
        {
            return;
        }

        const bool editMode     = m_MainScene->GetState() == ::Desert::Core::Scene::SceneState::Edit;
        const bool hasSelection = Core::SelectionManager::Count() > 0;

        if ( ImGui::MenuItem( "Undo", "Ctrl+Z", false, editMode ) )
            CommandHistory::Get().Undo();
        if ( ImGui::MenuItem( "Redo", "Ctrl+Shift+Z", false, editMode ) )
            CommandHistory::Get().Redo();

        ImGui::Separator();

        if ( ImGui::MenuItem( "Copy", "Ctrl+C", false, editMode && hasSelection ) )
            Commands::CopySelectionToClipboard( Core::SelectionManager::GetSelection() );
        if ( ImGui::MenuItem( "Paste", "Ctrl+V", false, editMode && Commands::ClipboardHasContent() ) )
        {
            if ( auto pasted = Commands::PasteClipboard(); !pasted.empty() )
                Core::SelectionManager::SetSelection( std::move( pasted ) );
        }
        if ( ImGui::MenuItem( "Duplicate", "Ctrl+D", false, editMode && hasSelection ) )
        {
            if ( auto dups = Commands::DuplicateEntities( Core::SelectionManager::GetSelection() ); !dups.empty() )
                Core::SelectionManager::SetSelection( std::move( dups ) );
        }
        if ( ImGui::MenuItem( "Delete", "Del", false, editMode && hasSelection ) )
            Commands::DeleteEntities( Core::SelectionManager::GetSelection() );

        ImGui::Separator();
        if ( ImGui::MenuItem( "Preferences..." ) )
            s_ShowPreferences = true;

        ImGui::EndMenu();
    }

    // THERE IS NO "SAVE" BUTTON HERE, AND ITS ABSENCE IS THE FEATURE (К8).
    //
    // Every control below binds &prefs.X and therefore edits the LIVE store — which is what the editor
    // reads every frame, so the change is already applied while the mouse is still down. The button
    // beneath them only wrote the file, and that split one decision between two deciders: closing this
    // window with the x left the edits live but unwritten (the user "cancelled" and the change kept
    // working), while ANY unrelated save — the Perf HUD toggle, an MSAA pick in Scene Settings, a star in
    // Details — silently committed those abandoned edits to disk. The button promised a decision it did
    // not take, and somebody in another panel took it.
    //
    // A gate was refused rather than overlooked. Gating would mean rewiring every CONSUMER onto a
    // committed copy, for values whose whole point is being visible while you drag the slider; a working
    // copy (as У8 built for materials) does not pay here, because a personal setting is neither authored
    // nor shared and there is nothing to revert to but what the user just saw; and three other panels
    // already persist into this same file on a click, so a gate in this one window would leave one file
    // half-gated — the same two-writers-disagreeing shape one floor up.
    //
    // So each control commits itself on ImGui::IsItemDeactivatedAfterEdit(), the pattern the viewport's
    // snap popup already uses: ONE file write when the mouse is released, not sixty a second while a drag
    // reports a change every frame. Nothing else in this window may call Save(), and
    // Desert/Tests/Editor/PreferenceOwnership reads this function to hold both halves — that every control
    // has its commit, and that no call site here saves outside one.
    void EditorLayer::DrawPreferencesWindow()
    {
        namespace ImGui = ::ImGui;
        if ( !s_ShowPreferences )
            return;

        ImGui::SetNextWindowSize( ImVec2( 380.0f, 0.0f ), ImGuiCond_Appearing );
        if ( ImGui::Begin( "Preferences", &s_ShowPreferences, ImGuiWindowFlags_NoDocking ) )
        {
            auto& prefs = EditorPreferences::Get();

            ImGui::Spacing();
            ImGui::TextDisabled( "Editor Camera" );
            ImGui::Separator();
            if ( ImGui::SliderFloat( "Speed", &prefs.CameraSpeed, 0.1f, 10.0f, "%.2fx" ) )
                if ( auto cam = m_MainScene->GetMainCamera().lock() )
                    if ( auto* editorCam = dynamic_cast<::Desert::Core::EditorCamera*>( cam.get() ) )
                        editorCam->SetMovementSpeed( prefs.CameraSpeed );
            if ( ImGui::IsItemDeactivatedAfterEdit() )
                EditorPreferences::Save();

            ImGui::Spacing();
            ImGui::TextDisabled( "Gizmo Snap" );
            ImGui::Separator();
            ImGui::Checkbox( "Snap always on (Ctrl inverts)", &prefs.PersistentSnap );
            if ( ImGui::IsItemDeactivatedAfterEdit() )
                EditorPreferences::Save();
            // CENTIMETRES, which is what the value has always been fed into: this control said "(m)" and
            // clamped to 0.01..100 while writing a field GizmoState reads as world units, and a world
            // unit is 1 cm. A slider whose unit disagrees with its consumer is how the shipped grid snap
            // ended up at half a centimetre (see EditorPreferences::TranslateSnap).
            //
            // Nothing is pushed anywhere afterwards: these four ARE the snap's storage and GizmoState
            // reads them, so the gizmo follows on the same frame. The block that used to copy them into
            // GizmoState is gone with the copy it fed (К6).
            ImGui::DragFloat( "Move (cm)", &prefs.TranslateSnap, 1.0f, 1.0f, 10000.0f, "%.0f" );
            if ( ImGui::IsItemDeactivatedAfterEdit() )
                EditorPreferences::Save();
            ImGui::DragFloat( "Rotate (deg)", &prefs.RotateSnapDeg, 0.5f, 0.1f, 180.0f, "%.1f" );
            if ( ImGui::IsItemDeactivatedAfterEdit() )
                EditorPreferences::Save();
            ImGui::DragFloat( "Scale", &prefs.ScaleSnap, 0.01f, 0.01f, 10.0f, "%.2f" );
            if ( ImGui::IsItemDeactivatedAfterEdit() )
                EditorPreferences::Save();

            ImGui::Spacing();
            ImGui::TextDisabled( "Autosave" );
            ImGui::Separator();
            ImGui::SliderInt( "Interval (min)", &prefs.AutosaveMinutes, 0, 30,
                              prefs.AutosaveMinutes == 0 ? "Off" : "%d min" );
            if ( ImGui::IsItemDeactivatedAfterEdit() )
                EditorPreferences::Save();
            // Wrapped for the same reason as the footer below: at 380 px this line was clipped to
            // "...the main file is nev" and the reassurance it exists to give was the part cut off.
            ImGui::PushStyleColor( ImGuiCol_Text, ImGui::GetStyleColorVec4( ImGuiCol_TextDisabled ) );
            ImGui::TextWrapped( "Autosaves land in Scene/Autosave/, the main file is never touched." );
            ImGui::PopStyleColor();

            ImGui::Spacing();
            ImGui::TextDisabled( "Selection Outline" );
            ImGui::Separator();
            ImGui::Checkbox( "Enable Outline", &prefs.EnableOutline );
            if ( ImGui::IsItemDeactivatedAfterEdit() )
                EditorPreferences::Save();
            // NoInputs: a swatch that opens the picker, not three cramped R/G/B fields. Two reasons, and
            // the first is a real collision rather than taste — with ThemeManager's
            // style.ColorButtonPosition = ImGuiDir_Left, ImGui places a ColorEdit's LABEL after the last
            // item it drew, which is the swatch and not the inputs, so the word "Color" is rendered on top
            // of the R field. (That is ImGui's own arithmetic and it affects every labelled ColorEdit in
            // this editor drawn under this theme; reported rather than fixed here, because the fix lives
            // in ThemeManager and the left-hand swatch is a deliberate UE-parity choice.) The second is
            // that three numeric fields do not fit a 380 px window, which is why this row was the one that
            // showed it.
            ImGui::ColorEdit3( "Color", glm::value_ptr( prefs.OutlineColor ), ImGuiColorEditFlags_NoInputs );
            if ( ImGui::IsItemDeactivatedAfterEdit() )
                EditorPreferences::Save();
            ImGui::SliderFloat( "Width (px)", &prefs.OutlineWidth, 0.0f, 20.0f );
            if ( ImGui::IsItemDeactivatedAfterEdit() )
                EditorPreferences::Save();
            ImGui::SliderFloat( "Smoothness", &prefs.OutlineSmoothness, 0.0f, 10.0f );
            if ( ImGui::IsItemDeactivatedAfterEdit() )
                EditorPreferences::Save();

            ImGui::Spacing();
            // WRAPPED, not two TextDisabled lines: this window opens 380 px wide and the user may make it
            // narrower, and a fixed line is silently CLIPPED by the window edge rather than shortened —
            // the first capture of this footer read "...and is written to" with the rest gone. A sentence
            // explaining that there is no Save button is a poor sentence to lose the end of.
            ImGui::PushStyleColor( ImGuiCol_Text, ImGui::GetStyleColorVec4( ImGuiCol_TextDisabled ) );
            ImGui::TextWrapped( "There is no Save button: every setting here applies as you change it and "
                                "is written to ~/.desertengine/editor.json when you let go of the control." );
            ImGui::PopStyleColor();
        }
        ImGui::End();
    }

    void EditorLayer::DrawViewMenu()
    {
        namespace ImGui = ::ImGui;

        if ( !ImGui::BeginMenu( "View" ) )
        {
            return;
        }

        // GROUPED BY WHAT THE ENTRY IS FOR. A flat alphabet-of-whatever-was-constructed-first list is a list
        // nobody reads; the ones that answer "where do I look at the level / the content / the output" stay
        // at the top level and the ones that belong to a particular job move behind the job's own submenu.
        // Nothing is deleted and nothing becomes unreachable — see the leftover section at the end, which is
        // empty when every panel is placed.
        //
        // THE COUNT USED TO BE WRITTEN OUT HERE ("twenty-one tools, twelve entries") AND IT WAS WRONG. Tools
        // are added to and taken out of m_Panels by every task that promotes one to a document, and a scene
        // view joins the same registry at RUNTIME (EditorLayer::AddSceneView) — so no number typed in this
        // comment can be right for a whole session, let alone across a release. The leftover section below
        // is the census that cannot drift, because it is computed from the registry it describes.
        //
        // AND NO DOCUMENTS. Not because this loop skips them: because m_Panels is a PanelRegistry and
        // cannot hold one. That is the whole task. Open documents are in Window -> Documents, where the
        // control is a radio and the close is an x, neither of which can be mistaken for "hide".
        // THE GROUPING IS DATA, NOT CONTROL FLOW, and that is a correction rather than a preference: a
        // submenu's body only runs while it is OPEN, so marking a panel "placed" from inside one reported
        // every panel behind a closed submenu as ungrouped. Measured — the first capture of this menu showed
        // ten panels under "NOT YET GROUPED" that are grouped. The census has to be readable without opening
        // anything, so it is stated once here and the drawing below refers to it.
        static constexpr const char* kLevelGroup[]     = { "Scene Outliner", "Collections", "Details",
                                                           "Scene Settings", "Scene Validation" };
        static constexpr const char* kContentGroup[]   = { "Assets", "Asset References", "Shader Library" };
        static constexpr const char* kOutputGroup[]    = { "Logs", "Lua Console", "History" };
        static constexpr const char* kViewportGroup[]  = { "Scene###scene" };
        // "Anim Graph", "Particle Editor", "UI Editor" and "Sequencer" are gone from these lists because
        // they are gone from the registry this menu loops over — a name left here would draw a group entry
        // for a panel that does not exist. They are opened from the component that holds them, in Details.
        // "Node Graph" is gone from here with the panel: the shader graph is a DOCUMENT, opened from the
        // `.dgraph` in the asset browser or from the palette's Open group, and the whole `Graph Editors`
        // submenu went with it rather than being left to draw an empty body.
        static constexpr const char* kSequencerGroup[] = { "Anim Layers" };
        // Localization sits with the tools rather than with the level: it is about the PROJECT's strings,
        // not about the scene that happens to be open, and it keeps answering after every scene change.
        static constexpr const char* kToolGroup[] = { "Modeling", "Model from Photos", "Build Settings",
                                                      "Localization" };

        std::unordered_set<std::string> placed;
        for ( const auto& group :
              { std::span<const char* const>( kLevelGroup ), std::span<const char* const>( kContentGroup ),
                std::span<const char* const>( kOutputGroup ), std::span<const char* const>( kViewportGroup ),
                std::span<const char* const>( kSequencerGroup ), std::span<const char* const>( kToolGroup ) } )
            for ( const char* name : group )
                placed.insert( name );

        auto panelItem = [&]( const char* name )
        {
            for ( auto& panel : m_Panels )
            {
                if ( panel->GetName() != name )
                    continue;

                // Same icon + stable ID as the panel title (the ###id keeps each menu entry unique/stable).
                const bool wasVisible = panel->GetVisibility();
                if ( ImGui::MenuItem( PanelDisplayTitle( panel->GetName() ).c_str(), "", &panel->GetVisibility(),
                                      true ) )
                {
                    // Ticking a contextual panel pins it open; unticking releases it back to the context.
                    if ( panel->IsContextual() )
                        panel->Pinned() = !wasVisible;
                }
                if ( panel->IsContextual() && ImGui::IsItemHovered() )
                    ImGui::SetTooltip( "Opens itself when its context appears. Ticking it keeps it open "
                                       "even when it does not apply." );
                return;
            }
        };

        auto group = [&]( std::span<const char* const> names )
        {
            for ( const char* name : names )
                panelItem( name );
        };

        ImGui::TextDisabled( "THE LEVEL" );
        group( kLevelGroup );

        ImGui::Separator();
        ImGui::TextDisabled( "CONTENT" );
        group( kContentGroup );

        ImGui::Separator();
        ImGui::TextDisabled( "OUTPUT" );
        group( kOutputGroup );
        // The Profiler is a window this layer draws itself rather than an IPanel, so it is a bool and not a
        // registry entry — it belongs in the group all the same, because the user is choosing between it and
        // the Logs beside it, not between two implementations.
        ImGui::MenuItem( ICON_MDI_CHART_BAR "  Profiler", "", &m_ShowProfiler, true );

        ImGui::Separator();

        // The nine that moved. Each is behind the job it belongs to rather than in a flat list beside
        // "Details" — a viewport is not a panel you tick, and a timeline is somewhere you go to author a
        // clip.
        if ( ImGui::BeginMenu( ICON_MDI_MONITOR "  Viewports" ) )
        {
            group( kViewportGroup );
            ImGui::Separator();
            // Multi-scene editing: a second, independent scene in its own live viewport (own SceneRenderer)
            // so a UI scene and the game scene can be worked on side by side. Focus a viewport to make its
            // scene active — the Outliner / Details / gizmo follow it.
            if ( ImGui::MenuItem( ICON_MDI_PLUS_BOX_MULTIPLE " New Scene View" ) )
                m_AddSceneViewRequested = true; // deferred to OnUpdate (allocates GPU resources)
            // A SECOND ANGLE, and FOUR of them. Both are about the ACTIVE scene, not a second one, which
            // is what the item above opens — the menu says so in the tooltips because the two read alike.
            if ( ImGui::MenuItem( ICON_MDI_BORDER_ALL " New Viewport (same scene)" ) )
                m_AddSceneViewportRequested = true;
            if ( ImGui::IsItemHovered() )
                ImGui::SetTooltip( "Another camera on the SAME world: same entities, same edits." );
            if ( ImGui::MenuItem( ICON_MDI_GRID " Four-Up Viewports" ) )
                m_ViewportGridRequested = true;
            if ( ImGui::IsItemHovered() )
                ImGui::SetTooltip( "Perspective, Top, Front and Right on the active scene, docked in a\n"
                                   "2x2 grid. The panes are ordinary windows: re-arrange them and save\n"
                                   "the result under View > Layouts." );
            if ( !m_ExtraScenes.empty() )
                ImGui::TextDisabled( "%d scene view(s) open + main", static_cast<int>( m_ExtraScenes.size() ) );
            // Closing from here does exactly what the window's x does — clear the VIEWPORT PANEL's
            // visibility — rather than tearing the scene down inside the ImGui pass. A scene view is a tool
            // panel bound to a scene, so visibility genuinely is its close signal; a document is the case
            // where that stopped being true, which is why documents have their own path.
            for ( const auto& doc : m_ExtraScenes )
            {
                const std::string item = std::string( ICON_MDI_CLOSE " Close " ) + doc->Name;
                if ( ImGui::MenuItem( item.c_str() ) && doc->Viewport )
                    doc->Viewport->GetVisibility() = false;
            }
            ImGui::EndMenu();
        }

        if ( ImGui::BeginMenu( ICON_MDI_CHART_TIMELINE "  Sequencer" ) )
        {
            // Both together: a clip is authored in the timeline and its layers, and two independent ticks
            // for one place you go was two decisions where there is one.
            group( kSequencerGroup );
            ImGui::EndMenu();
        }

        if ( ImGui::BeginMenu( ICON_MDI_HAMMER_WRENCH "  Tools" ) )
        {
            group( kToolGroup );
            ImGui::EndMenu();
        }

        if ( ImGui::BeginMenu( ICON_MDI_EYE "  Show" ) )
        {
            if ( ImGui::MenuItem( "Perf HUD", "", &EditorPreferences::Get().ShowPerfHud, true ) )
                EditorPreferences::Save(); // persist the toggle like the rest of the user prefs
            ImGui::EndMenu();
        }

        // ANYTHING THE GROUPS ABOVE DID NOT NAME. This is empty today and is not a placeholder: a panel
        // added later and forgotten here would otherwise have no menu entry at all, which is the same
        // "you cannot get it back" the documents had. It is visible precisely so that it gets fixed.
        {
            bool anyLeftover = false;
            for ( auto& panel : m_Panels )
            {
                if ( placed.count( panel->GetName() ) != 0 )
                    continue;
                if ( !anyLeftover )
                {
                    ImGui::Separator();
                    ImGui::TextDisabled( "NOT YET GROUPED" );
                    anyLeftover = true;
                }
                panelItem( panel->GetName().c_str() );
            }
        }

        ImGui::Separator();
        if ( ImGui::BeginMenu( "Layouts" ) )
        {
            for ( const auto& name : LayoutManager::List() )
            {
                if ( ImGui::MenuItem( name.c_str() ) )
                    LayoutManager::Load( name );
                if ( ImGui::IsItemHovered() && ImGui::IsMouseClicked( ImGuiMouseButton_Right ) )
                    LayoutManager::Delete( name ); // right-click removes it
            }
            ImGui::Separator();
            if ( ImGui::MenuItem( "Save Current Layout..." ) )
            {
                m_LayoutNameBuf[0]    = '\0';
                m_ShowSaveLayoutPopup = true;
            }
            if ( ImGui::MenuItem( "Reset to Default Layout" ) )
                m_ResetDefaultLayout = true;
            ImGui::EndMenu();
        }
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "Named docking layouts. Right-click a layout to delete it." );

        ImGui::EndMenu();
    }

    bool EditorLayer::SaveSceneTo( const std::string& path )
    {
        // THROUGH SaveToFile AND NOT A SECOND COPY OF IT. This function used to create the directory and
        // write SerializeToJson() itself, which was the same save spelled twice — and the moment the save
        // grew a step (a landscape writes its tile files beside the scene before the scene names them),
        // this copy would have written a .desce naming tile files that were never written.
        Desert::Core::SceneSerializer serializer( m_MainScene.get(), m_AssetManager.get() );
        if ( const auto written = serializer.SaveToFile( Common::Filepath( path ) ); !written )
        {
            LOG_ERROR( "[Scene] Could not write '{}': {}", path, written.GetError() );
            return false;
        }
        return true;
    }

    Common::Filepath EditorLayer::SceneSaveDestination() const
    {
        // The rule itself is a pure function in Editor/Core/SceneSaveRules.hpp so that a test can drive
        // the case that matters — a file whose name disagrees with the scene's — without an editor. This
        // call site supplies the two project paths it cannot know.
        return Common::Filepath( Editor::Core::Rules::SceneSaveDestination(
             m_OpenScenePath.generic_string(), m_MainScene->GetSceneName(),
             Common::Constants::Path::SCENE_PATH.generic_string(),
             Common::Constants::Extensions::SCENE_EXTENSION ) );
    }

    bool EditorLayer::SaveOpenScene()
    {
        const Common::Filepath destination = SceneSaveDestination();
        const auto             verdict     = Editor::Core::Rules::DecideAfterSceneSave(
             m_MainScene->Serialize( m_AssetManager.get(), destination ), m_MainScene->GetSceneName(),
             destination.string() );

        if ( verdict.MarkSceneSaved )
        {
            s_SavedRevision = CommandHistory::Get().Revision();
            // The scene now IS this file, whether it already was or was just given one. Without this a
            // new scene would re-derive its path on every save and a rename between two saves would
            // leave the user's work split across two files.
            m_OpenScenePath = destination;
        }

        if ( verdict.IsError )
        {
            LOG_ERROR( "[Scene] {}", verdict.Message );
        }
        else
        {
            LOG_INFO( "[Scene] {}", verdict.Message );
        }

        // Refresh the launcher's tile picture for this project. Only on a save that actually
        // happened — a refused save must not leave the launcher showing a world that was never
        // written. The failure is a toast and nothing more: the scene IS saved, and a launcher tile
        // without a picture is a state the launcher already draws.
        if ( verdict.MarkSceneSaved )
            if ( const auto thumbnail = WriteProjectThumbnail(); !thumbnail.IsSuccess() )
                Editor::ToastManager::Push( "The scene was saved, but the project thumbnail was not: " +
                                                 thumbnail.GetError(),
                                            Editor::ToastLevel::Warning );

        Editor::ToastManager::Push( verdict.Message,
                                    verdict.IsError ? Editor::ToastLevel::Error : Editor::ToastLevel::Success );
        return verdict.MayDiscardScene;
    }

    void EditorLayer::BuildStarterScene()
    {
        // A fresh project's first scene = a TEST PLAYGROUND: procedural sky + sun, a ground slab,
        // the classic PBR calibration rows (dielectric + metal, roughness 0..1), glass, an emissive
        // bloom probe, a shadow-caster cluster, coloured fill lights and a playable camera. Only
        // primitives + REAL material assets (created by name in the project's Materials/), so a new
        // project has zero external dependencies and every render feature has something to show on.
        auto prim = [&]( const std::string& name, Geometry::PrimitiveType type, glm::vec3 pos, glm::vec3 scale,
                         Assets::AssetHandle material = Common::UUID::Null() )
        {
            auto& e       = m_MainScene->CreateNewEntity( std::string( name ) );
            auto& smc     = e.AddComponent<ECS::StaticMeshComponent>();
            smc.Primitive = type;
            if ( material )
                smc.MaterialSlots.push_back( material );
            auto& tf       = e.GetComponent<ECS::TransformComponent>();
            // Demo scenes are authored in METRES for readability; a world unit is a centimetre, so every
            // position scales up. Scale does NOT: the primitive meshes themselves are one metre now.
            tf.Translation = pos * Common::Units::UnitsPerMetre;
            tf.Scale       = scale;
        };
        auto mat = [&]( const std::string& name, std::initializer_list<std::pair<const char*, glm::vec4>> params )
        {
            // .Handle drops the rest of the answer on purpose: this builder has nothing to do about a
            // demo material the user has since edited, and the disagreement is already reported by name
            // and value from inside the call. See Editor/Core/MaterialAssetUtils.hpp.
            return Editor::MaterialAssetUtils::FindOrCreatePBRMaterialAsset( m_AssetManager.get(), name, params )
                 .Handle;
        };

        // Sun (Translation encodes the direction the light TRAVELS; the sky uses -normalize(T)) + sky.
        // This is the site that MINTED the upside-down sun the shipped Sandbox/Starter scenes carried:
        // normalize(-0.35, -0.9, -0.25) reproduces their corrected value [-0.3509, -0.9023, -0.2506]
        // exactly, so a scene rebuilt from here now matches the one on disk instead of contradicting it.
        auto& sun = m_MainScene->CreateNewEntity( "Sun" );
        sun.AddComponent<ECS::DirectionLightComponent>();
        sun.GetComponent<ECS::TransformComponent>().Translation =
             glm::normalize( glm::vec3( -0.35f, -0.9f, -0.25f ) );

        auto& sky = m_MainScene->CreateNewEntity( "Sky" );
        sky.AddComponent<ECS::SkyAtmosphereComponent>();

        prim( "Ground", Geometry::PrimitiveType::Cube, { 0.0f, -0.1f, 0.0f }, { 24.0f, 0.2f, 24.0f },
              mat( "Starter_Ground", { { "AlbedoColor", { 0.55f, 0.55f, 0.58f, 1.0f } },
                                       { "RoughnessFactor", { 0.9f, 0, 0, 0 } } } ) );

        // PBR calibration rows: roughness 0 -> 1 in 6 steps; front row dielectric, back row metal.
        for ( int i = 0; i < 6; ++i )
        {
            const float roughness = static_cast<float>( i ) / 5.0f;
            const float x         = static_cast<float>( i ) * 1.4f - 3.5f;
            const auto  suffix    = std::to_string( i * 20 );

            prim( "PBR_Dielectric_" + suffix, Geometry::PrimitiveType::Sphere, { x, 0.6f, -3.0f },
                  glm::vec3( 0.55f ),
                  mat( "PBR_D_R" + suffix, { { "AlbedoColor", { 0.85f, 0.20f, 0.15f, 1.0f } },
                                             { "RoughnessFactor", { roughness, 0, 0, 0 } },
                                             { "MetallicFactor", { 0.0f, 0, 0, 0 } } } ) );
            prim( "PBR_Metal_" + suffix, Geometry::PrimitiveType::Sphere, { x, 0.6f, -4.6f }, glm::vec3( 0.55f ),
                  mat( "PBR_M_R" + suffix, { { "AlbedoColor", { 0.95f, 0.93f, 0.88f, 1.0f } },
                                             { "RoughnessFactor", { roughness, 0, 0, 0 } },
                                             { "MetallicFactor", { 1.0f, 0, 0, 0 } } } ) );
        }

        // Glass probe (refraction path) + emissive probe (bloom path — glows past the threshold).
        prim( "GlassSphere", Geometry::PrimitiveType::Sphere, { -2.5f, 1.0f, 0.5f }, glm::vec3( 1.2f ),
              mat( "Starter_Glass", { { "Transmission", { 0.9f, 0, 0, 0 } },
                                      { "IOR", { 1.5f, 0, 0, 0 } },
                                      { "GlassTint", { 0.8f, 0.95f, 1.0f, 1.0f } } } ) );
        prim( "EmissiveCube", Geometry::PrimitiveType::Cube, { 2.5f, 0.5f, 0.5f }, glm::vec3( 1.0f ),
              mat( "Starter_Emissive", { { "AlbedoColor", { 0.1f, 0.1f, 0.1f, 1.0f } },
                                         { "EmissiveColor", { 0.2f, 0.8f, 1.0f, 1.0f } },
                                         { "EmissiveIntensity", { 6.0f, 0, 0, 0 } } } ) );

        // Shadow-caster cluster (different silhouettes for the cascades to chew on).
        const auto clusterMat = mat( "Starter_Prop", { { "AlbedoColor", { 0.80f, 0.45f, 0.20f, 1.0f } },
                                                       { "RoughnessFactor", { 0.6f, 0, 0, 0 } } } );
        prim( "Cube", Geometry::PrimitiveType::Cube, { 0.0f, 0.5f, 1.5f }, glm::vec3( 1.0f ), clusterMat );
        prim( "Cylinder", Geometry::PrimitiveType::Cylinder, { 1.2f, 0.75f, 2.6f }, { 0.6f, 1.5f, 0.6f },
              clusterMat );
        prim( "Capsule", Geometry::PrimitiveType::Capsule, { -1.2f, 0.75f, 2.6f }, { 0.6f, 1.5f, 0.6f },
              clusterMat );

        // Coloured fills (shadowless accents) framing the set.
        auto pointLight = [&]( const char* name, glm::vec3 pos, glm::vec3 color, float intensity )
        {
            auto& e     = m_MainScene->CreateNewEntity( std::string( name ) );
            auto& d     = e.AddComponent<ECS::PointLightComponent>().Data;
            d.Color     = color;
            d.Intensity = intensity;
            d.Radius                                              = Common::Units::Metres( 12.0f );
            e.GetComponent<ECS::TransformComponent>().Translation = pos * Common::Units::UnitsPerMetre;
        };
        pointLight( "FillWarm", { 4.0f, 3.0f, 3.0f }, { 1.0f, 0.85f, 0.6f }, 5.0f );
        pointLight( "FillCool", { -4.0f, 2.5f, -1.0f }, { 0.4f, 0.6f, 1.0f }, 4.0f );

        // SDF text probe: emissive so it blooms like any emissive surface (no special path).
        {
            auto& label          = m_MainScene->CreateNewEntity( "Text" );
            auto& tc             = label.AddComponent<ECS::TextComponent>();
            tc.Text              = "Desert Engine";
            tc.Color             = { 0.55f, 0.85f, 1.0f, 1.0f };
            tc.Size              = Common::Units::Metres( 0.8f );
            tc.EmissiveIntensity = 2.5f; // past the bloom threshold -> the title glows
            auto& ttf            = label.GetComponent<ECS::TransformComponent>();
            ttf.Translation      = Common::Units::Metres( 1.0f ) * glm::vec3( -2.2f, 3.4f, -3.0f );
        }

        auto& camera = m_MainScene->CreateNewEntity( "Camera" );
        camera.AddComponent<ECS::CameraComponent>();
        camera.GetComponent<ECS::TransformComponent>().Translation =
             Common::Units::Metres( 1.0f ) * glm::vec3( 0.0f, 2.5f, 7.0f );
    }

    void EditorLayer::BuildCornellShowcase()
    {
        // Cornell-Box glass + direct-lighting showcase. A clear glass sphere sits in front of an orange
        // cube (visible THROUGH it); a point light backlights the set. Colours live in REAL material
        // assets in the mesh slots.
        //
        // IT DOES NOT DEMONSTRATE COLOUR BLEED, and this comment used to say it did ("Red/green walls
        // bleed onto the white objects (SSGI)"). Measured on the shipped scene: the whole screen-space
        // GI feature moves the frame by a mean of 0.11/255, the isolated indirect buffer reads 0.000 in
        // every statistic on the floor, and aiming the sun at the red wall changes nothing. The gather
        // shades each bouncing neighbour with the SUN alone, so a wall lit only by this scene's point
        // light emits exactly zero; and at the 290 cm from the floor to a wall the softened inverse
        // square already divides by 9.4 before the estimate is divided by its full sample count.
        // Bouncing point lights too is the owner's call, not a constant to raise — see
        // Desert/Tests/Engine/IndirectBounce, which pins the zero on the shipped shader text.
        // The colours are NOT literals here any more. They are Editor/Core/DemoMaterials.hpp, because a
        // value that only exists as an argument to a find-or-create is a value nothing can check the
        // shipped .demat against — which is exactly how CB_Red.demat came to be a chrome mirror while
        // this site asked for a diffuse red wall, invisibly, for as long as the file existed.
        auto tinted = [&]( const char* name, glm::vec3 pos, glm::vec3 scale, const char* matName )
        {
            auto& e       = m_MainScene->CreateNewEntity( std::string( name ) );
            auto& smc     = e.AddComponent<ECS::StaticMeshComponent>();
            smc.Primitive = Geometry::PrimitiveType::Cube;
            const auto* params = Editor::MaterialAssetUtils::FindDemoMaterial( matName );
            if ( !params )
            {
                LOG_ERROR( "[Cornell] '{}' is not in the demo material table; '{}' gets no material.", matName,
                           name );
            }
            else
            {
                smc.MaterialSlots.push_back( Editor::MaterialAssetUtils::FindOrCreatePBRMaterialAsset(
                                                  m_AssetManager.get(), matName, *params )
                                                  .Handle );
            }
            auto& tf       = e.GetComponent<ECS::TransformComponent>();
            tf.Translation = pos * Common::Units::UnitsPerMetre; // authored in metres (see BuildStarterScene)
            tf.Scale       = scale;
        };
        tinted( "CB_Floor", { 0, 0, 0 }, { 6, 0.2f, 6 }, "CB_White" );
        tinted( "CB_Back", { 0, 3, -3 }, { 6, 6, 0.2f }, "CB_White" );
        tinted( "CB_LeftRed", { -3, 3, 0 }, { 0.2f, 6, 6 }, "CB_Red" );
        tinted( "CB_RightGreen", { 3, 3, 0 }, { 0.2f, 6, 6 }, "CB_Green" );
        // Orange opaque cube directly behind the glass sphere (seen through it).
        tinted( "CB_OrangeCube", { 0, 1.3f, -1.2f }, { 1.4f, 1.4f, 1.4f }, "CB_Orange" );

        // Clear glass sphere in front of the cube.
        auto& glass    = m_MainScene->CreateNewEntity( std::string( "CB_GlassSphere" ) );
        auto& gsmc     = glass.AddComponent<ECS::StaticMeshComponent>();
        gsmc.Primitive = Geometry::PrimitiveType::Sphere;
        if ( const auto* glassParams = Editor::MaterialAssetUtils::FindDemoMaterial( "CB_Glass" ) )
        {
            gsmc.MaterialSlots.push_back( Editor::MaterialAssetUtils::FindOrCreatePBRMaterialAsset(
                                               m_AssetManager.get(), "CB_Glass", *glassParams )
                                               .Handle );
        }
        auto& gtf       = glass.GetComponent<ECS::TransformComponent>();
        gtf.Translation = Common::Units::Metres( 1.0f ) * glm::vec3( 0.0f, 1.5f, 0.7f );
        gtf.Scale       = glm::vec3( 1.6f );

        // Point light BEHIND the objects (backlight / rim).
        auto& pl      = m_MainScene->CreateNewEntity( std::string( "CB_BackLight" ) );
        auto& pld     = pl.AddComponent<ECS::PointLightComponent>().Data;
        pld.Color     = glm::vec3( 1.0f, 0.85f, 0.6f );
        pld.Intensity = 8.0f;
        pld.Radius    = Common::Units::Metres( 12.0f );
        pl.GetComponent<ECS::TransformComponent>().Translation =
             Common::Units::Metres( 1.0f ) * glm::vec3( 0.0f, 2.5f, -2.5f );

        // The baked scene must carry its OWN sun — it no longer piggybacks on startup state.
        // (Exactly one: a second directional light would overflow the single-light UB.)
        if ( m_MainScene->GetRegistry().view<ECS::DirectionLightComponent>().size() == 0 )
        {
            auto& sun = m_MainScene->CreateNewEntity( "CB_Sun" );
            sun.AddComponent<ECS::DirectionLightComponent>();
            // Translation is the direction the light TRAVELS, so a sun ABOVE the horizon points DOWN.
            // This site used to author +Y and put its own sun 57.7 degrees underground; the committed
            // CornellDemo scene carries the corrected value and this now reproduces it exactly
            // (normalize(0.6, -1, 0.2) == [0.5071, -0.8452, 0.1690]).
            sun.GetComponent<ECS::TransformComponent>().Translation =
                 glm::normalize( glm::vec3( 0.6f, -1.0f, 0.2f ) );
        }
    }

    void EditorLayer::LoadScene( const Common::Filepath& path )
    {
        m_SceneLoadRequested = path;
    }

    void EditorLayer::NewSceneInternal()
    {
        // Same teardown as a load, minus the deserialize: clear the current scene to empty and re-init. The
        // Scene object is REUSED (panels hold its shared_ptr), so their references stay valid.
        EngineContext::GetInstance().GetDevice()->WaitIdle();

        CommandHistory::Get().Clear();
        s_SavedRevision = CommandHistory::Get().Revision();

        Core::SelectionManager::ClearSelection();
        m_MainScene->Clear();
        m_MainScene->SetSceneName( "New Scene" );
        // A new scene is not any file yet. Left pointing at the previous one, the first Ctrl+S would
        // overwrite the scene the user had just moved away from with an empty world.
        m_OpenScenePath.clear();
        if ( const auto inited = m_MainScene->Init(); !inited.IsSuccess() )
            LOG_ERROR( "[EditorLayer] new scene failed to initialise: {}", inited.GetError() );

        // Rebuild the render registry against the fresh registry (its dtor unregisters editor passes by name).
        m_RenderRegistry.reset();
        m_RenderRegistry = std::make_unique<Render::RenderRegistry>( m_MainScene );

        Editor::ToastManager::Push( "New scene", Editor::ToastLevel::Success );
        LOG_INFO( "[Scene] New empty scene" );
    }

    void EditorLayer::LoadSceneInternal( const Common::Filepath& path )
    {
        if ( !std::filesystem::exists( path ) )
        {
            LOG_ERROR( "Scene file does not exist: {0}", path.string() );
            return;
        }

        // ASKED BEFORE ANYTHING IS DESTROYED, and this is the call site that makes it worth asking. Below
        // this point the undo history is dropped and the open scene is cleared; a file the loader will
        // refuse - an old autosave, a scene saved by an older build - would then have cost the user the
        // scene they had and given them nothing. So an unloadable file leaves the editor exactly as it is
        // and says why, with the command that fixes the file.
        Desert::Core::SceneLoadPhases phases( fmt::format( "Open '{}'", path.filename().string() ) );

        const auto contentRead = Common::Utils::FileSystem::ReadFileContent( path );
        if ( !contentRead )
        {
            LOG_ERROR( "{0}", contentRead.GetError() );
            Editor::ToastManager::Push( "Scene not loaded — the file could not be read (see the log)",
                                        Editor::ToastLevel::Error );
            return;
        }
        const std::string& content = contentRead.GetValue();
        phases.Lap( "read the file", content.size() );
        if ( const auto loadable = Desert::Core::ParseLoadableScene( path.string(), content ); !loadable )
        {
            LOG_ERROR( "{0}", loadable.GetError() );
            Editor::ToastManager::Push( "Scene not loaded — see the log (it names the SceneMigrator command)",
                                        Editor::ToastLevel::Error );
            return;
        }

        phases.Lap( "version gate before tearing down the open scene", content.size() );

        // Wait for GPU to be idle before destroying resources mid-frame
        EngineContext::GetInstance().GetDevice()->WaitIdle();

        // The undo history refers to entities of the OLD scene — none of it applies anymore.
        CommandHistory::Get().Clear();
        s_SavedRevision = CommandHistory::Get().Revision(); // a freshly loaded scene is "clean"

        phases.Lap( "wait for the GPU, drop the undo history", 0 );
        const std::size_t outgoing = m_MainScene->GetAllEntities().size();
        m_MainScene->Clear();
        phases.Lap( "clear the open scene", outgoing );

        Desert::Core::SceneSerializer serializer( m_MainScene.get(), m_AssetManager.get() );
        // Cannot fire - the same text passed the same check above, before anything was torn down. It is
        // reported and NOT returned from on purpose: the scene is already cleared by this point, so the
        // rebuild below is what leaves the editor in a coherent (empty) state rather than one holding a
        // render registry for entities that no longer exist.
        if ( const auto loaded = serializer.DeserializeFromJson( content, path.string() ); !loaded )
        {
            LOG_ERROR( "{0}", loaded.GetError() );
            Editor::ToastManager::Push( "Scene failed to load — see the log", Editor::ToastLevel::Error );
        }
        const std::size_t incoming = m_MainScene->GetAllEntities().size();
        phases.Lap( "deserialize (its own phases are logged above)", incoming );

        if ( const auto inited = m_MainScene->Init(); !inited.IsSuccess() )
        {
            LOG_ERROR( "[EditorLayer] loaded scene failed to initialise: {}", inited.GetError() );
            Editor::ToastManager::Push( "Scene could not be initialised — see the log",
                                        Editor::ToastLevel::Error );
        }
        phases.Lap( "initialise the scene", incoming );

        // Destroy the old registry FIRST: its destructor unregisters the editor passes by name, and
        // assignment would run it after the new registry already re-registered them.
        m_RenderRegistry.reset();
        m_RenderRegistry = std::make_unique<Render::RenderRegistry>( m_MainScene );
        phases.Lap( "rebuild the render registry", incoming );
        phases.LogSummary();

        // THE OPEN SCENE IS NOW THIS FILE, and it is set HERE rather than at the top of the function on
        // purpose: every early return above leaves a scene that was NOT replaced, and adopting a path for
        // a load that refused would point the next Ctrl+S at a file the user never opened.
        m_OpenScenePath = path;

        // Update recent scenes
        auto it = std::find( m_RecentScenes.begin(), m_RecentScenes.end(), path );
        if ( it != m_RecentScenes.end() )
        {
            m_RecentScenes.erase( it );
        }
        m_RecentScenes.insert( m_RecentScenes.begin(), path );

        if ( m_RecentScenes.size() > 5 )
        {
            m_RecentScenes.pop_back();
        }
    }

    void EditorLayer::DrawWindowMenu()
    {
        namespace ImGui = ::ImGui;

        if ( !ImGui::BeginMenu( "Window" ) )
            return;

        // A SECOND MENU, BECAUSE THESE ARE A SECOND KIND OF THING. The View menu ticks tools on and off;
        // this one lists what is open and lets you go to it or close it. Putting documents back among the
        // ticks is the defect, not the layout.
        if ( m_OpenDocuments.Empty() )
        {
            ImGui::TextDisabled( "No document open" );
            ImGui::TextDisabled( "Double-click an asset in the Content Browser." );
        }
        else
        {
            ImGui::TextDisabled( "OPEN DOCUMENTS \xe2\x80\x94 %zu", m_OpenDocuments.Count() );

            // The x column is placed against the WIDEST row, measured, not against the popup's content
            // region: a menu auto-sizes to its widest item, so asking the region where the right edge is
            // gives an answer that depends on the answer. (Measured — the first capture of this menu had no
            // x on any row, because every one of them was placed past the edge it was helping to define.)
            float widestRow = 0.0f;
            for ( const auto& document : m_OpenDocuments )
            {
                const std::string measured = std::string( ICON_MDI_RADIOBOX_MARKED ) + "  " +
                                             m_SubjectEditors.Icon( document->Subject(), kUnknownDocumentIcon ) +
                                             std::string( "  " ) + DocumentDisplayName( document->GetName() );
                widestRow = std::max( widestRow, ImGui::CalcTextSize( measured.c_str() ).x );
            }

            std::vector<SubjectId> closeRequests;
            for ( const SubjectId& subject : m_DocumentWell.MostRecentOrder() )
            {
                const ISubjectDocument* document = m_OpenDocuments.Find( subject );
                if ( !document )
                    continue;

                ImGui::PushID( static_cast<int>( std::hash<SubjectId>{}( subject ) & 0x7fffffff ) );

                // A RADIO, NOT A CHECKBOX, and the difference is the whole argument of this task written
                // in one glyph. A tick says "shown / hidden" and invites the user to untick it — which is
                // exactly what used to destroy the document. A radio says "this is the one you are in",
                // which is true, is the only thing picking a row can mean, and offers no way to un-pick.
                const bool        active = ( subject == m_FocusedDocument );
                const std::string label =
                     std::string( active ? ICON_MDI_RADIOBOX_MARKED : ICON_MDI_RADIOBOX_BLANK ) + "  " +
                     m_SubjectEditors.Icon( document->Subject(), kUnknownDocumentIcon ) + std::string( "  " ) +
                     DocumentDisplayName( document->GetName() );

                if ( ImGui::MenuItem( label.c_str() ) )
                    FocusDocument( subject );

                ImGui::SameLine( ImGui::GetCursorPosX() + widestRow + 24.0f );
                if ( ImGui::SmallButton( ICON_MDI_CLOSE ) )
                    closeRequests.push_back( subject );
                if ( ImGui::IsItemHovered() )
                    ImGui::SetTooltip( "Close this document. It is destroyed, and its renderer slot (if it "
                                       "holds one) is returned." );

                ImGui::PopID();
            }

            ImGui::Separator();
            if ( ImGui::MenuItem( ICON_MDI_CLOSE_BOX_OUTLINE "  Close All Documents" ) )
                RequestCloseAllDocuments();

            for ( const SubjectId& subject : closeRequests )
                RequestDocumentClose( subject, "closed from Window \xe2\x96\xb8 Documents" );
        }

        // NO "SAVE ALL" HERE, AND ITS ABSENCE IS DELIBERATE.
        //
        // The mock draws one. It cannot be built honestly yet: ISubjectDocument declares no Save() and no
        // IsDirty(), the editor's single dirty flag belongs to the SCENE (a CommandHistory revision), and a
        // material document writes straight into the in-memory asset as a slider moves. "Save All" would
        // therefore have to mean "rewrite every open document's file whether or not it changed", it could
        // not report how many of them needed it, and it would touch mtimes the asset hot-reload watches.
        // A per-document dirty flag with a working copy behind it is the next task; the item waits for it.

        ImGui::EndMenu();
    }

    void EditorLayer::DrawScenesMenu()
    {
        namespace ImGui = ::ImGui;

        if ( !ImGui::BeginMenu( "Scenes" ) )
        {
            return;
        }

        if ( ImGui::MenuItem( "Load Scene..." ) )
        {
            PrepareScenePopup();
            m_OpenScenePopup = true;
        }

        // Opening and closing scene VIEWS moved to View -> Viewports, next to the Scene panel's own toggle.
        // This menu is about scene FILES; a viewport is not one, and two menus offering the same New Scene
        // View was two places to keep in step for one action.

        if ( !m_RecentScenes.empty() )
        {
            ImGui::Separator();
            ImGui::TextDisabled( "Recent Scenes" );

            for ( const auto& path : m_RecentScenes )
            {
                const std::string label = SceneLabel( path );
                if ( ImGui::MenuItem( label.c_str() ) )
                {
                    LoadScene( path );
                }
                Utils::ImGuiUtilities::Tooltip( path.string().c_str() );
            }
        }

        ImGui::EndMenu();
    }

    void EditorLayer::DrawGraphicsMenu()
    {
        namespace ImGui = ::ImGui;

        if ( !ImGui::BeginMenu( "Graphics" ) )
        {
            return;
        }

        ImGui::EndMenu();
    }

    void EditorLayer::DrawAboutMenu()
    {
        namespace ImGui = ::ImGui;

        if ( !ImGui::BeginMenu( "About" ) )
        {
            return;
        }

        ImGui::TextUnformatted( "Desert Engine Editor" );
        ImGui::EndMenu();
    }

    void EditorLayer::DrawPlayButton( const ImVec2& size )
    {
        namespace ImGui    = ::ImGui;
        using SceneState   = ::Desert::Core::Scene::SceneState;
        const bool playing = m_MainScene->GetState() != SceneState::Edit;

        if ( playing )
            ImGui::PushStyleColor( ImGuiCol_Text, ThemeManager::GetSelectedColor() );

        // One toggle: Play when editing, Stop (restore the snapshot) when playing/paused.
        if ( ImGui::Button( playing ? ICON_MDI_STOP : ICON_MDI_PLAY, size ) )
        {
            if ( playing )
                m_PendingSceneStop = true; // deferred to OnUpdate (between frames) — see m_PendingSceneStop
            else
                OnScenePlay();
        }
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( playing ? "Stop" : "Play" );

        if ( playing )
            ImGui::PopStyleColor();
    }

    void EditorLayer::DrawPauseButton( const ImVec2& size )
    {
        namespace ImGui   = ::ImGui;
        using SceneState  = ::Desert::Core::Scene::SceneState;
        const bool paused = m_MainScene->GetState() == SceneState::Paused;
        const bool active = m_MainScene->GetState() != SceneState::Edit; // pause only matters while playing

        if ( !active )
            ImGui::BeginDisabled();
        if ( paused )
            ImGui::PushStyleColor( ImGuiCol_Text, ThemeManager::GetSelectedColor() );

        if ( ImGui::Button( ICON_MDI_PAUSE, size ) )
            OnScenePauseToggle();
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( paused ? "Resume" : "Pause" );

        if ( paused )
            ImGui::PopStyleColor();
        if ( !active )
            ImGui::EndDisabled();
    }

    namespace
    {
        // One static box = mesh (Cube primitive) + Box collider + Static body, as a child of `parent`.
        // The Cube primitive spans 2 units, so the visual size is 2*scale and the collider half-extents == scale
        // (matches the demo ground). Child colliders are placed at their WORLD pose by PhysicsECSSystem.
        void AddHousePart( ::Desert::Core::Scene* scene, ::Desert::ECS::Entity parent, const char* name,
                           const glm::vec3& localPos, const glm::vec3& scale )
        {
            using namespace ::Desert;
            auto& e                                              = scene->CreateNewEntity( std::string( name ) );
            e.AddComponent<ECS::StaticMeshComponent>().Primitive = Geometry::PrimitiveType::Cube;
            auto& t                                              = e.GetComponent<ECS::TransformComponent>();
            t.Translation                                        = localPos * Common::Units::UnitsPerMetre;
            t.Scale                                              = scale;
            auto& col                                            = e.AddComponent<ECS::ColliderComponent>();
            col.Data.Shape                                       = Physics::ShapeType::Box;
            // The Cube primitive spans one metre, so a box of Scale s reaches 50*s units either way.
            col.Data.HalfExtents                                 = scale * ( Common::Units::UnitsPerMetre * 0.5f );
            e.AddComponent<ECS::RigidBodyComponent>().Data.Type  = Physics::BodyType::Static;
            scene->Attach( parent, e );
        }
    } // namespace

    // Builds a walkable greybox HOUSE (floor-less; sits on the demo ground): 4 walls (front wall has a
    // doorway) + a flat roof, each a static collider so the character walks in through the door and is blocked
    // by walls. All parented under one "House" root (a ready prefab root). 2-unit-cube convention: dims = 2*scale.
    void EditorLayer::BuildHouse( const glm::vec3& origin )
    {
        using namespace ::Desert;

        // By VALUE: creating the wall children below reallocates the entity store; a reference would dangle.
        ECS::Entity house                                         = m_MainScene->CreateNewEntity( "House" );
        house.GetComponent<ECS::TransformComponent>().Translation = origin;

        // Interior ~8x8 m, walls 3 m tall, 0.2 m thick. Half-sizes (= scale, since the cube is 2 units):
        AddHousePart( m_MainScene.get(), house, "Wall_Back", { 0.0f, 1.5f, -4.0f }, { 4.0f, 1.5f, 0.1f } );
        AddHousePart( m_MainScene.get(), house, "Wall_Left", { -4.0f, 1.5f, 0.0f }, { 0.1f, 1.5f, 4.0f } );
        AddHousePart( m_MainScene.get(), house, "Wall_Right", { 4.0f, 1.5f, 0.0f }, { 0.1f, 1.5f, 4.0f } );
        // Front wall with a centered doorway (1.2 m wide, 2.2 m tall): two side segments + a lintel above.
        AddHousePart( m_MainScene.get(), house, "Wall_FrontL", { -2.3f, 1.5f, 4.0f }, { 1.7f, 1.5f, 0.1f } );
        AddHousePart( m_MainScene.get(), house, "Wall_FrontR", { 2.3f, 1.5f, 4.0f }, { 1.7f, 1.5f, 0.1f } );
        AddHousePart( m_MainScene.get(), house, "Door_Lintel", { 0.0f, 2.6f, 4.0f }, { 0.6f, 0.4f, 0.1f } );
        // Flat roof (slight overhang).
        AddHousePart( m_MainScene.get(), house, "Roof", { 0.0f, 3.1f, 0.0f }, { 4.2f, 0.1f, 4.2f } );

        LOG_INFO( "[Demo] House built at ({}, {}, {}) — walk in through the +Z doorway.", origin.x, origin.y,
                  origin.z );
    }

    void EditorLayer::BuildCharacterDemoScene()
    {
        using namespace ::Desert;

        // --- Sun (directional light) — DirectionLight stores its DIRECTION in TransformComponent.Translation
        {
            auto& sun         = m_MainScene->CreateNewEntity( "Sun" );
            auto& dl          = sun.AddComponent<ECS::DirectionLightComponent>();
            dl.Data.Color     = { 1.0f, 0.97f, 0.9f };
            dl.Data.Intensity = 3.0f;
            sun.GetComponent<ECS::TransformComponent>().Translation = { -0.4f, -1.0f, -0.5f }; // direction
        }

        // --- Ground: a flat static box the character stands on (mesh + Box collider + Static body)
        {
            auto& ground                                              = m_MainScene->CreateNewEntity( "Ground" );
            ground.AddComponent<ECS::StaticMeshComponent>().Primitive = Geometry::PrimitiveType::Cube;
            auto& gt              = ground.GetComponent<ECS::TransformComponent>();
            gt.Translation        = Common::Units::Metres( 1.0f ) * glm::vec3( 0.0f, -0.5f, 0.0f ); // top at y=0
            gt.Scale              = { 20.0f, 0.5f, 20.0f };
            auto& gcol            = ground.AddComponent<ECS::ColliderComponent>();
            gcol.Data.Shape       = Physics::ShapeType::Box;
            // Half-extents of the scaled 1 m cube: 50 units per unit of Scale.
            gcol.Data.HalfExtents = gt.Scale * ( Common::Units::UnitsPerMetre * 0.5f );
            ground.AddComponent<ECS::RigidBodyComponent>().Data.Type = Physics::BodyType::Static;
        }

        // --- A few static obstacle boxes to walk into / around
        for ( int i = 0; i < 3; ++i )
        {
            auto& box = m_MainScene->CreateNewEntity( "Obstacle" + std::to_string( i ) );
            box.AddComponent<ECS::StaticMeshComponent>().Primitive = Geometry::PrimitiveType::Cube;
            auto& bt                                               = box.GetComponent<ECS::TransformComponent>();
            bt.Translation        = Common::Units::Metres( 1.0f ) * glm::vec3( -4.0f + i * 4.0f, 0.5f, -5.0f );
            auto& bcol            = box.AddComponent<ECS::ColliderComponent>();
            bcol.Data.Shape       = Physics::ShapeType::Box;
            bcol.Data.HalfExtents = glm::vec3( Common::Units::Metres( 0.5f ) );
            box.AddComponent<ECS::RigidBodyComponent>().Data.Type  = Physics::BodyType::Static;
        }

        // --- Player: a Character Controller (the physics capsule). NO RigidBody/Collider — the controller
        // IS the physics. The player entity is left UNSCALED so its children (visual body + camera) don't
        // inherit a non-uniform scale (which would skew/displace a child camera and its gizmo). Starts above
        // the ground so it drops on Play. By VALUE: creating children below can reallocate the entity store.
        ECS::Entity player = m_MainScene->CreateNewEntity( "Player" );
        {
            auto& cc       = player.AddComponent<ECS::CharacterControllerComponent>();
            cc.Data.Radius = Common::Units::Metres( 0.3f );
            cc.Data.Height = Common::Units::Metres( 1.8f );
            // Move/jump/look speeds are the SCRIPT's Properties now (Details ▸ Script), not the controller.
            player.GetComponent<ECS::TransformComponent>().Translation =
                 Common::Units::Metres( 1.0f ) * glm::vec3( 0.0f, 3.0f, 0.0f );
            // Movement + mouse-look are now a Lua SCRIPT (engine only executes the physics it asks for).
            {
                ECS::ScriptSlot slot;
                // NAMES THE SCRIPT THAT EXISTS. This joined the scripts root to a lower-case, underscored
                // player-controller file name that no tree of this project has ever contained, so the
                // template's player was created with a slot that could only ever log "script not found"
                // on Play. The two are now held together by
                // ContentScanners.EveryLuaFileNamedInTheEditorExists rather than by whoever looks next.
                // (That test reads raw source, so the dead spelling is described here and not quoted.)
                // Through StableKeyForPath, like the picker: the slot stores a ROOT-TAGGED KEY, and a
                // template that stored the rooted spelling would author the very defect I9 migrated
                // three scenes out of — a reference that resolves here and nowhere a game ships to.
                slot.ScriptKey = Common::AssetHandle::StableKeyForPath( Common::Constants::Path::SCRIPT_PATH /
                                                                        "Examples/PlayerController.lua" );
                player.AddComponent<ECS::ScriptComponent>().Scripts.push_back( std::move( slot ) );
            }
        }

        // --- Visual body: a CHILD holding the procedural skinned HUMANOID (head/torso/2 arms/2 legs). Its
        // mesh origin is at the feet, so we drop it by the capsule half-height (~0.9) to stand on the capsule
        // bottom. An AnimationComponent is attached so it animates once idle/walk/run clips are registered.
        {
            auto& body = m_MainScene->CreateNewEntity( "PlayerBody" );
            body.AddComponent<ECS::SkinnedMeshComponent>().MeshHandle =
                 Geometry::ProceduralCharacterFactory::GetHumanoidMesh();
            body.AddComponent<ECS::AnimationComponent>();
            body.GetComponent<ECS::TransformComponent>().Translation =
                 Common::Units::Metres( 1.0f ) * glm::vec3( 0.0f, -0.9f, 0.0f );
            m_MainScene->Attach( player, body );
        }

        // --- Camera: a CHILD of the (unscaled) player. Offset behind+above = 3rd person; move it to ~(0,
        // 0.7, 0) with rotation 0 for 1st person. Follows the player via the hierarchy (WORLD transform).
        {
            auto& cam            = m_MainScene->CreateNewEntity( "PlayerCamera" );
            auto& cd             = cam.AddComponent<ECS::CameraComponent>();
            cd.Data.IsMainCamera = true;
            auto& ct             = cam.GetComponent<ECS::TransformComponent>();
            ct.Translation       = Common::Units::Metres( 1.0f ) * glm::vec3( 0.0f, 1.5f, 7.0f ); // 3rd person
            ct.Rotation          = { glm::radians( -10.0f ), 0.0f, 0.0f }; // look slightly down at the player
            m_MainScene->Attach( player, cam );
        }

        BuildHouse( Common::Units::Metres( 1.0f ) * glm::vec3( 12.0f, 0.0f, 0.0f ) ); // greybox house aside

        LOG_INFO( "[Demo] Character demo scene built — press Play, then WASD to move + Space to jump." );
    }

    void EditorLayer::OnScenePlay()
    {
        using SceneState = ::Desert::Core::Scene::SceneState;
        if ( m_MainScene->GetState() != SceneState::Edit )
            return;
        // Snapshot the authored scene so Stop can restore it exactly (play-time edits are discarded).
        // Timed like a load, because it is the other half of the round trip Stop pays: the snapshot is a
        // full write of the scene, and it was where a 50 000-record world spent seven minutes.
        Desert::Core::SceneLoadPhases phases( "Play start" );
        Desert::Core::SceneSerializer serializer( m_MainScene.get(), m_AssetManager.get() );
        m_PlaySnapshot = serializer.SerializeToJson();
        phases.Lap( "write the Play snapshot", m_MainScene->GetAllEntities().size() );
        // AFTER the snapshot: streaming destroys every cell outside the camera's neighbourhood, and Stop
        // restores what the snapshot holds. A world that cannot stream does not play half-loaded - it does not
        // play, and says why.
        auto streamer = Desert::Core::WorldStreamer::Begin( *m_MainScene, *m_AssetManager, m_PlaySnapshot );
        phases.Lap( "begin the world streamer", m_MainScene->GetAllEntities().size() );
        phases.LogSummary();
        if ( !streamer )
        {
            LOG_ERROR( "[Scene] Play refused: {0}", streamer.GetError() );
            Editor::ToastManager::Push( "Play refused: the world could not stream — see the log",
                                        Editor::ToastLevel::Error );
            m_PlaySnapshot.clear();
            return;
        }
        m_WorldStreamer    = streamer.ExtractValue();
        m_WorldStreamClock = 0.0;
        // Play-time changes are discarded on Stop anyway, and the Stop restore recreates every entity —
        // an undo stack recorded against the authored scene must not fire into either state.
        CommandHistory::Get().Clear();
        m_MainScene->SetState( SceneState::Play );
        m_EditorState = EditorState::Play;
    }

    void EditorLayer::OnSceneStop()
    {
        using SceneState = ::Desert::Core::Scene::SceneState;
        if ( m_MainScene->GetState() == SceneState::Edit || m_PlaySnapshot.empty() )
            return;

        Desert::Core::SceneLoadPhases phases( "Stop restore" );
        EngineContext::GetInstance().GetDevice()->WaitIdle();
        CommandHistory::Get().Clear(); // anything recorded during Play targets entities about to be rebuilt
        m_WorldStreamer.reset();       // before Clear: the snapshot below brings every cell back
        phases.Lap( "wait for the GPU, drop the undo history and the streamer", 0 );
        const std::size_t outgoing = m_MainScene->GetAllEntities().size();
        m_MainScene->Clear();
        phases.Lap( "clear the played scene", outgoing );

        Desert::Core::SceneSerializer serializer( m_MainScene.get(), m_AssetManager.get() );
        // NOT A FILE, and it cannot be at an old version: this text came out of SerializeToJson() a moment
        // ago in this same build, which stamps the head of both version integers. It is named rather than
        // pathed so that if it ever DOES fail, the message says which of the two things called "loading a
        // scene" broke - restoring the pre-Play state is not opening a file, and reading "Play snapshot" in
        // the log is the difference between one minute of diagnosis and twenty.
        if ( const auto restored = serializer.DeserializeFromJson( m_PlaySnapshot, "<Play snapshot>" ); !restored )
        {
            LOG_ERROR( "[Scene] Play snapshot could not be restored: {0}", restored.GetError() );
            Editor::ToastManager::Push( "Play snapshot could not be restored — see the log",
                                        Editor::ToastLevel::Error );
        }
        const std::size_t incoming = m_MainScene->GetAllEntities().size();
        phases.Lap( "deserialize the snapshot (its own phases are logged above)", incoming );
        if ( const auto inited = m_MainScene->Init(); !inited.IsSuccess() )
        {
            LOG_ERROR( "[EditorLayer] scene failed to initialise after Play: {}", inited.GetError() );
            Editor::ToastManager::Push( "Scene could not be initialised after Play — see the log",
                                        Editor::ToastLevel::Error );
        }
        phases.Lap( "initialise the scene", incoming );

        // Destroy the old registry FIRST: its destructor unregisters the editor passes by name, and
        // assignment would run it after the new registry already re-registered them.
        m_RenderRegistry.reset();
        m_RenderRegistry = std::make_unique<Render::RenderRegistry>( m_MainScene );
        phases.Lap( "rebuild the render registry", incoming );
        phases.LogSummary();

        m_MainScene->SetState( SceneState::Edit );
    }

    void EditorLayer::OnScenePauseToggle()
    {
        using SceneState = ::Desert::Core::Scene::SceneState;
        if ( m_MainScene->GetState() == SceneState::Play )
            m_MainScene->SetState( SceneState::Paused );
        else if ( m_MainScene->GetState() == SceneState::Paused )
            m_MainScene->SetState( SceneState::Play );
    }

    void EditorLayer::DrawOpenScenePopup()
    {
        namespace ImGui = ::ImGui;

        if ( m_OpenScenePopup )
        {
            ImGui::OpenPopup( "Open Scene" );
            m_OpenScenePopup = false;
        }

        ImGui::SetNextWindowPos( ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing,
                                 ImVec2( 0.5f, 0.5f ) );

        if ( ImGui::BeginPopupModal( "Open Scene", nullptr, ImGuiWindowFlags_AlwaysAutoResize ) )
        {
            ImGui::TextUnformatted( "Select Scene" );
            ImGui::Separator();

            ImGui::SetNextItemWidth( 450.0f );
            ImGui::InputTextWithHint( "##SceneFilter", ICON_MDI_MAGNIFY " Filter", m_SceneFilter,
                                      sizeof( m_SceneFilter ) );

            ImGui::BeginChild( "SceneList", ImVec2( 450, 300 ), true );

            const std::string filter  = Lowercased( m_SceneFilter );
            bool              loadNow = false; // double-click = pick AND load, in one gesture
            std::string       shownFolder;     // last folder header drawn
            bool              haveFolder = false;
            bool              anyShown   = false;

            for ( int i = 0; i < static_cast<int>( m_AvailableScenes.size() ); ++i )
            {
                const std::string label = SceneLabel( m_AvailableScenes[i] );
                if ( !filter.empty() && Lowercased( label ).find( filter ) == std::string::npos )
                    continue;

                // Split "Folder/Sub/Scene.desce" into its folder header and the scene's own name.
                const size_t      slash  = label.find_last_of( '/' );
                const std::string folder = slash == std::string::npos ? std::string() : label.substr( 0, slash );
                const std::string name   = slash == std::string::npos ? label : label.substr( slash + 1 );

                if ( !haveFolder || folder != shownFolder )
                {
                    if ( anyShown )
                        ImGui::Spacing();
                    if ( folder.empty() )
                        ImGui::TextDisabled( ICON_MDI_FOLDER_HOME " Scenes" );
                    else
                        ImGui::TextDisabled( ICON_MDI_FOLDER " %s", folder.c_str() );
                    shownFolder = folder;
                    haveFolder  = true;
                }

                anyShown = true;

                ImGui::PushID( i ); // two folders may hold the same filename
                ImGui::Indent( 12.0f );
                if ( ImGui::Selectable( name.c_str(), m_SelectedSceneIndex == i,
                                        ImGuiSelectableFlags_AllowDoubleClick ) )
                {
                    m_SelectedSceneIndex = i;
                    if ( ImGui::IsMouseDoubleClicked( ImGuiMouseButton_Left ) )
                        loadNow = true;
                }
                if ( ImGui::IsItemHovered() )
                    ImGui::SetTooltip( "%s", m_AvailableScenes[i].string().c_str() );
                ImGui::Unindent( 12.0f );
                ImGui::PopID();
            }

            if ( !anyShown )
                ImGui::TextDisabled( m_AvailableScenes.empty() ? "No scenes found" : "No match" );

            ImGui::EndChild();

            ImGui::Separator();

            const bool hasSelection =
                 m_SelectedSceneIndex >= 0 && m_SelectedSceneIndex < static_cast<int>( m_AvailableScenes.size() );

            if ( ImGui::Button( "Load", ImVec2( 120, 0 ) ) || loadNow )
            {
                if ( hasSelection )
                {
                    LoadScene( m_AvailableScenes[m_SelectedSceneIndex] );
                }

                ImGui::CloseCurrentPopup();
            }

            ImGui::SameLine();

            if ( ImGui::Button( "Cancel", ImVec2( 120, 0 ) ) )
            {
                ImGui::CloseCurrentPopup();
            }

            ImGui::EndPopup();
        }
    }

    // Guard for the scene handed over by a panel (viewport drop / asset double-click): the document is
    // about to be replaced, and unlike the menu path this can be triggered by a slip of the mouse. Only
    // shown when there is something to lose — a clean scene opens straight away.
    void EditorLayer::DrawConfirmOpenScenePopup()
    {
        namespace ImGui = ::ImGui;

        if ( m_ConfirmOpenScenePopup )
        {
            ImGui::OpenPopup( "Open Scene?" );
            m_ConfirmOpenScenePopup = false;
            // A failure belongs to the attempt that produced it. Without this a save that failed once
            // would keep warning about a scene the user has since saved by hand.
            m_SaveAndOpenError.clear();
        }

        ImGui::SetNextWindowPos( ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing,
                                 ImVec2( 0.5f, 0.5f ) );

        if ( !ImGui::BeginPopupModal( "Open Scene?", nullptr, ImGuiWindowFlags_AlwaysAutoResize ) )
            return;

        const bool havePending = m_PendingOpenScene.has_value();

        ImGui::TextUnformatted( "The current scene has unsaved changes." );
        ImGui::TextDisabled( "Open %s", havePending ? SceneLabel( *m_PendingOpenScene ).c_str() : "" );

        // A failed "Save and Open" from a previous click of this same modal. It is shown INSIDE the
        // modal rather than only as a toast because the buttons below are still live: the user is about
        // to decide whether to discard this scene, and that decision changes completely once the save
        // they asked for turns out not to have happened.
        if ( !m_SaveAndOpenError.empty() )
        {
            ImGui::Separator();
            ImGui::TextColored( ThemeManager::GetErrorColor(), "%s", m_SaveAndOpenError.c_str() );
            ImGui::TextDisabled( "\"Discard\" below would throw these changes away for good." );
        }

        ImGui::Separator();

        if ( ImGui::Button( "Save and Open", ImVec2( 130, 0 ) ) )
        {
            // THE GATE THIS WHOLE TASK EXISTS FOR. LoadScene below clears the command history and calls
            // m_MainScene->Clear() — it destroys the only copy of the work the user just asked to have
            // saved. Before the save chain returned a result this ran unconditionally, so a scene that
            // failed to reach the disk was then deleted from memory, with a green "Saved" toast over it
            // and nowhere to recover from. The modal now stays open on a failed write and says so.
            if ( SaveOpenScene() )
            {
                m_SaveAndOpenError.clear();
                if ( havePending )
                    LoadScene( *m_PendingOpenScene );
                m_PendingOpenScene.reset();
                ImGui::CloseCurrentPopup();
            }
            else
            {
                m_SaveAndOpenError = "The scene was NOT saved — see the log for the failing step. "
                                     "Nothing has been opened and nothing has been thrown away.";
            }
        }

        ImGui::SameLine();

        if ( ImGui::Button( "Discard", ImVec2( 110, 0 ) ) )
        {
            m_SaveAndOpenError.clear();
            if ( m_PendingOpenScene.has_value() )
            {
                LoadScene( *m_PendingOpenScene );
            }
            m_PendingOpenScene.reset();
            ImGui::CloseCurrentPopup();
        }

        ImGui::SameLine();

        if ( ImGui::Button( "Cancel", ImVec2( 110, 0 ) ) )
        {
            m_SaveAndOpenError.clear();
            m_PendingOpenScene.reset();
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }

    void EditorLayer::DrawSaveScenePopup()
    {
        if ( !m_SaveSceneRequested )
        {
            return;
        }

        m_SaveSceneRequested = false;
        // Discarded for the same reason as Ctrl+S: File -> Save destroys nothing, and SaveOpenScene has
        // already reported the outcome and left the unsaved mark standing if the write failed.
        (void)SaveOpenScene();
    }

    void EditorLayer::DrawNewScenePopup()
    {
    }

    void EditorLayer::DrawReloadScenePopup()
    {
    }

    void EditorLayer::DrawProjectPopup()
    {
        // Intentionally empty: the editor never opens/switches projects in-session. All content paths
        // are remapped to the project at startup (--project), so switching would require re-initializing
        // the asset manager, cooked caches and panels — relaunch through the Project Hub instead.
    }

    void EditorLayer::OnEvent( Common::Event& event )
    {
        for ( auto& panel : m_Panels )
        {
            if ( event.m_Handled )
                break;
            panel->OnEvent( event );
        }
        for ( auto& document : m_OpenDocuments )
        {
            if ( event.m_Handled )
                break;
            document->OnEvent( event );
        }
    }

    Common::BoolResultStr EditorLayer::OnDetach()
    {
        // The socket goes first, and its file with it. A leftover path is not harmless: the next editor
        // to be given it PROBES what is there, and while a dead one only costs a log line, leaving the
        // file behind on every exit would train everybody to ignore that line.
        //
        // A request still in flight is abandoned rather than answered — the frame that would have proved
        // it is never going to be drawn, and a reply promising otherwise is exactly the lie this channel
        // is built to prevent. The client sees the connection close, which is the truth.
        if ( m_ControlInFlight )
        {
            LOG_WARN( "[Control] the editor is closing with a '{}' still in flight; it is abandoned rather "
                      "than answered, because the frame that would have proved it will not be drawn.",
                      m_ControlInFlight->Group.empty() ? "request" : m_ControlInFlight->Label );
        }
        m_ControlGate.Disarm();
        m_ControlSocket.Close();

        // THE DEVICE DIED, AND THIS IS THE LAST MOMENT THE USER'S WORK EXISTS ANYWHERE.
        //
        // Not left to the autosave timer, which has three separate reasons not to have run recently: it
        // fires every AutosaveMinutes (default 5), it skips when the command revision has not moved, and
        // it runs in Edit mode only. This one runs ONCE, unconditionally, at the moment of loss.
        //
        // It writes a SEPARATE file so that a good periodic autosave is never clobbered by it. The name
        // still contains "_autosave", which is what CrashRecovery::LatestAutosave matches on, and it is
        // the newest file there, so the recovery prompt offers this one.
        //
        // IN PLAY MODE THE AUTHORED SCENE IS WHAT GETS WRITTEN — m_PlaySnapshot, the same text Stop would
        // have restored. The live scene at that instant holds runtime mutations nobody authored and nobody
        // wants back; saving those under the user's scene name would be the wrong answer wearing the right
        // filename.
        if ( Graphic::DeviceLost::IsLost() && m_MainScene )
        {
            using SceneState = ::Desert::Core::Scene::SceneState;
            Desert::Core::SceneSerializer serializer( m_MainScene.get(), m_AssetManager.get() );
            const std::string             text =
                 m_MainScene->GetState() == SceneState::Edit ? serializer.SerializeToJson() : m_PlaySnapshot;
            if ( text.empty() )
            {
                // An empty file under a recovery name is a silent wrong answer: the prompt would offer it
                // and the user would open nothing. Say so instead.
                LOG_ERROR( "[DeviceLost] nothing could be serialized to save — the scene is in {} and its "
                           "authored snapshot is empty. Your periodic autosave, if any, is untouched.",
                           m_MainScene->GetState() == SceneState::Edit ? "Edit" : "Play" );
            }
            else
            {
                std::string name = m_MainScene->GetSceneName();
                for ( auto& ch : name )
                    if ( ch == ' ' )
                        ch = '_';
                const auto      dir = Common::Constants::Path::SCENE_PATH / "Autosave";
                std::error_code ec;
                std::filesystem::create_directories( dir, ec );
                const auto path = dir / ( name + "_devicelost_autosave" +
                                          std::string( Common::Constants::Extensions::SCENE_EXTENSION ) );
                const auto written =
                     ec ? Common::MakeFormattedError( "could not create {}: {}", dir.string(), ec.message() )
                        : Common::Utils::FileSystem::WriteContentToFileAtomic( path, text );
                // BRACES ARE REQUIRED ON BOTH ARMS: the LOG_ macros are not single statements, so a
                // braceless if/else here does not compile. The autosave block above is written the same
                // way for the same reason.
                if ( written )
                {
                    LOG_INFO( "[DeviceLost] your work was saved to {} before shutting down; the next start "
                              "will offer it.",
                              path.string() );
                }
                else
                {
                    LOG_ERROR( "[DeviceLost] the emergency save FAILED: {}. The periodic autosave in {} is "
                               "the newest copy that exists.",
                               written.GetError(), dir.string() );
                }
            }
        }

        // Clean shutdown: drop the session lock so the next start doesn't think we crashed. After a device
        // loss CrashRecovery::DisarmSession refuses, on purpose — see its own comment.
        CrashRecovery::DisarmSession();

        // The launcher's tile picture, refreshed on the way out — HERE, while the device and the
        // scene's final image still exist. Everything below this point is teardown; a few lines
        // further down there is a WaitDeviceIdle and the release of exactly the GPU objects this
        // readback needs.
        //
        // Not on an UNATTENDED run: those open scratch projects in worktrees, and the picture would
        // be of a scene nobody chose, written into a project nobody will open. Same rule, and the
        // same reason, as staying out of the recent-projects registry — and the same correction:
        // this asked `shot.Active()` and therefore missed every control-channel session, which is
        // the unattended path that no longer needs a capture flag at all. See
        // Editor/Core/CommandLine.hpp::IsUnattendedSession.
        //
        // A failure is logged and nothing else. Refusing to shut down because a picture could not
        // be written would be the tail wagging the dog.
        if ( !Editor::IsUnattendedSession( Editor::ShotOptions::Get(),
                                           Control::ControlChannelOptions::Get().Requested() ) )
            if ( const auto thumbnail = WriteProjectThumbnail(); !thumbnail.IsSuccess() )
                LOG_WARN( "[Project] the tile thumbnail was not written on exit: {}", thumbnail.GetError() );

        // The app loop exits right after the last PresentFinalImage, so the GPU is still chewing on that
        // frame's command buffer. Panels own GPU objects — offscreen SceneRenderers (Details preview, asset
        // thumbnails, node-graph preview), framebuffers, descriptor pools — and destroying those while that
        // buffer is in flight is what produced the "can't be called on VkPipeline/VkDescriptorPool ... that
        // is currently in use by VkCommandBuffer" validation errors on quit. Idle first, then tear down.
        Graphic::Renderer::GetInstance().WaitDeviceIdle();

        // The thumbnail service is NOT one of the panels, and the sentence above is why that matters: it
        // names "asset thumbnails" among the GPU objects this teardown covers, but m_Panels.clear() cannot
        // reach a function-static that the panels merely talk to. Left to itself the service is destroyed at
        // __cxa_finalize, long after the device is gone, and ~AssetThumbnailRenderer's WaitDeviceIdle() then
        // dereferences a null renderer API — exit 139, measured. Released here, deterministically, while
        // there is still a device to wait on. See ThumbnailService::Shutdown().
        //
        // Before the panels rather than after: a panel's own teardown must never be able to queue one last
        // preview into a service that has already let its renderer go.
        ThumbnailService::Get().Shutdown();

        // The SECOND half of the same problem, and the half the sentence above still does not cover: the
        // component widgets keep their thumbnail caches in function-statics (StaticMeshComponent.cpp,
        // SkinnedMeshComponentWidget.cpp), so those GPU images belong to no panel and no service. Cleared
        // here for the same reason and at the same moment. See ThumbnailCache::ReleaseAll().
        ThumbnailCache::ReleaseAll();

        // Documents BEFORE tools, and both before the ImGui layer: a document owns a PreviewViewport whose
        // UIHelper holds descriptor sets, and the device has already been idled above. Explicit rather than
        // left to ~EditorLayer, which runs after the layer stack has moved on.
        (void)m_OpenDocuments.ReleaseAll();
        m_Panels.Clear();
        // EVERY ALIAS OF A PANEL DIES WITH THE PANEL, and this line is the half of that CloseSceneView
        // already had and OnDetach did not. `m_Panels.Clear()` destroys every panel while
        // m_FileExplorerPanel and every SceneDocument::Viewport still name one; nothing between here and
        // the end of OnDetach reads them today, so this was latent rather than live — and "nothing reads
        // it today" is the weakest guarantee in this audit, because it is about the code that exists
        // rather than about the code. A8-2.
        m_FileExplorerPanel = nullptr;
        // Reported and not returned even though OnDetach has a channel: everything below this line still
        // has to run, and an early return would leave the extra documents and their render slots alive.
        if ( const auto detached = m_ImGuiLayer->OnDetach(); !detached.IsSuccess() )
            LOG_ERROR( "[EditorLayer] ImGui layer failed to detach: {}", detached.GetError() );
        m_ImGuiLayer.reset();

        // Extra documents in the same order CloseSceneView uses (their panels went with m_Panels above):
        // registry, then scene, then renderer. Explicit rather than left to ~EditorLayer, which runs after
        // the layer stack has moved on and would destroy renderers at an unspecified point relative to it.
        for ( auto& doc : m_ExtraScenes )
        {
            doc->Viewport = nullptr; // the panel went with m_Panels above; see the note there
            doc->Registry.reset();
            doc->Scene.reset();
            doc->Renderer.reset();
        }
        m_ExtraScenes.clear();

        return BOOLSUCCESS;
    }

} // namespace Desert::Editor
