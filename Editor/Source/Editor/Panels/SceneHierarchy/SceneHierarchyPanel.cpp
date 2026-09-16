#include "SceneHierarchyPanel.hpp"
#include <Editor/Core/DragPayloads.hpp>
#include <Editor/Panels/PropertyEditor/ComponentWidgetRegistry.hpp>
#include <Engine/ECS/Entity.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/EntityLock.hpp>
#include <Engine/Assets/MaterialData.hpp>
#include <Engine/Assets/Mesh/SurfaceMaterialAsset.hpp>
#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Common/Core/Serialization/GlmReflection.hpp>
#include <Engine/Core/Serialize/CustomReflect.hpp>
#include <rflcpp/rfl/json.hpp>
#include <Engine/Assets/Prefab/PrefabAsset.hpp>
#include <Engine/Geometry/ProceduralCharacterFactory.hpp>
#include <Engine/Geometry/DynamicMesh.hpp>

#include <algorithm>
#include <cmath>
#include <Editor/Core/Selection/SelectionManager.hpp>
#include <Editor/Core/Commands/SceneCommands.hpp>
#include <Editor/Core/EditorResources.hpp>
#include <Editor/Core/ThemeManager.hpp>
#include <Editor/Core/ImGuiUtilities.hpp>
#include <Editor/Core/IconsMaterialDesignIcons.hpp>

#include <ImGui/imgui_internal.h>

#include <Editor/Builtin/BuiltinMeshRegistry.hpp>
#include <Common/Core/Constants.hpp>
#include <Common/Utilities/FileSystem.hpp>

#include <filesystem>
#include <functional>
#include <cstring>

namespace Desert::Editor
{
    namespace ImGui = ::ImGui;

    namespace
    {
        // Creatable actor archetypes for the outliner "+ Add" menu, grouped the SAME way as the Details
        // "Add Component" menu. A plain POD table + a free SpawnArchetype() switch (rather than inline
        // lambdas in an initializer list) keeps it data-driven AND formats consistently under Allman —
        // parameter-less multi-line lambdas are the one construct clang-format versions disagree on.
        enum class Archetype
        {
            Cube,
            Model3D,
            Skybox,
            Terrain,
            DirLight,
            PointLight,
            SpotLight,
            SkinnedModel,
            Character,
            Camera,
        };

        struct ArchetypeDef
        {
            const char* Category;
            const char* Icon;
            const char* Label;
            Archetype   Kind;
        };

        constexpr ArchetypeDef kArchetypes[] = {
             { "Rendering", ICON_MDI_CUBE, "Cube", Archetype::Cube },
             { "Rendering", ICON_MDI_CUBE_OUTLINE, "3D Model", Archetype::Model3D },
             { "Rendering", ICON_MDI_EARTH, "Skybox", Archetype::Skybox },
             { "Rendering", ICON_MDI_TERRAIN, "Terrain", Archetype::Terrain },
             { "Lighting", ICON_MDI_LIGHTBULB, "Directional Light", Archetype::DirLight },
             { "Lighting", ICON_MDI_LIGHTBULB, "Point Light", Archetype::PointLight },
             { "Lighting", ICON_MDI_SPOTLIGHT, "Spot Light", Archetype::SpotLight },
             { "Animation", ICON_MDI_RUN, "Skinned Model", Archetype::SkinnedModel },
             { "Animation", ICON_MDI_HUMAN, "Character (Procedural)", Archetype::Character },
             { "Camera", ICON_MDI_VIDEO, "Camera", Archetype::Camera },
        };

        // Category submenu order + icons (mirrors the Details Add-Component menu). Physics/Other are listed
        // for parity even while currently empty — the render loop skips a category with no matching archetype.
        struct AddCategory
        {
            const char* Icon;
            const char* Name;
        };
        constexpr AddCategory kAddCategories[] = {
             { ICON_MDI_SHAPE, "Rendering" }, { ICON_MDI_LIGHTBULB, "Lighting" },
             { ICON_MDI_RUN, "Animation" },   { ICON_MDI_VIDEO, "Camera" },
             { ICON_MDI_ATOM, "Physics" },    { ICON_MDI_DOTS_HORIZONTAL, "Other" },
        };

        // One undo step per spawn (Ctrl+Z removes what was just added).
        void Track( ECS::Entity& e )
        {
            Commands::NotifyCreated( { e.GetComponent<ECS::UUIDComponent>().UUID } );
        }

        void SpawnArchetype( Desert::Core::Scene& scene, Archetype kind )
        {
            switch ( kind )
            {
                case Archetype::Cube:
                {
                    // Primitive path (RuntimeMesh generated + Invalidated by MeshECSSystem) — renders +
                    // serializes reliably, unlike the builtin procedural-handle path.
                    auto e                                               = scene.CreateNewEntity( "Cube" );
                    e.AddComponent<ECS::StaticMeshComponent>().Primitive = Geometry::PrimitiveType::Cube;
                    Track( e );
                    break;
                }
                case Archetype::Model3D:
                {
                    auto e                                                = scene.CreateNewEntity( "3D Model" );
                    e.AddComponent<ECS::StaticMeshComponent>().MeshHandle = Assets::AssetHandle{ 0 };
                    Track( e );
                    break;
                }
                case Archetype::Skybox:
                {
                    auto e = scene.CreateNewEntity( "Skybox" );
                    e.AddComponent<ECS::SkyboxComponent>();
                    Track( e );
                    break;
                }
                case Archetype::Terrain:
                {
                    auto e = scene.CreateNewEntity( "Terrain" );
                    e.AddComponent<ECS::TerrainComponent>();
                    Track( e );
                    break;
                }
                case Archetype::DirLight:
                {
                    auto e = scene.CreateNewEntity( "Directional Light" );
                    e.AddComponent<ECS::DirectionLightComponent>();
                    Track( e );
                    break;
                }
                case Archetype::PointLight:
                {
                    auto e = scene.CreateNewEntity( "Point Light" );
                    e.AddComponent<ECS::PointLightComponent>();
                    Track( e );
                    break;
                }
                case Archetype::SpotLight:
                {
                    auto e = scene.CreateNewEntity( "Spot Light" );
                    e.AddComponent<ECS::SpotLightComponent>();
                    Track( e );
                    break;
                }
                case Archetype::SkinnedModel:
                {
                    auto e = scene.CreateNewEntity( "Skinned Model" );
                    e.AddComponent<ECS::SkinnedMeshComponent>();
                    e.AddComponent<ECS::AnimationComponent>();
                    Track( e );
                    break;
                }
                case Archetype::Character:
                {
                    // Code-generated rounded humanoid mannequin (no import). Renders in bind/A-pose; pick
                    // Idle/Walk/Run/Jump in Details ▸ Animation, or parent it to a Character Controller so
                    // LocomotionSystem drives it from movement.
                    auto e = scene.CreateNewEntity( "Character" );
                    e.AddComponent<ECS::SkinnedMeshComponent>().MeshHandle =
                         Geometry::ProceduralCharacterFactory::GetHumanoidMesh();
                    e.AddComponent<ECS::AnimationComponent>();
                    Track( e );
                    break;
                }
                case Archetype::Camera:
                {
                    // Spawn at the editor viewpoint (UE "Create Camera Here") instead of the origin, so the
                    // gizmo + frustum are immediately visible and frame the current view instead of clipping
                    // at the near plane behind the editor camera.
                    auto e = scene.CreateNewEntity( "Camera" );
                    e.AddComponent<ECS::CameraComponent>();
                    if ( auto active = scene.GetActiveCamera() )
                    {
                        const glm::mat4 world = glm::inverse( active->GetViewMatrix() );
                        const glm::vec3 eye   = glm::vec3( world[3] );
                        const glm::vec3 fwd   = -glm::normalize( glm::vec3( world[2] ) );

                        auto& tf       = e.GetComponent<ECS::TransformComponent>();
                        tf.Translation = eye + fwd * 4.0f;
                        tf.Rotation    = glm::eulerAngles( glm::quatLookAt( fwd, glm::vec3( 0.0f, 1.0f, 0.0f ) ) );
                    }
                    Track( e );
                    break;
                }
            }
        }
    } // namespace

    EntityTypeKind SceneHierarchyPanel::ClassifyEntity( const ECS::Entity& entity )
    {
        // Order is precedence, not taste: an entity carrying both a SkyAtmosphere and a Skybox is a
        // procedural sky that keeps an HDR fallback, and the atmosphere is what it is named for (the
        // same precedence ScenePropertiesPanel::GetPrimaryComponentName uses).
        if ( entity.HasComponent<ECS::FolderComponent>() )
            return EntityTypeKind::Folder;
        if ( entity.HasComponent<ECS::CameraComponent>() )
            return EntityTypeKind::Camera;
        if ( entity.HasComponent<ECS::DirectionLightComponent>() )
            return EntityTypeKind::DirectionalLight;
        if ( entity.HasComponent<ECS::PointLightComponent>() )
            return EntityTypeKind::PointLight;
        if ( entity.HasComponent<ECS::SpotLightComponent>() )
            return EntityTypeKind::SpotLight;
        if ( entity.HasComponent<ECS::SkyAtmosphereComponent>() )
            return EntityTypeKind::SkyAtmosphere;
        if ( entity.HasComponent<ECS::SkyboxComponent>() )
            return EntityTypeKind::Skybox;
        if ( entity.HasComponent<ECS::TerrainComponent>() )
            return EntityTypeKind::Terrain;
        if ( entity.HasComponent<ECS::SkinnedMeshComponent>() )
            return EntityTypeKind::SkinnedMesh;
        if ( entity.HasComponent<ECS::StaticMeshComponent>() )
            return EntityTypeKind::StaticMesh;
        if ( entity.HasComponent<ECS::TextComponent>() )
            return EntityTypeKind::Text;
        return EntityTypeKind::Actor;
    }

    const char* SceneHierarchyPanel::GetEntityTypeName( const ECS::Entity& entity )
    {
        return EntityTypeOf( ClassifyEntity( entity ) ).Name;
    }

    void SceneHierarchyPanel::DrawEntityNode( ECS::Entity& entity )
    {
        const std::string& name = entity.GetComponent<ECS::TagComponent>().Tag;
        const auto         UUID = entity.GetComponent<ECS::UUIDComponent>().UUID;

        if ( m_HierarchyFilter.IsActive() && !m_HierarchyFilter.PassFilter( name.c_str() ) )
            return;

        Utils::ImGuiUtilities::PushID();

        std::string uuidStr     = UUID.ToString();
        bool        hasChildren = entity.HasComponent<ECS::RelationshipComponent>() &&
                           !entity.GetComponent<ECS::RelationshipComponent>().Children.empty();

        m_VisibleOrder.push_back( UUID ); // visible draw order (Shift+click range source)

        const bool isSelected = Core::SelectionManager::IsSelected( UUID );

        ImGuiTreeNodeFlags nodeFlags = isSelected ? ImGuiTreeNodeFlags_Selected : 0;
        nodeFlags |= ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_FramePadding |
                     ImGuiTreeNodeFlags_AllowItemOverlap | ImGuiTreeNodeFlags_SpanAvailWidth;
        if ( !hasChildren )
            nodeFlags |= ImGuiTreeNodeFlags_Leaf;

        const bool isPrefab = entity.HasComponent<ECS::PrefabComponent>();

        // Hidden entities (VisibilityComponent.Visible == false) render dimmed with an "eye-off" toggle.
        const bool visible = !entity.HasComponent<ECS::VisibilityComponent>() ||
                             entity.GetComponent<ECS::VisibilityComponent>().Visible;

        // Locked entities (LockComponent present) refuse viewport picking and gizmo edits. Asked through
        // the shared predicate rather than HasComponent here, so the outliner's padlock and the two places
        // that ENFORCE it can never be reading different rules.
        const bool locked = ECS::IsLocked( *entity.GetRegistry(), entity.GetHandle() );

        const char* icon = ICON_MDI_CUBE_OUTLINE;
        if ( entity.HasComponent<ECS::FolderComponent>() )
            icon = ICON_MDI_FOLDER;
        else if ( entity.HasComponent<ECS::CameraComponent>() )
            icon = ICON_MDI_CAMERA;
        else if ( entity.HasComponent<ECS::SpotLightComponent>() )
            icon = ICON_MDI_SPOTLIGHT;
        else if ( entity.HasComponent<ECS::DirectionLightComponent>() ||
                  entity.HasComponent<ECS::PointLightComponent>() )
            icon = ICON_MDI_LIGHTBULB;
        else if ( entity.HasComponent<ECS::SkyAtmosphereComponent>() )
            icon = ICON_MDI_WEATHER_SUNSET;
        else if ( entity.HasComponent<ECS::SkyboxComponent>() )
            icon = ICON_MDI_EARTH;
        else if ( entity.HasComponent<ECS::TerrainComponent>() )
            icon = ICON_MDI_TERRAIN;
        else if ( entity.HasComponent<ECS::TextComponent>() )
            icon = ICON_MDI_FORMAT_TEXT;
        else if ( isPrefab )
            icon = ICON_MDI_PACKAGE_VARIANT;

        // What the outliner calls this row, looked up ONCE: the icon's colour, the Type column's text and
        // the column's own width are then three readings of one census rather than three opinions.
        const EntityTypeKind  typeKind  = ClassifyEntity( entity );
        const EntityTypeInfo& typeInfo  = EntityTypeOf( typeKind );
        const ImVec4          typeColor = ImVec4( typeInfo.R, typeInfo.G, typeInfo.B, 1.0f );

        // Column 0: icon tree node + name
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex( 0 );

        // The icon takes the TYPE's colour, which is the whole point of having one: a list of thirty rows
        // is sorted into meshes / lights / environment / gameplay before a single name is read. A prefab
        // root still overrides it — "this is an instance of something" outranks "this is a mesh", and it
        // is the fact you can act on.
        const ImVec4 iconColor = isPrefab ? ImVec4( 0.3f, 0.9f, 0.8f, 1.0f ) : typeColor;

        // Reveal-on-select: open this node once if it's an ancestor of the newly-selected entity, and scroll
        // the selected row into view. Applied a single frame so the user can still collapse afterwards.
        if ( m_ExpandToSelection.erase( static_cast<uint64_t>( UUID ) ) > 0 )
            ImGui::SetNextItemOpen( true );

        ImGui::PushStyleColor( ImGuiCol_Text, iconColor );
        bool nodeOpen = ImGui::TreeNodeEx( (void*)(uint64_t)entity.GetHandle(), nodeFlags, "%s", icon );
        ImGui::PopStyleColor();

        if ( isSelected && m_ScrollToSelection )
        {
            ImGui::SetScrollHereY( 0.5f );
            m_ScrollToSelection = false;
        }

        if ( ImGui::IsItemClicked() )
        {
            // UE-style modifiers: Ctrl toggles, Shift extends the range from the primary, plain replaces.
            ImGuiIO& io = ImGui::GetIO();
            if ( io.KeyCtrl )
                Core::SelectionManager::Toggle( UUID );
            else if ( io.KeyShift )
                SelectRangeTo( UUID );
            else
                Core::SelectionManager::SetSelected( UUID );
        }

        if ( ImGui::BeginDragDropSource() )
        {
            ImGui::SetDragDropPayload( ::Desert::Editor::DragPayloads::EntityRelationship, &UUID,
                                       sizeof( Common::UUID ) );
            ImGui::TextUnformatted( name.c_str() );
            ImGui::EndDragDropSource();
        }

        if ( ImGui::BeginDragDropTarget() )
        {
            if ( const ImGuiPayload* payload =
                      ImGui::AcceptDragDropPayload( ::Desert::Editor::DragPayloads::EntityRelationship ) )
            {
                // Dragging a multi-selected entity drags the whole selection. Applied after iteration.
                Common::UUID childUUID = *(const Common::UUID*)payload->Data;
                m_PendingReparent      = { Core::SelectionManager::IsSelected( childUUID )
                                                ? Core::SelectionManager::GetSelection()
                                                : std::vector<Common::UUID>{ childUUID },
                                      UUID };
            }
            ImGui::EndDragDropTarget();
        }

        ImGui::SameLine();
        if ( m_RenamingEntity.has_value() && *m_RenamingEntity == UUID )
        {
            // Inline rename: commit on Enter/click-away, cancel on Escape.
            ImGui::SetNextItemWidth( -FLT_MIN );
            if ( m_RenameFocusPending )
            {
                ImGui::SetKeyboardFocusHere();
                m_RenameFocusPending = false;
            }
            Utils::ImGuiUtilities::InputText( m_RenameBuffer, "##EntityRename" );
            if ( ImGui::IsItemDeactivated() )
            {
                if ( !ImGui::IsKeyPressed( ImGuiKey_Escape, false ) )
                    Commands::Rename( UUID, m_RenameBuffer ); // undoable; no-ops on empty/unchanged
                m_RenamingEntity.reset();
            }
        }
        else
        {
            // Name colour: prefab teal, dimmed when hidden. Prefab tag stays subtle.
            ImVec4 nameColor = isPrefab ? iconColor : ImGui::GetStyleColorVec4( ImGuiCol_Text );
            if ( !visible )
                nameColor = ImGui::GetStyleColorVec4( ImGuiCol_TextDisabled );
            ImGui::PushStyleColor( ImGuiCol_Text, nameColor );
            ImGui::TextUnformatted( name.c_str() );
            ImGui::PopStyleColor();
            if ( ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked( ImGuiMouseButton_Left ) )
            {
                // Double-click FRAMES the entity in the viewport — the most frequent
                // hierarchy->viewport hop gets the fastest gesture. Renaming lives in the
                // context menu, where slower, deliberate edits belong.
                if ( auto cam = m_Scene->GetMainCamera().lock() )
                    if ( auto* editorCam = dynamic_cast<::Desert::Core::EditorCamera*>( cam.get() ) )
                        if ( auto ref = m_Scene->FindEntityByID( UUID ) )
                            editorCam->Focus( glm::vec3( ref->get().GetWorldTransform()[3] ) );
            }
        }
        if ( isPrefab )
        {
            ImGui::SameLine();
            ImGui::TextDisabled( "[Prefab]" );
        }
        // Lua-script badge: a small green script glyph when the entity carries a ScriptComponent, so scripted
        // entities are spottable at a glance in the hierarchy.
        if ( entity.HasComponent<ECS::ScriptComponent>() )
        {
            ImGui::SameLine();
            ImGui::TextColored( ImVec4( 0.45f, 0.85f, 0.45f, 1.0f ), ICON_MDI_LANGUAGE_LUA );
            if ( ImGui::IsItemHovered() )
                ImGui::SetTooltip( "Has a Lua script" );
        }

        bool deleteEntity = false;
        if ( ImGui::BeginPopupContextItem( uuidStr.c_str() ) )
        {
            if ( ImGui::Selectable( "Rename" ) )
            {
                m_RenamingEntity     = UUID;
                m_RenameBuffer       = name;
                m_RenameFocusPending = true;
            }
            // An operation on a MULTI-selected entity applies to the whole selection (UE behavior).
            auto targetSet = [&]() -> std::vector<Common::UUID>
            {
                if ( Core::SelectionManager::IsSelected( UUID ) )
                    return Core::SelectionManager::GetSelection();
                return { UUID };
            };
            if ( ImGui::Selectable( Core::SelectionManager::Count() > 1 &&
                                              Core::SelectionManager::IsSelected( UUID )
                                         ? "Duplicate (selection)"
                                         : "Duplicate" ) )
                m_PendingDuplicate = targetSet(); // deferred: cloning creates entities mid-iteration
            if ( ImGui::Selectable( Core::SelectionManager::Count() > 1 &&
                                              Core::SelectionManager::IsSelected( UUID )
                                         ? "Delete (selection)"
                                         : "Delete" ) )
                deleteEntity = true;

            // The padlock gutter is a 16px target; the menu is how you find the feature at all, and how a
            // whole selection is locked in one go. Both spellings call the same recursive setter.
            if ( ImGui::Selectable( locked ? ICON_MDI_LOCK_OPEN_OUTLINE " Unlock"
                                           : ICON_MDI_LOCK " Lock (no picking, no gizmo)" ) )
            {
                auto& reg = *entity.GetRegistry();
                if ( Core::SelectionManager::Count() > 1 && Core::SelectionManager::IsSelected( UUID ) )
                {
                    for ( const auto& id : Core::SelectionManager::GetSelection() )
                        if ( auto ref = m_Scene->FindEntityByID( id ) )
                            ECS::SetLockedRecursive( reg, ref->get().GetHandle(), !locked );
                }
                else
                {
                    ECS::SetLockedRecursive( reg, entity.GetHandle(), !locked );
                }
            }

            ImGui::Separator();
            if ( ImGui::Selectable( "Add Child" ) )
            {
                auto child = m_Scene->CreateNewEntity( "Child Entity" );
                m_Scene->Attach( entity, child );
                Commands::NotifyCreated( { child.GetComponent<ECS::UUIDComponent>().UUID } );
            }

            ImGui::Separator();
            // Add a component straight from the outliner, using the SAME grouped menu as Details (shared
            // DrawAddComponentMenu — the categorization is defined in exactly one place).
            if ( ImGui::BeginMenu( ICON_MDI_PLUS_BOX_OUTLINE " Add Component" ) )
            {
                DrawAddComponentMenu( entity, m_AddComponentFilter );
                ImGui::EndMenu();
            }

            if ( ImGui::Selectable( ICON_MDI_PACKAGE_VARIANT " Save as Prefab..." ) )
            {
                // Default path from the entity's tag (spaces -> underscores).
                std::string stem = name;
                for ( auto& ch : stem )
                    if ( ch == ' ' )
                        ch = '_';
                m_SavePrefabTarget = UUID;
                m_SavePrefabPath   = "Resources/Assets/Prefabs/" + stem +
                                   std::string( Common::Constants::Extensions::PREFAB_EXTENSION );
                m_OpenSavePrefab = true; // deferred: OpenPopup at panel scope (see OnUIRender)
            }
            if ( ImGui::Selectable( ICON_MDI_PACKAGE_VARIANT " Instantiate Prefab..." ) )
            {
                // UNDER THIS ENTITY. Asked from an entity's own context menu, "instantiate" means "in
                // here" — and for a UI prefab it is the difference between an element the canvas walk
                // reaches and one it does not.
                m_InstantiatePrefabParent = UUID;
                m_OpenInstantiatePrefab   = true; // deferred: OpenPopup at panel scope (see OnUIRender)
            }

            if ( isPrefab )
            {
                ImGui::Separator();
                if ( ImGui::Selectable( ICON_MDI_PACKAGE_UP " Apply Instance Changes to Prefab..." ) )
                {
                    m_ApplyPrefabTarget = UUID;
                    m_OpenApplyPrefab   = true; // confirmation modal (overwrites the source file)
                }
                if ( ImGui::Selectable( ICON_MDI_PACKAGE_DOWN " Revert Instance to Prefab" ) )
                    m_PendingPrefabRevert = UUID; // deferred: replaces the subtree after iteration
                if ( ImGui::Selectable( ICON_MDI_PACKAGE_VARIANT_CLOSED " Unpack Prefab (make local)" ) )
                    m_PendingPrefabUnpack = UUID; // deferred: strips the prefab link from the subtree
            }

            ImGui::EndPopup();
        }

        // Column 1: the type, in its family colour and in a column wide enough for it. Dimmed rather than
        // recoloured when the entity is hidden — a hidden row should read as one thing, not as a coloured
        // label attached to grey text.
        ImGui::TableSetColumnIndex( 1 );
        ImGui::PushStyleColor( ImGuiCol_Text,
                               visible ? typeColor : ImGui::GetStyleColorVec4( ImGuiCol_TextDisabled ) );
        ImGui::TextUnformatted( typeInfo.Name );
        ImGui::PopStyleColor();

        // Column 2: the visibility eye, in its own fixed gutter so it can never push the type out.
        ImGui::TableSetColumnIndex( 2 );
        ImGui::PushStyleColor( ImGuiCol_Text, visible ? ThemeManager::GetIconColor()
                                                      : ImGui::GetStyleColorVec4( ImGuiCol_TextDisabled ) );
        ImGui::TextUnformatted( visible ? ICON_MDI_EYE_OUTLINE : ICON_MDI_EYE_OFF_OUTLINE );
        ImGui::PopStyleColor();
        if ( ImGui::IsItemClicked() )
            m_Scene->SetVisibleRecursive( entity, !visible ); // toggles the entity + its subtree
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( visible ? "Hide" : "Show" );

        // Column 3: the authoring padlock, beside the eye because the two answer the same shape of
        // question about a row — "is this in my way?". Closed means the viewport will not pick it and the
        // gizmo will not drag it; this row stays clickable, which is deliberately the only way back.
        ImGui::TableSetColumnIndex( 3 );
        ImGui::PushStyleColor( ImGuiCol_Text, locked ? ImGui::GetStyleColorVec4( ImGuiCol_TextDisabled )
                                                     : ThemeManager::GetIconColor() );
        ImGui::TextUnformatted( locked ? ICON_MDI_LOCK : ICON_MDI_LOCK_OPEN_OUTLINE );
        ImGui::PopStyleColor();
        if ( ImGui::IsItemClicked() )
            ECS::SetLockedRecursive( *entity.GetRegistry(), entity.GetHandle(), !locked );
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( locked ? "Unlock (allow picking and gizmo edits)"
                                      : "Lock (refuse picking and gizmo edits)" );

        if ( nodeOpen )
        {
            if ( hasChildren )
            {
                auto& rel      = entity.GetComponent<ECS::RelationshipComponent>();
                auto& registry = *entity.GetRegistry();
                for ( auto childHandle : rel.Children )
                {
                    ECS::Entity child( childHandle, registry );
                    DrawEntityNode( child );
                }
            }
            ImGui::TreePop();
        }

        // Deferred: destroying here would invalidate the entt view being iterated in OnUIRender
        // (deleting a parent destroys its whole subtree at once). Process after EndTable.
        if ( deleteEntity )
        {
            m_PendingDelete = Core::SelectionManager::IsSelected( UUID ) ? Core::SelectionManager::GetSelection()
                                                                         : std::vector<Common::UUID>{ UUID };
        }

        Utils::ImGuiUtilities::PopID();
    }

    void SceneHierarchyPanel::SelectRangeTo( const Common::UUID& target )
    {
        // Range = [primary .. target] over the order the user SEES (last frame's visible tree order).
        const auto& order   = m_VisibleOrderLast;
        const auto& primary = Core::SelectionManager::GetSelected();

        const auto find = [&]( const Common::UUID& id ) -> int
        {
            for ( size_t i = 0; i < order.size(); ++i )
                if ( order[i] == id )
                    return static_cast<int>( i );
            return -1;
        };

        const int from = primary.has_value() ? find( *primary ) : -1;
        const int to   = find( target );
        if ( from < 0 || to < 0 )
        {
            Core::SelectionManager::SetSelected( target );
            return;
        }

        std::vector<Common::UUID> range;
        const int                 lo = std::min( from, to ), hi = std::max( from, to );
        for ( int i = lo; i <= hi; ++i )
            if ( order[i] != target )
                range.push_back( order[i] );
        range.push_back( target ); // clicked entity becomes the new primary
        Core::SelectionManager::SetSelection( std::move( range ) );
    }

    void SceneHierarchyPanel::OnUIRender()
    {
        // Rotate the visible-order buffers: ranges act on what was drawn (and thus seen) last frame.
        m_VisibleOrderLast = std::move( m_VisibleOrder );
        m_VisibleOrder.clear();

        ImRect windowRect = { ImGui::GetWindowContentRegionMin(), ImGui::GetWindowContentRegionMax() };

        // NOTE: no standalone "Material" entity — a MaterialComponent is meaningless without geometry. It is
        // added ONTO a renderable entity via Details -> Add Component. The Cornell Box / LOD-grid demo
        // builders were removed on purpose — showcase content is DATA now (Assets/Scenes/*.desce).
        auto AddEntity = [this]( const std::shared_ptr<Desert::Core::Scene>& scene,
                                 const std::shared_ptr<Assets::AssetManager>& /*assetManager*/ )
        {
            if ( !ImGui::BeginMenu( "Add" ) )
                return;

            // Search box (mirrors the Details Add-Component menu).
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted( ICON_MDI_MAGNIFY );
            ImGui::SameLine();
            float filterW = ImGui::GetContentRegionAvail().x - ImGui::GetStyle().IndentSpacing;
            m_AddEntityFilter.Draw( "##AddEntityFilter", filterW < 220.0f ? 220.0f : filterW );
            ImGui::Separator();

            if ( m_AddEntityFilter.IsActive() )
            {
                // Flat filtered list while searching — categories only get in the way of a query.
                if ( m_AddEntityFilter.PassFilter( "Empty Entity" ) &&
                     ImGui::Selectable( ICON_MDI_CUBE_OUTLINE "  Empty Entity" ) )
                {
                    auto e = scene->CreateNewEntity( "Empty Entity" );
                    Track( e );
                }
                if ( m_AddEntityFilter.PassFilter( "Folder" ) && ImGui::Selectable( ICON_MDI_FOLDER "  Folder" ) )
                {
                    auto e = scene->CreateNewEntity( "Folder" );
                    e.AddComponent<ECS::FolderComponent>();
                    Track( e );
                }
                for ( const auto& a : kArchetypes )
                    if ( m_AddEntityFilter.PassFilter( a.Label ) &&
                         ImGui::Selectable( ( std::string( a.Icon ) + "  " + a.Label ).c_str() ) )
                        SpawnArchetype( *scene, a.Kind );
                ImGui::EndMenu();
                return;
            }

            // Empty + Folder live at the top (Folder must stay reachable, never buried in a submenu).
            if ( ImGui::Selectable( ICON_MDI_CUBE_OUTLINE "  Empty Entity" ) )
            {
                auto e = scene->CreateNewEntity( "Empty Entity" );
                Track( e );
            }
            // Folder: an empty grouping node. Drag entities onto it to group them, or select entities first
            // and use "Group Selected into Folder" below to auto-parent them.
            if ( ImGui::Selectable( ICON_MDI_FOLDER "  Folder" ) )
            {
                auto e = scene->CreateNewEntity( "Folder" );
                e.AddComponent<ECS::FolderComponent>();
                Track( e );
            }
            if ( Core::SelectionManager::Count() > 0 &&
                 ImGui::Selectable( ICON_MDI_FOLDER_PLUS "  Group Selected into Folder" ) )
            {
                auto grp = scene->CreateNewEntity( "Folder" );
                grp.AddComponent<ECS::FolderComponent>();
                Track( grp );
                for ( const auto& id : Core::SelectionManager::GetSelection() )
                    if ( const auto e = scene->FindEntityByID( id ) )
                        scene->Attach( grp, e->get() );
            }

            ImGui::Separator();

            for ( const auto& cat : kAddCategories )
            {
                bool any = false;
                for ( const auto& a : kArchetypes )
                    if ( std::strcmp( a.Category, cat.Name ) == 0 )
                    {
                        any = true;
                        break;
                    }
                if ( !any )
                    continue;

                if ( ImGui::BeginMenu( ( std::string( cat.Icon ) + "  " + cat.Name ).c_str() ) )
                {
                    for ( const auto& a : kArchetypes )
                        if ( std::strcmp( a.Category, cat.Name ) == 0 &&
                             ImGui::Selectable( ( std::string( a.Icon ) + "  " + a.Label ).c_str() ) )
                            SpawnArchetype( *scene, a.Kind );
                    ImGui::EndMenu();
                }
            }

            ImGui::EndMenu();
        };

        ImGui::PushStyleColor( ImGuiCol_MenuBarBg, ImGui::GetStyleColorVec4( ImGuiCol_TabActive ) );

        if ( ImGui::Button( ICON_MDI_PLUS ) )
            ImGui::OpenPopup( "AddEntity" );

        if ( ImGui::BeginPopup( "AddEntity" ) )
        {
            AddEntity( m_Scene, m_AssetManager );
            ImGui::EndPopup();
        }

        ImGui::SameLine();
        ImGui::TextUnformatted( ICON_MDI_MAGNIFY );
        ImGui::SameLine();

        ImGui::PushFont( EditorResources::GetBoldFont() );
        ImGui::PushStyleVar( ImGuiStyleVar_FrameBorderSize, 0.0f );
        ImGui::PushStyleColor( ImGuiCol_FrameBg, IM_COL32( 0, 0, 0, 0 ) );
        m_HierarchyFilter.Draw( "##HierarchyFilter",
                                ImGui::GetContentRegionAvail().x - ImGui::GetStyle().IndentSpacing );
        Utils::ImGuiUtilities::DrawItemActivityOutline( 2.0f, false, ImColor( 80, 80, 80 ) );

        bool isFilterFocused = ImGui::IsItemActive();

        ImGui::PopFont();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();

        if ( !m_HierarchyFilter.IsActive() && !isFilterFocused )
        {
            ImGui::SameLine();
            ImGui::PushFont( EditorResources::GetBoldFont() );
            ImGui::SetCursorPosX( ImGui::GetFontSize() * 4.0f );
            ImGui::PushStyleVar( ImGuiStyleVar_FramePadding, ImVec2( 0.0f, ImGui::GetStyle().FramePadding.y ) );
            ImGui::TextUnformatted( "Search..." );
            ImGui::PopFont();
            ImGui::PopStyleVar();
        }

        ImGui::PopStyleColor();

        if ( ImGui::BeginPopupContextWindow() )
        {
            ImGui::Separator();
            AddEntity( m_Scene, m_AssetManager );
            ImGui::Separator();
            if ( ImGui::MenuItem( ICON_MDI_PACKAGE_VARIANT " Instantiate Prefab..." ) )
            {
                m_InstantiatePrefabParent.reset(); // the blank area IS the scene root
                m_OpenInstantiatePrefab = true;    // deferred: OpenPopup at panel scope below
            }
            ImGui::EndPopup();
        }

        // Open the modal at panel scope (NOT inside a context popup — that ID mismatch was why it never
        // opened). Both context-menu triggers set the flag; we open + draw here.
        if ( m_OpenInstantiatePrefab )
        {
            ImGui::OpenPopup( "InstantiatePrefabPopup" );
            m_OpenInstantiatePrefab = false;
        }
        DrawInstantiatePrefabPopup();

        if ( m_OpenSavePrefab )
        {
            ImGui::OpenPopup( "SavePrefabPopup" );
            m_OpenSavePrefab = false;
        }
        DrawSavePrefabPopup();

        if ( m_OpenApplyPrefab )
        {
            ImGui::OpenPopup( "ApplyPrefabPopup" );
            m_OpenApplyPrefab = false;
        }
        if ( ImGui::BeginPopupModal( "ApplyPrefabPopup", nullptr, ImGuiWindowFlags_AlwaysAutoResize ) )
        {
            static std::string s_applyError;

            ImGui::TextUnformatted( "Overwrite the source .deprefab file with this instance's current state?" );
            ImGui::TextDisabled( "Other instances pick the changes up when they are (re)instantiated." );
            if ( !s_applyError.empty() )
                ImGui::TextColored( ImVec4( 1.0f, 0.4f, 0.4f, 1.0f ), "%s", s_applyError.c_str() );
            ImGui::Spacing();
            if ( ImGui::Button( "Apply", ImVec2( 120, 0 ) ) )
            {
                // The popup used to close whatever ApplyPrefabInstance answered, so a refused apply
                // looked exactly like a successful one. It closes only when the file was written.
                if ( m_ApplyPrefabTarget && !Commands::ApplyPrefabInstance( *m_ApplyPrefabTarget ) )
                {
                    s_applyError = "The prefab file was NOT written (see the log). Nothing has changed.";
                }
                else
                {
                    s_applyError.clear();
                    m_ApplyPrefabTarget.reset();
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::SameLine();
            if ( ImGui::Button( "Cancel", ImVec2( 90, 0 ) ) )
            {
                s_applyError.clear();
                m_ApplyPrefabTarget.reset();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        // Keyboard (outliner focused, not while typing in a field):
        //   Delete — remove the whole selection; F2 — rename the primary; Esc — clear the selection.
        if ( ImGui::IsWindowFocused( ImGuiFocusedFlags_RootAndChildWindows ) && !ImGui::IsAnyItemActive() )
        {
            if ( ImGui::IsKeyPressed( ImGuiKey_Delete, false ) && Core::SelectionManager::Count() > 0 )
                m_PendingDelete = Core::SelectionManager::GetSelection();

            if ( ImGui::IsKeyPressed( ImGuiKey_F2, false ) )
            {
                if ( const auto& sel = Core::SelectionManager::GetSelected() )
                    if ( auto ref = m_Scene->FindEntityByID( *sel ) )
                    {
                        m_RenamingEntity     = *sel;
                        m_RenameBuffer       = ref->get().GetComponent<ECS::TagComponent>().Tag;
                        m_RenameFocusPending = true;
                    }
            }

            if ( ImGui::IsKeyPressed( ImGuiKey_Escape, false ) && !m_RenamingEntity.has_value() )
                Core::SelectionManager::ClearSelection();
        }

        // Entity / selection counters (compact, above the table).
        {
            const size_t total = m_Scene->GetAllEntities().size();
            const size_t sel   = Core::SelectionManager::Count();
            if ( sel > 0 )
                ImGui::TextDisabled( "%zu entities, %zu selected", total, sel );
            else
                ImGui::TextDisabled( "%zu entities", total );
        }

        // Entity table
        {
            // Name, Type, and a fixed gutter for EACH of the two per-row toggles. The old two-column form
            // made the Type column carry the visibility eye as well — and an eye plus "StaticMeshActor"
            // never fit in 110px, so the type was clipped on every row of the Starter scene. Each toggle
            // now has its own gutter and the type column is sized off the census, so it fits the widest
            // name it can ever print (EntityTypeCensus.hpp).
            //
            // (This comment said "FOUR columns" while the call below asked for three, from the time the eye
            // was split out. It is four now for real; the count is written once, below, and read from there.)
            //
            // Resizable stays: the user can still widen Name at the Type column's expense. What changed is
            // the DEFAULT, which is what every fresh layout gets.
            constexpr ImGuiTableFlags tableFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                                                   ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_Resizable;

            // A toggle gutter: one glyph plus the cell padding it is drawn inside. Measured on the WIDEST
            // glyph either gutter can show, so neither clips when its icon changes with the row's state.
            const float gutterWidth =
                 std::max( { ImGui::CalcTextSize( ICON_MDI_EYE_OUTLINE ).x, ImGui::CalcTextSize( ICON_MDI_LOCK ).x,
                             ImGui::CalcTextSize( ICON_MDI_LOCK_OPEN_OUTLINE ).x } ) +
                 8.0f;
            const float typeWidth =
                 TypeColumnWidth( []( const char* s ) { return ImGui::CalcTextSize( s ).x; }, 12.0f );

            ImGui::PushStyleVar( ImGuiStyleVar_CellPadding, ImVec2( 4.0f, 2.0f ) );
            if ( ImGui::BeginTable( "##outliner", 4, tableFlags ) )
            {
                ImGui::TableSetupColumn( "Name", ImGuiTableColumnFlags_WidthStretch );
                ImGui::TableSetupColumn( "Type", ImGuiTableColumnFlags_WidthFixed, typeWidth );
                // NoResize on the gutters: each holds one glyph, and a user who drags one to nothing loses
                // the only control the outliner has for that state.
                ImGui::TableSetupColumn( "##visible",
                                         ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoResize |
                                              ImGuiTableColumnFlags_NoHeaderLabel,
                                         gutterWidth );
                ImGui::TableSetupColumn( "##locked",
                                         ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoResize |
                                              ImGuiTableColumnFlags_NoHeaderLabel,
                                         gutterWidth );
                ImGui::TableHeadersRow();

                auto& registry = m_Scene->GetRegistry();

                // Reveal-on-select: when the primary selection changed since last frame, collect its ancestor
                // chain so DrawEntityNode force-opens those nodes (and scrolls to the selected one) — a UI
                // element picked in the viewport unfolds to it in the outliner instead of staying hidden.
                {
                    const auto&  sel = Core::SelectionManager::GetSelected();
                    Common::UUID cur = sel.has_value() ? *sel : Common::UUID::Null();
                    if ( static_cast<uint64_t>( cur ) != static_cast<uint64_t>( m_LastRevealedSelection ) )
                    {
                        m_LastRevealedSelection = cur;
                        m_ExpandToSelection.clear();
                        m_ScrollToSelection = false;
                        if ( static_cast<uint64_t>( cur ) != 0 )
                            if ( auto ref = m_Scene->FindEntityByID( cur ) )
                            {
                                entt::entity e = ref->get().GetHandle();
                                while ( registry.has<ECS::RelationshipComponent>( e ) )
                                {
                                    const entt::entity p = registry.get<ECS::RelationshipComponent>( e ).Parent;
                                    if ( p == entt::null )
                                        break;
                                    if ( registry.has<ECS::UUIDComponent>( p ) )
                                        m_ExpandToSelection.insert(
                                             static_cast<uint64_t>( registry.get<ECS::UUIDComponent>( p ).UUID ) );
                                    e = p;
                                }
                                m_ScrollToSelection = true;
                            }
                    }
                }

                auto view = registry.view<ECS::UUIDComponent>();
                for ( auto entityHandle : view )
                {
                    ECS::Entity entity( entityHandle, registry );
                    bool        hasParent = entity.HasComponent<ECS::RelationshipComponent>() &&
                                     entity.GetComponent<ECS::RelationshipComponent>().Parent != entt::null;
                    if ( !hasParent )
                        DrawEntityNode( entity );
                }

                ImGui::EndTable();
            }
            ImGui::PopStyleVar();

            if ( ImGui::BeginDragDropTargetCustom( windowRect, ImGui::GetCurrentWindow()->ID ) )
            {
                if ( const ImGuiPayload* payload =
                          ImGui::AcceptDragDropPayload( ::Desert::Editor::DragPayloads::EntityRelationship ) )
                {
                    // Drop on empty space = unparent to scene root (undoable, applied after iteration).
                    Common::UUID uuid = *(const Common::UUID*)payload->Data;
                    m_PendingReparent = { Core::SelectionManager::IsSelected( uuid )
                                               ? Core::SelectionManager::GetSelection()
                                               : std::vector<Common::UUID>{ uuid },
                                          Common::UUID::Null() };
                }

                if ( const ImGuiPayload* payload =
                          ImGui::AcceptDragDropPayload( ::Desert::Editor::DragPayloads::PrefabFile ) )
                {
                    std::string path( static_cast<const char*>( payload->Data ),
                                      static_cast<size_t>( payload->DataSize ) - 1 );
                    if ( m_AssetManager )
                    {
                        auto prefabAsset = m_AssetManager->FindByPath<Assets::PrefabAsset>( path );
                        if ( !prefabAsset )
                        {
                            prefabAsset =
                                 const_cast<Assets::AssetManager&>( *m_AssetManager )
                                      .CreateAsset<Assets::PrefabAsset>( Assets::AssetPriority::High, path );
                        }
                        if ( prefabAsset )
                        {
                            if ( !prefabAsset->IsReadyForUse() )
                                prefabAsset->Load();
                            // Dropped on the EMPTY area of the outliner, which is the scene root — the
                            // same thing this drop target means for an entity dragged onto it.
                            const auto placed =
                                 prefabAsset->Instantiate( m_Scene.get(), *m_AssetManager, {}, nullptr );
                            if ( !placed )
                            {
                                LOG_ERROR( "{}", placed.GetError() );
                            }
                            else if ( const ECS::Entity root = placed.GetValue() )
                            {
                                Commands::NotifyCreated( { root.GetComponent<ECS::UUIDComponent>().UUID } );
                            }
                        }
                    }
                }

                ImGui::EndDragDropTarget();
            }
        }

        // Process deferred structural edits AFTER the entt view iteration above (they create/destroy
        // entities or edit the Children vectors the tree walk was iterating).
        if ( !m_PendingDelete.empty() )
        {
            Commands::DeleteEntities( m_PendingDelete ); // one undo step restores everything
            m_PendingDelete.clear();
        }
        if ( !m_PendingDuplicate.empty() )
        {
            if ( auto dups = Commands::DuplicateEntities( m_PendingDuplicate ); !dups.empty() )
                Core::SelectionManager::SetSelection( std::move( dups ) );
            m_PendingDuplicate.clear();
        }
        if ( m_PendingReparent.has_value() )
        {
            Commands::ReparentMany( m_PendingReparent->first, m_PendingReparent->second );
            m_PendingReparent.reset();
        }
        if ( m_PendingPrefabRevert.has_value() )
        {
            Commands::RevertPrefabInstance( *m_PendingPrefabRevert ); // one undo step
            m_PendingPrefabRevert.reset();
        }
        if ( m_PendingPrefabUnpack.has_value() )
        {
            // Make local: strip the PrefabComponent link from the whole instance subtree so it becomes a
            // plain, freely-editable entity hierarchy (no more Apply/Revert to a source prefab).
            if ( auto root = m_Scene->FindEntityByID( *m_PendingPrefabUnpack ) )
            {
                auto&                               reg   = m_Scene->GetRegistry();
                std::function<void( entt::entity )> strip = [&]( entt::entity e )
                {
                    if ( reg.has<ECS::PrefabComponent>( e ) )
                        reg.remove<ECS::PrefabComponent>( e );
                    if ( reg.has<ECS::RelationshipComponent>( e ) )
                        for ( auto c : reg.get<ECS::RelationshipComponent>( e ).Children )
                            if ( reg.valid( c ) )
                                strip( c );
                };
                strip( root->get().GetHandle() );
            }
            m_PendingPrefabUnpack.reset();
        }
    }

    void SceneHierarchyPanel::DrawSavePrefabPopup()
    {
        if ( !ImGui::BeginPopupModal( "SavePrefabPopup", nullptr, ImGuiWindowFlags_AlwaysAutoResize ) )
            return;

        static std::string s_error;

        ImGui::TextUnformatted( "Save entity (with its whole subtree) as a prefab:" );
        ImGui::SetNextItemWidth( 460.0f );
        Utils::ImGuiUtilities::InputText( m_SavePrefabPath, "##SavePrefabPath" );
        ImGui::TextDisabled( "An existing file at this path will be overwritten." );

        if ( !s_error.empty() )
            ImGui::TextColored( ImVec4( 1.0f, 0.4f, 0.4f, 1.0f ), "%s", s_error.c_str() );

        ImGui::Spacing();

        if ( ImGui::Button( "Save", ImVec2( 120, 0 ) ) && !m_SavePrefabPath.empty() )
        {
            namespace fs = std::filesystem;
            const bool rightExt =
                 fs::path( m_SavePrefabPath ).extension() == Common::Constants::Extensions::PREFAB_EXTENSION;
            std::optional<std::reference_wrapper<const ECS::Entity>> entityRef;
            if ( m_SavePrefabTarget )
                entityRef = m_Scene->FindEntityByID( *m_SavePrefabTarget );

            if ( !rightExt )
            {
                s_error = "Path must end with .deprefab";
            }
            else if ( !entityRef || !m_AssetManager )
            {
                s_error = "Entity no longer exists.";
            }
            else
            {
                std::error_code ec;
                fs::create_directories( fs::path( m_SavePrefabPath ).parent_path(), ec );

                auto prefabAsset = m_AssetManager->FindByPath<Assets::PrefabAsset>( m_SavePrefabPath );
                if ( !prefabAsset )
                    prefabAsset = m_AssetManager->CreateAsset<Assets::PrefabAsset>(
                         Assets::AssetPriority::High, m_SavePrefabPath,
                         /*loadAfterCreate=*/false ); // the file does not exist yet — we are creating it

                if ( !prefabAsset )
                {
                    s_error = "Could not create the prefab asset.";
                }
                else
                {
                    ECS::Entity root = entityRef->get();
                    prefabAsset->CreateFromEntity( root, *m_AssetManager );
                    const auto written = Common::Utils::FileSystem::WriteContentToFileAtomic(
                         Common::Filepath( m_SavePrefabPath ), prefabAsset->Serialize() );

                    if ( !written )
                    {
                        // The PrefabComponent below is NOT attached on a failed write. It would make
                        // the live entity declare itself an instance of a file that does not exist —
                        // the hierarchy draws it with a prefab badge, "Revert to Prefab" reverts it to
                        // nothing, and the next scene save persists the dangling reference.
                        s_error = "The prefab file was NOT written: " + written.GetError();
                    }
                    else
                    {
                        // Mark the live entity as an instance of the prefab it was just saved as.
                        if ( !root.HasComponent<ECS::PrefabComponent>() )
                            root.AddComponent<ECS::PrefabComponent>();
                        root.GetComponent<ECS::PrefabComponent>().Prefab = prefabAsset->GetMetadata().Handle;

                        LOG_INFO( "[Prefab] Saved '{}' -> {}", root.GetComponent<ECS::TagComponent>().Tag,
                                  m_SavePrefabPath );
                        s_error.clear();
                        ImGui::CloseCurrentPopup();
                    }
                }
            }
        }

        ImGui::SameLine();
        if ( ImGui::Button( "Cancel", ImVec2( 90, 0 ) ) )
        {
            s_error.clear();
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }

    void SceneHierarchyPanel::DrawInstantiatePrefabPopup()
    {
        if ( ImGui::BeginPopupModal( "InstantiatePrefabPopup", nullptr, ImGuiWindowFlags_AlwaysAutoResize ) )
        {
            ImGui::TextUnformatted( "Prefab path (.deprefab):" );
            ImGui::SetNextItemWidth( 420.0f );
            Utils::ImGuiUtilities::InputText( m_PrefabInstantiatePath, "##PrefabInstPath" );

            static std::string s_prefabError;
            if ( !s_prefabError.empty() )
                ImGui::TextColored( ImVec4( 1.0f, 0.4f, 0.4f, 1.0f ), "%s", s_prefabError.c_str() );

            ImGui::Spacing();

            if ( ImGui::Button( "Instantiate", ImVec2( 130, 0 ) ) && !m_PrefabInstantiatePath.empty() )
            {
                namespace fs = std::filesystem;
                std::error_code ec;
                const bool      exists = fs::exists( m_PrefabInstantiatePath, ec ) &&
                                    fs::is_regular_file( m_PrefabInstantiatePath, ec );
                const bool rightExt = fs::path( m_PrefabInstantiatePath ).extension() ==
                                      Common::Constants::Extensions::PREFAB_EXTENSION;

                if ( !exists )
                {
                    s_prefabError = "File not found (point to an existing .deprefab file, not a folder).";
                    LOG_ERROR( "Could not load prefab, file not found: {0}", m_PrefabInstantiatePath );
                }
                else if ( !rightExt )
                {
                    s_prefabError = "Not a .deprefab file.";
                    LOG_ERROR( "Could not load prefab, wrong extension: {0}", m_PrefabInstantiatePath );
                }
                else if ( m_AssetManager )
                {
                    s_prefabError.clear();
                    auto prefabAsset = m_AssetManager->FindByPath<Assets::PrefabAsset>( m_PrefabInstantiatePath );
                    if ( !prefabAsset )
                    {
                        prefabAsset = const_cast<Assets::AssetManager&>( *m_AssetManager )
                                           .CreateAsset<Assets::PrefabAsset>( Assets::AssetPriority::High,
                                                                              m_PrefabInstantiatePath );
                    }
                    if ( prefabAsset )
                    {
                        if ( !prefabAsset->IsReadyForUse() )
                            prefabAsset->Load();

                        ECS::Entity parentEntity;
                        if ( m_InstantiatePrefabParent.has_value() )
                        {
                            if ( auto ref = m_Scene->FindEntityByID( *m_InstantiatePrefabParent ) )
                            {
                                parentEntity = ref->get();
                            }
                        }

                        const auto placed =
                             prefabAsset->Instantiate( m_Scene.get(), *m_AssetManager, parentEntity, nullptr );
                        if ( !placed )
                        {
                            // THE POPUP STAYS OPEN ON A REFUSAL, and says why. Closing it would leave the
                            // author looking at an outliner that did not change, which is precisely the
                            // report "Instantiate Prefab does nothing" that this whole path has produced
                            // before.
                            s_prefabError = placed.GetError();
                            LOG_ERROR( "{}", placed.GetError() );
                        }
                        else
                        {
                            if ( const ECS::Entity root = placed.GetValue() )
                            {
                                Commands::NotifyCreated( { root.GetComponent<ECS::UUIDComponent>().UUID } );
                            }
                            ImGui::CloseCurrentPopup();
                        }
                    }
                    else
                    {
                        s_prefabError = "Could not load prefab.";
                        LOG_ERROR( "Could not load prefab: {0}", m_PrefabInstantiatePath );
                    }
                }
            }

            ImGui::SameLine();
            if ( ImGui::Button( "Cancel", ImVec2( 90, 0 ) ) )
                ImGui::CloseCurrentPopup();

            ImGui::EndPopup();
        }
    }

} // namespace Desert::Editor
