#include <Editor/Core/DetailsNavigation.hpp>
#include "ScenePropertiesPanel.hpp"
#include "ComponentEditor.hpp"

#include <Engine/ECS/Entity.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/System/SkyboxECSSystem.hpp>
#include <Editor/Core/Commands/SceneCommands.hpp>
#include <Editor/Core/IconsMaterialDesignIcons.hpp>
#include <Engine/ECS/EntityLock.hpp>
#include <Editor/Core/Selection/SelectionManager.hpp>
#include <Editor/Core/EditorResources.hpp>
#include <Editor/Core/ImGuiUtilities.hpp>
#include <Editor/Core/ThemeManager.hpp>
#include <Editor/Widgets/Controls/Controls.hpp>
#include <ImGui/imgui.h>
#include <Editor/Widgets/ThumbnailCache.hpp>
#include <Engine/Assets/Prefab/PrefabAsset.hpp>
#include <Engine/Assets/Mesh/MeshAsset.hpp>
#include <Engine/Assets/Mesh/SurfaceMaterialAsset.hpp>

#include <filesystem>
#include <system_error>
#include <Engine/Core/Scene.hpp>
#include <Engine/Core/EngineContext.hpp>
#include <Common/Core/Constants.hpp>
#include <Common/Core/Logger.hpp>

namespace Desert::Editor
{
    namespace ImGui = ::ImGui;

    namespace
    {
        const char* GetPrimaryComponentName( const ECS::Entity& entity )
        {
            if ( entity.HasComponent<ECS::CameraComponent>() )
                return "CameraComponent";
            if ( entity.HasComponent<ECS::DirectionLightComponent>() )
                return "DirectionalLightComponent";
            if ( entity.HasComponent<ECS::PointLightComponent>() )
                return "PointLightComponent";
            if ( entity.HasComponent<ECS::SpotLightComponent>() )
                return "SpotLightComponent";
            // Before Skybox: an entity carrying both is a procedural sky that also keeps an HDR fallback,
            // and the atmosphere is what it is named for.
            if ( entity.HasComponent<ECS::SkyAtmosphereComponent>() )
                return "SkyAtmosphereComponent";
            if ( entity.HasComponent<ECS::SkyboxComponent>() )
                return "SkyboxComponent";
            if ( entity.HasComponent<ECS::LandscapeComponent>() )
                return "LandscapeComponent";
            if ( entity.HasComponent<ECS::SkinnedMeshComponent>() )
                return "SkinnedMeshComponent";
            if ( entity.HasComponent<ECS::StaticMeshComponent>() )
                return "StaticMeshComponent";
            return "Actor";
        }

        const char* GetEntityIcon( const ECS::Entity& entity )
        {
            if ( entity.HasComponent<ECS::CameraComponent>() )
                return ICON_MDI_CAMERA;
            if ( entity.HasComponent<ECS::SpotLightComponent>() )
                return ICON_MDI_SPOTLIGHT;
            if ( entity.HasComponent<ECS::DirectionLightComponent>() ||
                 entity.HasComponent<ECS::PointLightComponent>() )
                return ICON_MDI_LIGHTBULB;
            if ( entity.HasComponent<ECS::SkyAtmosphereComponent>() )
                return ICON_MDI_WEATHER_SUNSET;
            if ( entity.HasComponent<ECS::SkyboxComponent>() )
                return ICON_MDI_EARTH;
            if ( entity.HasComponent<ECS::LandscapeComponent>() )
                return ICON_MDI_TERRAIN;
            if ( entity.HasComponent<ECS::TextComponent>() )
                return ICON_MDI_FORMAT_TEXT;
            return ICON_MDI_CUBE_OUTLINE;
        }
    } // namespace

    void ScenePropertiesPanel::DrawSearchBox()
    {
        // UE's Details search: type a property name and the panel narrows to it (fields, categories and
        // whole components). Lives ABOVE the scrolling list so it never scrolls out of reach.
        ImGui::PushStyleColor( ImGuiCol_Text, ThemeManager::GetIconColor() );
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted( ICON_MDI_MAGNIFY );
        ImGui::PopStyleColor();
        ImGui::SameLine();

        const bool hasText = !m_FieldSearch.empty();
        ImGui::PushItemWidth( ImGui::GetContentRegionAvail().x -
                              ( hasText ? ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x : 0.0f ) );
        Utils::ImGuiUtilities::InputText( m_FieldSearch, "##DetailsSearch" );
        ImGui::PopItemWidth();

        if ( hasText )
        {
            ImGui::SameLine();
            if ( ImGui::Button( ICON_MDI_CLOSE "##ClearDetailsSearch" ) )
                m_FieldSearch.clear();
            Utils::ImGuiUtilities::Tooltip( "Clear the search" );
        }

        ImGui::Spacing();
    }

    void ScenePropertiesPanel::DrawNoSelectionState()
    {
        GetDetailsNavigation().BeginFrame( 0, std::string() );
        GetDetailsNavigation().EndFrame();
        // Details with nothing selected used to return immediately, leaving the dock as a bare grey
        // rectangle — the single worst square of the default editor layout, because it is also the
        // largest, and it says neither what the panel is nor how to make it show something.
        //
        // The action creates an entity and SELECTS it, which means the button leaves the state it is
        // offered from: an empty state whose button leaves you in the empty state is a decoration.
        const bool add = Utils::ImGuiUtilities::EmptyState(
             ICON_MDI_CURSOR_DEFAULT_OUTLINE, "Nothing selected",
             "Pick an entity in the Outliner, or click one in the viewport.", ICON_MDI_PLUS "  Add an entity" );

        if ( add && m_Scene )
        {
            auto       entity = m_Scene->CreateNewEntity( "Entity" );
            const auto uuid   = entity.GetComponent<ECS::UUIDComponent>().UUID;
            // Through the same command notification the Outliner's spawn menu uses, so the new entity is
            // one Ctrl+Z away and the unsaved-changes marker moves.
            Commands::NotifyCreated( { uuid } );
            Core::SelectionManager::SetSelected( uuid );
        }
    }

    void ScenePropertiesPanel::OnUIRender()
    {
        auto selectedOpt = Core::SelectionManager::GetSelected();
        if ( !selectedOpt.has_value() )
        {
            DrawNoSelectionState();
            return;
        }

        const auto& selectedEntityOpt = m_Scene->FindEntityByID( selectedOpt.value() );
        if ( !selectedEntityOpt )
        {
            // Selected, but the entity is gone (deleted while the panel was hidden, or a stale UUID from a
            // reloaded scene). Same shape, different sentence: the reason matters to whoever is reading it.
            Utils::ImGuiUtilities::EmptyState( ICON_MDI_CUBE_OUTLINE, "Entity not found",
                                               "The selected entity is no longer in this scene." );
            return;
        }

        const auto& selectedEntity = selectedEntityOpt.value().get();

        // --- Header row: [ ] [icon] [Name (bold, editable)]      [save] [tune] ---
        // Visible toggle -> VisibilityComponent.Visible (added on demand). Render systems skip invisible.
        bool active = !selectedEntity.HasComponent<ECS::VisibilityComponent>() ||
                      selectedEntity.GetComponent<ECS::VisibilityComponent>().Visible;
        if ( ImGui::Checkbox( "##ActiveCheckbox", &active ) )
        {
            // Cascade visibility to the whole subtree so hiding a parent hides its children (UE-like).
            m_Scene->SetVisibleRecursive( const_cast<ECS::Entity&>( selectedEntity ), active );
        }
        ImGui::SameLine();

        // The authoring padlock, in the header beside the visibility box for the same reason the outliner
        // puts them side by side. An entity reached from the outliner while locked shows its state HERE
        // too — otherwise Details would present a full set of editable transform fields for something the
        // viewport refuses to touch, which is the two-halves-disagreeing shape this task exists to avoid.
        {
            const bool locked = ECS::IsLocked( *selectedEntity.GetRegistry(), selectedEntity.GetHandle() );
            ImGui::PushStyleColor( ImGuiCol_Text, locked ? ImGui::GetStyleColorVec4( ImGuiCol_TextDisabled )
                                                         : ThemeManager::GetIconColor() );
            ImGui::TextUnformatted( locked ? ICON_MDI_LOCK : ICON_MDI_LOCK_OPEN_OUTLINE );
            ImGui::PopStyleColor();
            if ( ImGui::IsItemClicked() )
                ECS::SetLockedRecursive( *selectedEntity.GetRegistry(), selectedEntity.GetHandle(), !locked );
            if ( ImGui::IsItemHovered() )
                ImGui::SetTooltip( locked ? "Locked: the viewport will not pick this and the gizmo will not "
                                            "move it. Click to unlock."
                                          : "Click to lock: refuse viewport picking and gizmo edits." );
            ImGui::SameLine();
        }

        // World Partition's one authored input per entity (ECS::AlwaysLoadedComponent). Beside the lock
        // because it is the same kind of thing - a marker whose presence is the state, toggled in place.
        // Not recursive: the partitioner keeps a whole composite together, so marking one member already
        // keeps its parent and children loaded with it.
        {
            ECS::Entity& entity       = const_cast<ECS::Entity&>( selectedEntity );
            const bool   alwaysLoaded = entity.HasComponent<ECS::AlwaysLoadedComponent>();
            ImGui::PushStyleColor( ImGuiCol_Text, alwaysLoaded
                                                       ? ThemeManager::GetIconColor()
                                                       : ImGui::GetStyleColorVec4( ImGuiCol_TextDisabled ) );
            ImGui::TextUnformatted( alwaysLoaded ? ICON_MDI_PIN : ICON_MDI_PIN_OUTLINE );
            ImGui::PopStyleColor();
            if ( ImGui::IsItemClicked() )
            {
                if ( alwaysLoaded )
                    entity.RemoveComponent<ECS::AlwaysLoadedComponent>();
                else
                    entity.AddComponent<ECS::AlwaysLoadedComponent>();
            }
            if ( ImGui::IsItemHovered() )
                ImGui::SetTooltip(
                     alwaysLoaded ? "Always loaded: in a partitioned world this entity and everything it "
                                    "is attached to stay loaded everywhere. Click to let its position decide."
                                  : "Click to keep this entity loaded everywhere in a partitioned world. "
                                    "Worlds without a WorldPartition block load everything anyway." );
            ImGui::SameLine();
        }

        ImGui::PushStyleColor( ImGuiCol_Text, ThemeManager::GetIconColor() );
        ImGui::TextUnformatted( GetEntityIcon( selectedEntity ) );
        ImGui::PopStyleColor();
        ImGui::SameLine();

        auto& tag = selectedEntity.GetComponent<ECS::TagComponent>().Tag;

        ImGui::PushItemWidth( ImGui::GetContentRegionAvail().x - ImGui::GetFontSize() * 4.5f );
        ImGui::PushFont( EditorResources::GetBoldFont() );
        if ( Utils::ImGuiUtilities::InputText( tag, "##InspectorNameChange" ) )
        {
        }
        ImGui::PopFont();
        ImGui::PopItemWidth();
        ImGui::SameLine();

        ImGui::PushStyleColor( ImGuiCol_Button, ImVec4( 0.7f, 0.7f, 0.7f, 0.0f ) );

        if ( ImGui::Button( ICON_MDI_FLOPPY ) )
            ImGui::OpenPopup( "SavePrefab" );

        Utils::ImGuiUtilities::Tooltip( "Save Entity As Prefab" );

        ImGui::SameLine();
        if ( ImGui::Button( ICON_MDI_TUNE ) )
            ImGui::OpenPopup( "SetDebugMode" );

        ImGui::PopStyleColor();

        // --- Subtitle: component type ---
        {
            ImGui::SetCursorPosX( ImGui::GetFontSize() * 2.8f );
            ImGui::TextDisabled( "%s", GetPrimaryComponentName( selectedEntity ) );
        }

        if ( ImGui::BeginPopup( "SetDebugMode", 3 ) )
        {
            const bool hasPrefab = selectedEntity.HasComponent<ECS::PrefabComponent>();
            if ( !hasPrefab )
                ImGui::BeginDisabled();

            if ( ImGui::Button( "Revert To Prefab" ) )
            {
                if ( hasPrefab )
                {
                    auto& pc         = selectedEntity.GetComponent<ECS::PrefabComponent>();
                    auto  prefabAsset = m_AssetManager->FindByHandle<Assets::PrefabAsset>( pc.Prefab );
                    if ( prefabAsset )
                    {
                        std::optional<glm::vec3> savedPos;
                        if ( selectedEntity.HasComponent<ECS::TransformComponent>() )
                            savedPos = selectedEntity.GetComponent<ECS::TransformComponent>().Translation;

                        ECS::Entity parentEntity;
                        if ( selectedEntity.HasComponent<ECS::RelationshipComponent>() )
                        {
                            const entt::entity ph =
                                 selectedEntity.GetComponent<ECS::RelationshipComponent>().Parent;
                            if ( ph != entt::null )
                            {
                                parentEntity = ECS::Entity{ ph, m_Scene->GetRegistry() };
                            }
                        }

                        m_Scene->DestroyEntity( const_cast<ECS::Entity&>( selectedEntity ) );
                        Core::SelectionManager::ClearSelection();
                        // The parent is kept, because a UI element reverted to the scene root would be
                        // legal, invisible and silent — the case PrefabPlacement refuses by name.
                        if ( const auto placed = prefabAsset->Instantiate(
                                  m_Scene.get(), *m_AssetManager, parentEntity, savedPos ? &*savedPos : nullptr );
                             !placed )
                        {
                            LOG_ERROR( "{}", placed.GetError() );
                        }
                    }
                }
                ImGui::CloseCurrentPopup();
            }

            if ( !hasPrefab )
                ImGui::EndDisabled();

            ImGui::Separator();

            if ( ImGui::Selectable( "Debug Mode", m_DebugMode ) )
            {
                m_DebugMode = !m_DebugMode;
            }
            ImGui::EndPopup();
        }

        if ( ImGui::BeginPopupModal( "SavePrefab", NULL, ImGuiWindowFlags_AlwaysAutoResize ) )
        {
            ImGui::Text( "Save Current Entity as a Prefab?\n\n" );
            ImGui::Separator();

            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted( "Name : " );
            ImGui::SameLine();
            Utils::ImGuiUtilities::InputText( tag, "##PrefabNameChange" );

            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted( "Path : " );
            ImGui::SameLine();
            Utils::ImGuiUtilities::InputText( m_PrefabSavePath, "##PrefabPathChange" );

            if ( ImGui::Button( "OK", ImVec2( 120, 0 ) ) )
            {
                const Common::Filepath fullPath =
                     Common::Filepath( m_PrefabSavePath ) /
                     ( tag + std::string( Common::Constants::Extensions::PREFAB_EXTENSION ) );

                // The user types this path by hand, so its directory may not exist at all. Created here;
                // the entity is only tagged as an instance once the file is really there.
                std::error_code dirEc;
                std::filesystem::create_directories( fullPath.parent_path(), dirEc );

                // Entity is a handle: the copy names the same registry row, so nothing here needs
                // the panel's const reference made mutable.
                const ECS::Entity sourceEntity = selectedEntity;
                const auto  saved =
                     Assets::PrefabAsset::SaveNewFromEntity( sourceEntity, *m_AssetManager, fullPath );
                {
                    if ( !saved )
                    {
                        LOG_ERROR( "[Prefab] '{}' was NOT written: {} — the entity is unchanged and is "
                                   "NOT marked as a prefab instance.",
                                   fullPath.string(), saved.GetError() );
                    }
                    else
                    {
                        // Tag the source entity as a prefab instance so the hierarchy panel shows it
                        auto& pc           = sourceEntity.HasComponent<ECS::PrefabComponent>()
                                                  ? sourceEntity.GetComponent<ECS::PrefabComponent>()
                                                  : sourceEntity.AddComponent<ECS::PrefabComponent>();
                        pc.Prefab          = saved.GetValue()->GetMetadata().Handle;

                        LOG_INFO( "Prefab saved: {0}", fullPath.string() );
                    }
                }

                ImGui::CloseCurrentPopup();
            }

            ImGui::SetItemDefaultFocus();
            ImGui::SameLine();
            if ( ImGui::Button( "Cancel", ImVec2( 120, 0 ) ) )
            {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        ImGui::Separator();

        if ( m_DebugMode )
        {
            // The entity's real UUID — this row used to print the TAG under a "UUID" label, which is the
            // one thing debug mode exists to show.
            Utils::ImGuiUtilities::ResetPropertyRows();
            Utils::ImGuiUtilities::BeginPropertyRow( "UUID", "Stable identity used by prefabs, sockets and "
                                                             "scripts" );
            ImGui::AlignTextToFramePadding();
            if ( selectedEntity.HasComponent<ECS::UUIDComponent>() )
            {
                ImGui::Text( "%llu", static_cast<unsigned long long>(
                                          selectedEntity.GetComponent<ECS::UUIDComponent>().UUID ) );
            }
            else
            {
                ImGui::TextDisabled( "none" );
            }
            Utils::ImGuiUtilities::EndPropertyRow();
        }

        DrawSearchBox();

        ImGui::BeginChild( "Components", ImVec2( 0.0f, 0.0f ), false, ImGuiWindowFlags_None );
        if ( !m_ComponentEditor )
            m_ComponentEditor = std::make_unique<ComponentEditor>( m_AssetManager, m_AnimationLibrary );
        // The texture-id cache the component rows blit their thumbnails through (lazy: an entity with no
        // thumbnail never builds one).
        if ( !m_ThumbnailUI )
        {
            m_ThumbnailUI = std::make_unique<UI::UIHelper>();
            m_ThumbnailUI->Init();
        }
        m_ComponentEditor->SetThumbnailUI( m_ThumbnailUI.get() );
        DetailsNavigation& navigation = GetDetailsNavigation();
        navigation.BeginFrame(
             selectedEntity.HasComponent<ECS::UUIDComponent>()
                  ? static_cast<std::uint64_t>( selectedEntity.GetComponent<ECS::UUIDComponent>().UUID )
                  : 0,
             selectedEntity.HasComponent<ECS::TagComponent>()
                  ? selectedEntity.GetComponent<ECS::TagComponent>().Tag
                  : std::string() );
        m_ComponentEditor->Render( const_cast<ECS::Entity&>( selectedEntity ), m_Scene.get(),
                                   m_FieldSearch.c_str() );
        navigation.EndFrame();
        ImGui::EndChild();
    }

} // namespace Desert::Editor