// THE TOOLS THE LEVEL EDITOR OFFERS, and the order the View menu lists them in (UE: the level editor's
// RegisterTabSpawner calls). Declared in DockLayout.hpp; the panels are owned by EditorLayer's PanelRegistry.
#include "Editor/LevelEditor/DockLayout.hpp"

#include "Editor/Core/PanelRegistry.hpp"
#include "Editor/Core/SceneViewIdentity.hpp"
#include "Editor/LevelEditor/DocumentHost.hpp"
#include "Editor/LevelEditor/PlaySession.hpp"
#include "Editor/LevelEditor/SceneWorkspace.hpp"
#include "Editor/Panels/Animation/ControlRigPanel.hpp"
#include "Editor/Panels/AssetReferences/AssetReferencesPanel.hpp"
#include "Editor/Panels/Build/BuildSettingsPanel.hpp"
#include "Editor/Panels/Build/ContentChunksPanel.hpp"
#include "Editor/Panels/Clouds/CloudsPanel.hpp"
#include "Editor/Panels/Collections/CollectionsPanel.hpp"
#include "Editor/Panels/Debug/ShaderLibraryPanel.hpp"
#include "Editor/Panels/Debug/UIDebuggerPanel.hpp"
#include "Editor/Panels/FileExplorer/FileExplorerPanel.hpp"
#include "Editor/Panels/History/HistoryPanel.hpp"
#include "Editor/Panels/Landscape/LandscapePanel.hpp"
#include "Editor/Panels/Localization/LocalizationPanel.hpp"
#include "Editor/Panels/Logs/LogsPanel.hpp"
#include "Editor/Panels/LuaConsole/LuaConsolePanel.hpp"
#include "Editor/Panels/Modeling/ModelingPanel.hpp"
#include "Editor/Panels/Photogrammetry/PhotogrammetryPanel.hpp"
#include "Editor/Panels/Scalability/ScalabilityPanel.hpp"
#include "Editor/Panels/SceneHierarchy/SceneHierarchyPanel.hpp"
#include "Editor/Panels/SceneProperties/ScenePropertiesPanel.hpp"
#include "Editor/Panels/Validation/SceneValidationPanel.hpp"
#include "Editor/Panels/ViewportPanel/ViewportPanel.hpp"
#include "Editor/Panels/WorldPartition/WorldPartitionPanel.hpp"
#include "Editor/Panels/WorldSettings/WorldSettingsPanel.hpp"
#include <Common/Core/Constants.hpp>
#include <Engine/Animation/AnimationLibrary.hpp>

#include <memory>
#include <utility>

namespace Desert::Editor
{
    EditorPanelHandles RegisterEditorPanels( PanelRegistry& panels, SceneWorkspace& workspace, PlaySession& play,
                                             DocumentHost& documents, std::shared_ptr<Assets::AssetManager>& assetManager,
                                             const std::unique_ptr<Animation::AnimationLibrary>& animationLibrary )
    {
        EditorPanelHandles handles;
        panels.Add<Editor::SceneHierarchyPanel>( workspace.ActiveScene(), assetManager );
        panels.Add<Editor::ScenePropertiesPanel>( workspace.ActiveScene(), assetManager,
                                                    animationLibrary.get() );
        panels.Add<Editor::ShaderLibraryPanel>();
        {
            auto primaryViewport =
                 std::make_unique<Editor::ViewportPanel>( workspace.ActiveScene(), assetManager.get() );
            // Focusing the main viewport rebinds the editor back to the primary scene.
            primaryViewport->SetOnActivate( [&workspace] { workspace.SetActiveScene( kPrimarySceneViewId ); } );
            panels.Adopt( std::move( primaryViewport ) );
        }
        {
            auto fileExplorer = std::make_unique<Editor::FileExplorerPanel>(
                 Common::Constants::Path::ASSETS_PATH, &documents.SubjectEditors(), assetManager.get(),
                 workspace.ActiveScene() );
            handles.FileExplorer = fileExplorer.get();
            panels.Adopt( std::move( fileExplorer ) );
        }
        panels.Add<Editor::ModelingPanel>( workspace.ActiveScene() );
        panels.Add<Editor::LandscapePanel>( workspace.ActiveScene() );
        panels.Add<Editor::WorldSettingsPanel>( workspace.ActiveScene() );
        panels.Add<Editor::ScalabilityPanel>();
        // Hidden until asked for: the map is only meaningful on a partitioned scene. The streamer is read through
        // the getter each frame, because Stop and a streaming error destroy it from this side.
        handles.WorldPartition = &panels.Add<Editor::WorldPartitionPanel>(
             workspace.ActiveScene(), assetManager.get(), [&play] { return play.Streamer(); } );
        panels.Add<Editor::LogsPanel>();
        panels.Add<Editor::CollectionsPanel>( assetManager.get() );
        panels.Add<Editor::HistoryPanel>();
        panels.Add<Editor::SceneValidationPanel>( workspace.ActiveScene(), assetManager.get() );
        panels.Add<Editor::LocalizationPanel>();
        panels.Add<Editor::UIDebuggerPanel>( workspace.ActiveScene() );
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
        panels.Add<Editor::PhotogrammetryPanel>( workspace.ActiveScene(), assetManager.get() );
        panels.Add<Editor::AssetReferencesPanel>( workspace.ActiveScene(), assetManager );
        panels.Add<Editor::LuaConsolePanel>( workspace.ActiveScene().get(), assetManager.get() );
        panels.Add<Editor::ControlRigPanel>( workspace.ActiveScene() );
        panels.Add<Editor::BuildSettingsPanel>();
        panels.Add<Editor::ContentChunksPanel>();
        // THE CLOUDS WINDOW IS A TOOL, and it must be: it is a setting the user keeps (View ▸ Clouds), it
        // edits no subject of its own, and the compiler refuses a document here anyway (PanelRegistry).
        // What it DOES is show the documents that edit the six stages of the sky — asked of
        // documents.Documents(), which is the same container the document well reads, so both windows show the
        // same object and neither knows the other exists. See Editor/Panels/Clouds/CloudsPanel.hpp.
        panels.Add<Editor::CloudsPanel>( workspace.ActiveScene(), assetManager, documents.Documents() );

        return handles;
    }
} // namespace Desert::Editor
