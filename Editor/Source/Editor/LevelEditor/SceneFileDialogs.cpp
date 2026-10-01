// SCENE FILE DIALOGS — the Scenes menu, File -> Open Scene, the Open Scene modal and the unsaved-changes
// confirm. Members of SceneFiles (SceneFiles.hpp); moved out of EditorLayer.cpp unchanged (EDL-4).
#include "Editor/LevelEditor/SceneFiles.hpp"
#include "Editor/Core/IconsMaterialDesignIcons.hpp"
#include <Engine/World/Landscape/LandscapeData.hpp>
#include <Editor/Core/Control/InputInjection.hpp>
#include <Editor/Core/Control/PointerDrag.hpp>
#include <Engine/Core/Glfw.hpp>
#include <Engine/Core/PlayerStart.hpp>
#include <Editor/Core/SaveShortcut.hpp>
#include <Editor/Core/ContentCreateCommands.hpp>
#include <Editor/Core/DetailsNavigation.hpp>
#include <Engine/Graphic/ViewBudgetGate.hpp>
#include <Engine/Graphic/Environment/EnvironmentBake.hpp>
#include <Engine/Assets/ContentWork.hpp>
#include <Engine/Assets/BootContent.hpp>
#include <Engine/Assets/ContentRegistry.hpp>
#include <Engine/Assets/Serialization/MeshBinary.hpp>
#include <Common/Core/AssetHandle.hpp>
#include <Common/Content/AssetRedirector.hpp>
#include <Common/Content/ContentChunks.hpp>
#include <Engine/Assets/TextureSourceAsset.hpp>
#include <Engine/Assets/MeshDerivedData.hpp>
#include <Editor/Import/MeshDeriver.hpp>
#include <Common/Core/AssetPathIndex.hpp>
#include <Common/Utilities/ContentScanLedger.hpp>
#include <Engine/Core/ShaderCompiler/ShaderSpirvCache.hpp>
#include <Engine/Graphic/MemoryReadout.hpp>
#include <Engine/Graphic/ResourceLedger.hpp>
#include <Engine/Graphic/DrawCounters.hpp>
#include <Engine/Assets/SyncLoadLedger.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Assets/TextureSourceAsset.hpp>
#include <functional>
#include <set>
#include <Editor/Widgets/ThumbnailService.hpp>
#include <Editor/Widgets/ThumbnailWarmup.hpp>
#include <Editor/Widgets/ToolbarLayout.hpp>
#include <Engine/Core/SceneAssetRoots.hpp>
#include <Common/Core/Core.hpp>
#include <Common/Core/CrashHandler.hpp>
#include <Common/Core/Profiler.hpp>
#include <Editor/Import/CookPaths.hpp>
#include <Editor/Import/MeshDnD.hpp>
#include <Editor/Import/ImportOptionsDialog.hpp>
#include <Engine/Runtime/Services/Mesh/MeshService.hpp>
#include <Engine/Runtime/Services/AssetServiceRegistration.hpp>
#include <Common/Core/JobSystem.hpp>
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
#include <Engine/Animation/Rig/ControlHierarchy.hpp>
#include <Engine/Animation/Rig/ControlRigStage.hpp>
#include <Engine/Assets/AssetEviction.hpp>
#include <Engine/Assets/Mesh/AnimationAsset.hpp>
#include <Engine/Scripting/ScriptEngine.hpp>
#include <Engine/Core/Serialize/SceneSerializer.hpp>
#include <Engine/Core/WorldStreamer.hpp>
#include <Engine/Core/Serialize/SceneFormat.hpp>
#include <Engine/Core/Serialize/ExternalEntities.hpp>
#include "Editor/Core/CommandLine.hpp"
#include "Editor/Core/Control/ControlChannelOptions.hpp"
#include "Editor/Core/Control/ControlDispatch.hpp" // resolving a request to a palette entry
#include "Editor/Core/AutosavePaths.hpp"
#include "Editor/Core/CrashRecovery.hpp"
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
#include "Editor/Core/EditorPreferences.hpp"
#include "Editor/Packaging/GamePackager.hpp"
#include "Editor/Core/ProjectContext.hpp"
#include <Engine/Graphic/API/Vulkan/VulkanContext.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanSwapChain.hpp> // reading the PRESENTED frame back (shot.window)
#include <Engine/Graphic/Image.hpp>                      // Image2D::ReadPixelsRGBA8 (debug frame dump)
#include <Engine/Core/Input.hpp>
#include <Common/Core/KeyCodes.hpp>
#include <Common/Core/Version.hpp>
#include <Common/Settings/MachineSettings.hpp>
#include <stb_image/stb_image_write.h>
#include "Editor/Core/ImGuiUtilities.hpp"
#include <ImGui/imgui_internal.h>
#include <array>
#include <format>
#include <ImGuizmo.h>
#include "Editor/Import/ImportManager.hpp"
#include "Editor/Splash/SplashControls.hpp"
#include "Editor/Splash/SplashImage.hpp"
#include "Editor/Widgets/WindowButtonStyle.hpp"
#include "Editor/Builtin/BuiltinMeshRegistry.hpp"
#include "Editor/Panels/SceneHierarchy/SceneHierarchyPanel.hpp"
#include <Editor/Core/SkeletonAssignPalette.hpp>
#include <Editor/Panels/AnimationEditor/SkeletonReferenceSlots.hpp>
#include <Engine/Assets/Mesh/SkinnedMeshAsset.hpp>
#include <Engine/Assets/Serialization/AnimationClipWrite.hpp>
#include "Editor/Panels/SceneProperties/ScenePropertiesPanel.hpp"
#include "Editor/Panels/Debug/ShaderLibraryPanel.hpp"
#include "Editor/Panels/Debug/UIDebuggerPanel.hpp"
#include "Editor/Panels/FileExplorer/FileExplorerPanel.hpp"
#include "Editor/Widgets/ThumbnailEdit.hpp"
#include "Editor/Panels/ViewportPanel/ViewportPanel.hpp"
#include "Editor/Panels/ViewportPanel/Tools/ActiveToolBar.hpp"
#include "Editor/Panels/Scalability/ScalabilityPanel.hpp"
#include "Editor/Panels/WorldSettings/WorldSettingsPanel.hpp"
#include "Editor/Panels/WorldPartition/WorldPartitionPanel.hpp"
#include <Engine/Core/Serialize/WorldPartitionConversion.hpp>
#include "Editor/Panels/Landscape/LandscapePanel.hpp"
#include "Editor/Panels/Landscape/LandscapeCommands.hpp"
#include "Editor/LevelEditor/ViewportCommands.hpp"
#include "Editor/Panels/Clouds/CloudCommands.hpp"
#include "Editor/Panels/Localization/LanguageCommands.hpp"
#include "Editor/Panels/Build/BuildCommands.hpp"
#include "Editor/Panels/UI/UICommands.hpp"
#include "Editor/Core/DebugCommands.hpp"
#include "Editor/Panels/Modeling/ModelingCommands.hpp"
#include "Editor/Panels/Foliage/FoliageCommands.hpp"
#include "Editor/Core/Rigging/HumanoidCommands.hpp"
#include "Editor/Panels/Modeling/ModelingPanel.hpp"
#include "Editor/Panels/Logs/LogsPanel.hpp"
#include "Editor/Panels/Collections/CollectionsPanel.hpp"
#include "Editor/Panels/NodeGraph/NodeGraphPanel.hpp"
#include "Editor/Panels/NodeGraph/ShaderGraphDocumentOpen.hpp"
#include "Editor/Panels/MaterialEditor/MaterialEditorPanel.hpp"
#include "Editor/Panels/MaterialEditor/MaterialDocumentOpen.hpp"
#include "Editor/Panels/SceneProperties/ComponentWidgets/MaterialsPanelComponent.hpp"
#include "Editor/Core/AssetOpen.hpp"
#include "Editor/Panels/Animation/AnimGraphPanel.hpp"
#include "Editor/Panels/Photogrammetry/PhotogrammetryPanel.hpp"
#include "Editor/Panels/Particles/ParticleEditorPanel.hpp"
#include "Editor/Panels/UI/UIEditorPanel.hpp"
#include "Editor/Panels/AssetReferences/AssetReferencesPanel.hpp"
#include "Editor/Panels/LuaConsole/LuaConsolePanel.hpp"
#include "Editor/Panels/Sequencer/SequencerPanel.hpp"
#include <Engine/Animation/Timeline/Hosts.hpp>
#include "Editor/Panels/Build/BuildSettingsPanel.hpp"
#include "Editor/Panels/Build/ContentChunksPanel.hpp"
#include "Editor/Packaging/ProjectChunkScheme.hpp"
#include "Editor/Panels/History/HistoryPanel.hpp"
#include "Editor/Panels/Localization/LocalizationPanel.hpp"
#include "Editor/Panels/Validation/SceneValidationPanel.hpp"
#include "Editor/Panels/Clouds/CloudModellingVolumePanel.hpp"
#include "Editor/Panels/Clouds/CloudDocumentOpen.hpp"
#include "Editor/Panels/Clouds/CloudLayoutPanel.hpp"
#include "Editor/Panels/Clouds/CloudNoiseVolumePanel.hpp"
#include "Editor/Panels/SkyboxViewer/SkyboxViewerDocument.hpp"
#include "Editor/Panels/AnimationEditor/AnimationEditorDocument.hpp"
#include <Common/Content/ContentKinds.hpp>
#include "Editor/Panels/StaticMeshViewer/StaticMeshViewerDocument.hpp"
#include "Editor/Panels/TextureViewer/TextureViewerDocument.hpp"
#include "Editor/Panels/Clouds/CloudTypePanel.hpp"
#include "Editor/Panels/Clouds/CloudsPanel.hpp"
#include "Editor/Panels/Animation/ControlRigPanel.hpp"
#include "Editor/Core/Selection/AuthoringContext.hpp"
#include "Editor/Core/ToastManager.hpp"
#include "Editor/Core/OpenableAssets.hpp"
#include "Editor/Core/ViewportCameraProperties.hpp"
#include "Editor/Core/SubjectEditorRegistry.hpp"
#include "Editor/Core/ControlNudgeRequest.hpp"
#include "Editor/Core/Commands/PoseEditTransaction.hpp"
#include <Engine/Animation/Rig/ControlManipulator.hpp>
#include "Editor/Core/SubjectOpenRequest.hpp"
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
#include <Editor/Core/DocumentPlacement.hpp>
#include <Editor/Core/Rigging/RigBuilder.hpp>
#include <Editor/Core/Selection/ModelingState.hpp>
#include <Editor/Core/Selection/ModelingStateProperties.hpp>
#include <Editor/Core/Selection/SelectionTransformProperties.hpp>
#include <Editor/Core/Selection/SelectionManager.hpp>
#include <Engine/ECS/System/PointLightSystem.hpp>
#include <Engine/ECS/System/SpotLightSystem.hpp>
#include <Engine/ECS/System/AnimationECSSystem.hpp>
#include <Engine/ECS/System/AttachmentSystem.hpp>
#include <Engine/ECS/System/PhysicsECSSystem.hpp>
#include <Engine/ECS/System/LevelSequenceSystem.hpp>
#include <Engine/ECS/System/LocomotionSystem.hpp>
#include <Engine/ECS/System/ScriptSystem.hpp>
#include <Engine/ECS/System/AudioECSSystem.hpp>
#include <algorithm> // std::sort / std::transform (scene list)
#include <span>      // the View menu's groups, declared as data rather than as control flow
#include <cctype>    // std::tolower (scene filter)
#include <chrono>    // per-stage startup timing (see the staged boot in OnUpdate)

namespace Desert::Editor
{
    // Case-insensitive matching for the scene filter (ASCII: scene paths on disk are ASCII).
    static std::string Lowercased( const std::string& text )
    {
        std::string out = text;
        std::transform( out.begin(), out.end(), out.begin(),
                        []( unsigned char c ) { return static_cast<char>( std::tolower( c ) ); } );
        return out;
    }

    void SceneFiles::DrawDialogs()
    {
        DrawOpenScenePopup();
        DrawConfirmOpenScenePopup();
        DrawSaveScenePopup();
    }

    void SceneFiles::DrawOpenSceneMenuItem()
    {
        namespace ImGui = ::ImGui;

        if ( ImGui::MenuItem( "Open Scene" ) )
        {
            PrepareScenePopup();
            m_OpenScenePopup = true;
        }
    }

    void SceneFiles::DrawScenesMenu()
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
                const std::string label = Label( path );
                if ( ImGui::MenuItem( label.c_str() ) )
                {
                    // Same gated path as the palette and the Open Scene dialog (see SceneOpenRequest).
                    Editor::Core::SceneOpenRequest::Request( path.string() );
                }
                Utils::ImGuiUtilities::Tooltip( path.string().c_str() );
            }
        }

        ImGui::EndMenu();
    }

    void SceneFiles::DrawOpenScenePopup()
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
                const std::string label = Label( m_AvailableScenes[i] );
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
                    // The palette's path, not LoadScene: this button used to skip the unsaved-changes gate
                    // that the palette, the drop and the asset browser all run, and discarded edits silently.
                    Editor::Core::SceneOpenRequest::Request( m_AvailableScenes[m_SelectedSceneIndex].string() );
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
    void SceneFiles::DrawConfirmOpenScenePopup()
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
        ImGui::TextDisabled( "Open %s", havePending ? Label( *m_PendingOpenScene ).c_str() : "" );

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
            // m_Workspace.ActiveScene()->Clear() — it destroys the only copy of the work the user just asked to
            // have saved. Before the save chain returned a result this ran unconditionally, so a scene that failed
            // to reach the disk was then deleted from memory, with a green "Saved" toast over it and nowhere to
            // recover from. The modal now stays open on a failed write and says so.
            if ( SaveOpenScene() )
            {
                m_SaveAndOpenError.clear();
                if ( havePending )
                    RequestLoad( *m_PendingOpenScene );
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
                RequestLoad( *m_PendingOpenScene );
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

    void SceneFiles::DrawSaveScenePopup()
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
} // namespace Desert::Editor
