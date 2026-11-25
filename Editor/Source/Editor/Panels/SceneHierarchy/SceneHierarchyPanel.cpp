#include "SceneHierarchyPanel.hpp"
#include <Editor/Core/Commands/SceneCommands.hpp>
#include <Common/Core/Logger.hpp>

#include <Editor/Panels/WorldPartition/WorldPartitionPanel.hpp>

#include <Engine/Core/Serialize/ComponentRegistry.hpp>
#include <Engine/Core/Serialize/EntityPackages.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <string_view>
#include <Engine/ECS/EditorHLODComponent.hpp>
#include <Engine/ECS/TransientComponent.hpp>
#include <Editor/Core/DragPayloads.hpp>
#include <Editor/Panels/ViewportPanel/CameraPilot.hpp>
#include <Editor/Panels/PropertyEditor/ComponentWidgetRegistry.hpp>
#include <Engine/ECS/Entity.hpp>
#include <Engine/ECS/EntityLock.hpp>
#include <Engine/ECS/FoliageFieldEntities.hpp>
#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Assets/Prefab/PrefabAsset.hpp>
#include <Engine/Geometry/ProceduralCharacterFactory.hpp>

#include <algorithm>
#include <cstdio>
#include <Editor/Core/Selection/SelectionManager.hpp>
#include <Editor/Core/Commands/SceneCommands.hpp>
#include <Editor/Core/EditorResources.hpp>
#include <Editor/Core/ThemeManager.hpp>
#include <Editor/Core/ImGuiUtilities.hpp>
#include <Editor/Core/IconsMaterialDesignIcons.hpp>

#include <ImGui/imgui_internal.h>

#include <Common/Core/Constants.hpp>

#include <filesystem>
#include <functional>
#include <cstring>
#include <Engine/ECS/AnimationComponent.hpp>
#include <Engine/ECS/CameraComponent.hpp>
#include <Engine/ECS/DirectionLightComponent.hpp>
#include <Engine/ECS/FolderComponent.hpp>
#include <Engine/ECS/PointLightComponent.hpp>
#include <Engine/ECS/PrefabComponent.hpp>
#include <Engine/ECS/RectLightComponent.hpp>
#include <Engine/ECS/RelationshipComponent.hpp>
#include <Engine/ECS/SkinnedMeshComponent.hpp>
#include <Engine/ECS/SkyAtmosphereComponent.hpp>
#include <Engine/ECS/SkyboxComponent.hpp>
#include <Engine/ECS/SpotLightComponent.hpp>
#include <Engine/ECS/StaticMeshComponent.hpp>
#include <Engine/ECS/TagComponent.hpp>
#include <Engine/ECS/TextComponent.hpp>
#include <Engine/ECS/TransformComponent.hpp>
#include <Engine/ECS/UUIDComponent.hpp>
#include <Engine/ECS/VisibilityComponent.hpp>
#include <Engine/Scripting/ScriptComponent.hpp>
#include <Landscape/Public/LandscapeComponent.hpp>
#include <Engine/World/Partition/DataLayersComponent.hpp>
#include <Common/Core/Serialization/GlmReflection.hpp>

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
            Model3D,
            Skybox,
            DirLight,
            PointLight,
            SpotLight,
            RectLight,
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
             { "Rendering", ICON_MDI_CUBE_OUTLINE, "3D Model", Archetype::Model3D },
             { "Rendering", ICON_MDI_EARTH, "Skybox", Archetype::Skybox },
             { "Lighting", ICON_MDI_LIGHTBULB, "Directional Light", Archetype::DirLight },
             { "Lighting", ICON_MDI_LIGHTBULB, "Point Light", Archetype::PointLight },
             { "Lighting", ICON_MDI_SPOTLIGHT, "Spot Light", Archetype::SpotLight },
             { "Lighting", ICON_MDI_CEILING_LIGHT, "Rect Light", Archetype::RectLight },
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
                case Archetype::Model3D:
                {
                    auto e = scene.CreateNewEntity( "3D Model" );
                    // An empty mesh slot the user fills in Details: the default value, handle 0.
                    ECS::SetStaticMesh( *e.GetRegistry(), e.GetHandle(), ECS::StaticMeshDesc{} );
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
                case Archetype::DirLight:
                {
                    auto e = scene.CreateNewEntity( "Directional Light" );
                    e.AddComponent<ECS::DirectionLightComponent>();
                    Track( e );
                    break;
                }
                case Archetype::PointLight:
                    (void)SceneHierarchyPanel::SpawnLocalLight( scene, SceneHierarchyPanel::LocalLightKind::Point );
                    break;
                case Archetype::SpotLight:
                    (void)SceneHierarchyPanel::SpawnLocalLight( scene, SceneHierarchyPanel::LocalLightKind::Spot );
                    break;
                case Archetype::RectLight:
                    (void)SceneHierarchyPanel::SpawnLocalLight( scene, SceneHierarchyPanel::LocalLightKind::Rect );
                    break;
                case Archetype::SkinnedModel:
                {
                    auto e = scene.CreateNewEntity( "Skinned Model" );
                    e.AddComponent<ECS::SkinnedMeshComponent>();
                    e.AddComponent<ECS::AnimationComponent>();
                    Track( e );
                    break;
                }
                case Archetype::Character:
                    (void)SceneHierarchyPanel::SpawnProceduralHumanoid( scene );
                    break;
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

                        ECS::LocalTransform tf = e.GetComponent<ECS::TransformComponent>().GetLocal();
                        tf.Translation         = eye + fwd * 4.0f;
                        tf.Rotation = glm::eulerAngles( glm::quatLookAt( fwd, glm::vec3( 0.0f, 1.0f, 0.0f ) ) );
                        e.SetLocalTransform( tf );
                    }
                    Track( e );
                    break;
                }
            }
        }
    } // namespace

    Common::UUID SceneHierarchyPanel::SpawnPrimitive( Desert::Core::Scene& scene, Geometry::PrimitiveType type )
    {
        // Primitive path: the shared mesh comes from PrimitiveMeshFactory, i.e. from ShapeGenerators.
        auto e = scene.CreateNewEntity( Geometry::PrimitiveTypeName( type ) );
        ECS::StaticMeshDesc mesh;
        mesh.Primitive = type;
        ECS::SetStaticMesh( *e.GetRegistry(), e.GetHandle(), mesh );
        Track( e );
        return e.GetComponent<ECS::UUIDComponent>().UUID;
    }

    Common::UUID SceneHierarchyPanel::SpawnLocalLight( Desert::Core::Scene& scene, LocalLightKind kind )
    {
        switch ( kind )
        {
            case LocalLightKind::Point:
            {
                auto e = scene.CreateNewEntity( "Point Light" );
                e.AddComponent<ECS::PointLightComponent>();
                Track( e );
                return e.GetComponent<ECS::UUIDComponent>().UUID;
            }
            case LocalLightKind::Spot:
            {
                auto e = scene.CreateNewEntity( "Spot Light" );
                e.AddComponent<ECS::SpotLightComponent>();
                Track( e );
                return e.GetComponent<ECS::UUIDComponent>().UUID;
            }
            case LocalLightKind::Rect:
                break;
        }
        // UE ARectLight: the component's defaults (a 64 x 64 cm rect facing the entity's -Z).
        auto e = scene.CreateNewEntity( "Rect Light" );
        e.AddComponent<ECS::RectLightComponent>();
        Track( e );
        return e.GetComponent<ECS::UUIDComponent>().UUID;
    }

    Common::UUID SceneHierarchyPanel::SpawnProceduralHumanoid( Desert::Core::Scene& scene )
    {
        // The engine's mannequin (UE's /Engine character): an ordinary entity REFERENCING engine content -
        // Humanoid.skmesh by its GUID, and the default locomotion clip by name (the clip plays on the mesh by the
        // skeleton GUID both state). A Play snapshot, a save and a load carry it like any imported character.
        auto                 e = scene.CreateNewEntity( "Character" );
        ECS::SkinnedMeshDesc mesh;
        mesh.MeshHandle = Geometry::HumanoidMeshHandle();
        ECS::SetSkinnedMesh( *e.GetRegistry(), e.GetHandle(), mesh );
        e.AddComponent<ECS::AnimationComponent>().CurrentClip = std::string( Geometry::kHumanoidDefaultClip );
        Track( e );
        return e.GetComponent<ECS::UUIDComponent>().UUID;
    }

    namespace
    {
        // The Data Layers column's width (World Partition mode), in ems: two short layer names.
        constexpr float kOutlinerLayersEm = 7.0f;

        // THE OUTLINER'S TYPE PRECEDENCE, one table for a loaded entity and for a record left on disk (whose type
        // is read off the component keys its descriptor states). Order is precedence, not taste: an entity
        // carrying both a SkyAtmosphere and a Skybox is a procedural sky that keeps an HDR fallback, and the
        // atmosphere is what it is named for (the same precedence ScenePropertiesPanel::GetPrimaryComponentName
        // uses). The landscape ROOT (the actor that owns the tiles) reads as one environment thing; its tiles are
        // children with their own component and fall through to Actor like any other part of an actor. The keys
        // are the ComponentRegistry's, and an entity is asked through the registered component's Has.
        struct TypeRule
        {
            std::string_view Key;
            EntityTypeKind   Kind;
        };
        constexpr TypeRule kTypePrecedence[] = {
             { "Folder", EntityTypeKind::Folder },
             { "Camera", EntityTypeKind::Camera },
             { "DirectionLight", EntityTypeKind::DirectionalLight },
             { "PointLight", EntityTypeKind::PointLight },
             { "SpotLight", EntityTypeKind::SpotLight },
             { "RectLight", EntityTypeKind::RectLight },
             { "SkyAtmosphere", EntityTypeKind::SkyAtmosphere },
             { "Skybox", EntityTypeKind::Skybox },
             { "Landscape", EntityTypeKind::Landscape },
             { "SkinnedMesh", EntityTypeKind::SkinnedMesh },
             { "StaticMesh", EntityTypeKind::StaticMesh },
             { "Text", EntityTypeKind::Text },
        };
        constexpr std::size_t kTypeRules = std::size( kTypePrecedence );

        // Each rule's registered component, resolved once. A key the registry does not hold is a broken table:
        // named in the log, and that rule never matches.
        const std::array<const ::Desert::Core::Serialize::ComponentSerializer*, kTypeRules>& TypeRuleComponents()
        {
            static const std::array<const ::Desert::Core::Serialize::ComponentSerializer*, kTypeRules> resolved =
                 []
            {
                std::array<const ::Desert::Core::Serialize::ComponentSerializer*, kTypeRules> found{};
                const auto& all = ::Desert::Core::Serialize::ComponentRegistry::Get().All();
                for ( std::size_t rule = 0; rule < kTypeRules; ++rule )
                {
                    const auto match =
                         std::find_if( all.begin(), all.end(),
                                       [&]( const ::Desert::Core::Serialize::ComponentSerializer& serializer )
                                       { return serializer.Key == kTypePrecedence[rule].Key; } );
                    if ( match == all.end() )
                    {
                        LOG_ERROR( "[Outliner] type rule '{}' names no registered component",
                                   kTypePrecedence[rule].Key );
                    }
                    else
                    {
                        found[rule] = &*match;
                    }
                }
                return found;
            }();
            return resolved;
        }

        EntityTypeKind ClassifyComponentKeys( const std::vector<std::string>& keys )
        {
            for ( const TypeRule& rule : kTypePrecedence )
                if ( std::find( keys.begin(), keys.end(), rule.Key ) != keys.end() )
                    return rule.Kind;
            return EntityTypeKind::Actor;
        }

        std::string JoinNames( const std::vector<std::string>& names )
        {
            std::string joined;
            for ( const std::string& name : names )
                joined += joined.empty() ? name : std::format( ", {}", name );
            return joined;
        }
    } // namespace

    EntityTypeKind SceneHierarchyPanel::ClassifyEntity( const ECS::Entity& entity )
    {
        const auto& components = TypeRuleComponents();
        for ( std::size_t rule = 0; rule < kTypeRules; ++rule )
            if ( components[rule] != nullptr && components[rule]->Has( entity ) )
                return kTypePrecedence[rule].Kind;
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
        if ( m_WorldMode && m_UnsavedOnly && !m_UnsavedShown.contains( static_cast<std::uint64_t>( UUID ) ) )
            return;

        Utils::ImGuiUtilities::PushID();

        // A foliage field's children are the prefab instances it realizes (derived state): the field row
        // is listed, they are not (UE's bHideFromSceneOutliner on the actors foliage spawns).
        const bool hasChildren = entity.HasComponent<ECS::RelationshipComponent>() &&
                                 !entity.GetComponent<ECS::RelationshipComponent>().Children.empty() &&
                                 !ECS::HidesChildrenFromOutliner( *entity.GetRegistry(), entity.GetHandle() );

        m_VisibleOrder.push_back( UUID ); // visible draw order (Shift+click range source)

        const bool isSelected = Core::SelectionManager::IsSelected( UUID );

        // A row outside the outliner's clip rect (CPU-TAILS G9; UE: STreeView generates only the rows in view)
        // still takes its height, its place in the Shift+click order and its open state - so its children
        // keep their rows and their ids - but submits no widget and asks none of the drawn row's component
        // questions. The row being renamed and the row being scrolled to always draw: both need their widget
        // alive this frame. The height is the drawn row's: a FramePadding tree node plus the cell padding.
        const void* nodeId    = (void*)(uint64_t)entity.GetHandle();
        const float rowHeight = ImGui::GetFrameHeight() + 2.0f * ImGui::GetStyle().CellPadding.y;
        const bool  pinned    = ( m_RenamingEntity.has_value() && *m_RenamingEntity == UUID ) ||
                            ( isSelected && m_ScrollToSelection );
        ImGui::TableNextRow( ImGuiTableRowFlags_None, rowHeight );
        ImGui::TableSetColumnIndex( 0 );
        if ( !pinned && !ImGui::IsRectVisible( ImVec2( 1.0f, ImGui::GetFrameHeight() ) ) )
        {
            // The same storage slot TreeNodeEx reads (window state storage, keyed by the node's id), and the
            // same reveal-on-select write SetNextItemOpen would make on the drawn path.
            const ImGuiID nodeKey = ImGui::GetID( nodeId );
            ImGuiStorage* storage = ImGui::GetStateStorage();
            if ( m_ExpandToSelection.erase( static_cast<uint64_t>( UUID ) ) > 0 )
                storage->SetInt( nodeKey, 1 );
            if ( hasChildren && storage->GetInt( nodeKey, 0 ) != 0 )
            {
                ImGui::TreePush( nodeId );
                auto& registry = *entity.GetRegistry();
                for ( auto childHandle : entity.GetComponent<ECS::RelationshipComponent>().Children )
                {
                    ECS::Entity child( childHandle, registry );
                    DrawEntityNode( child );
                }
                ImGui::TreePop();
            }
            Utils::ImGuiUtilities::PopID();
            return;
        }

        ImGuiTreeNodeFlags nodeFlags = isSelected ? ImGuiTreeNodeFlags_Selected : 0;
        nodeFlags |= ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_FramePadding |
                     ImGuiTreeNodeFlags_AllowItemOverlap | ImGuiTreeNodeFlags_SpanAvailWidth;
        if ( !hasChildren )
            nodeFlags |= ImGuiTreeNodeFlags_Leaf;

        const bool isPrefab = entity.HasComponent<ECS::PrefabComponent>();

        // Hidden entities (VisibilityComponent.Visible == false) render dimmed with an "eye-off" toggle.
        const bool visible = !entity.HasComponent<ECS::VisibilityComponent>() ||
                             entity.GetComponent<ECS::VisibilityComponent>().IsVisible();

        // Locked entities (LockComponent present) refuse viewport picking and gizmo edits. Asked through
        // the shared predicate rather than HasComponent here, so the outliner's padlock and the two places
        // that ENFORCE it can never be reading different rules.
        const bool locked = ECS::IsLocked( *entity.GetRegistry(), entity.GetHandle() );

        // Outside an open prefab edit (Commands::BeginPrefabEdit): the row is dimmed like a hidden one - the world
        // around the edited instance is inactive, as in UE's level-instance edit. The padlock above stays the
        // entity's OWN lock; the edit's lock is not the user's to toggle.
        const bool outsideEdit = Commands::IsOutsidePrefabEdit( *entity.GetRegistry(), entity.GetHandle() );

        const char* icon = ICON_MDI_CUBE_OUTLINE;
        if ( entity.HasComponent<ECS::FolderComponent>() )
            icon = ICON_MDI_FOLDER;
        else if ( entity.HasComponent<ECS::CameraComponent>() )
            icon = ICON_MDI_CAMERA;
        else if ( entity.HasComponent<ECS::SpotLightComponent>() )
            icon = ICON_MDI_SPOTLIGHT;
        else if ( entity.HasComponent<ECS::RectLightComponent>() )
            icon = ICON_MDI_CEILING_LIGHT;
        else if ( entity.HasComponent<ECS::DirectionLightComponent>() ||
                  entity.HasComponent<ECS::PointLightComponent>() )
            icon = ICON_MDI_LIGHTBULB;
        else if ( entity.HasComponent<ECS::SkyAtmosphereComponent>() )
            icon = ICON_MDI_WEATHER_SUNSET;
        else if ( entity.HasComponent<ECS::SkyboxComponent>() )
            icon = ICON_MDI_EARTH;
        else if ( entity.HasComponent<ECS::LandscapeComponent>() )
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

        // Column 0: icon tree node + name (the row itself was begun above, before the off-screen test).

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
        bool nodeOpen = ImGui::TreeNodeEx( nodeId, nodeFlags, "%s", icon );
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
            if ( !visible || outsideEdit )
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
        // UE's unsaved marker on an actor whose own package is dirty (One File Per Actor): this entity differs
        // from its file (EntityPackages::IsDirty - the same question a save and an unload ask).
        if ( m_WorldMode && m_Scene->Packages()->IsDirty( UUID ) )
        {
            ImGui::SameLine( 0.0f, 2.0f );
            ImGui::TextColored( ImVec4( 0.95f, 0.75f, 0.30f, 1.0f ), "*" );
            if ( ImGui::IsItemHovered() )
                ImGui::SetTooltip( "Unsaved: differs from its file" );
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
        // The row's popup id is its UUID in a stack buffer: a drawn row formats no heap string.
        char popupId[24];
        std::snprintf( popupId, sizeof( popupId ), "%llu",
                       static_cast<unsigned long long>( static_cast<uint64_t>( UUID ) ) );
        if ( ImGui::BeginPopupContextItem( popupId ) )
        {
            if ( ImGui::Selectable( "Rename" ) )
            {
                m_RenamingEntity     = UUID;
                m_RenameBuffer       = name;
                m_RenameFocusPending = true;
            }
            // Pilot / Eject, as UE's outliner offers it on a camera actor (CameraPilot.hpp).
            if ( entity.HasComponent<ECS::CameraComponent>() )
            {
                if ( IsPiloted( UUID ) )
                {
                    if ( ImGui::Selectable( ICON_MDI_EJECT " Eject" ) )
                        if ( const auto ejected = EjectPilot(); !ejected )
                            LOG_WARN( "[Outliner] eject refused: {}", ejected.GetError() );
                }
                else if ( ImGui::Selectable( ( ICON_MDI_EYE " Pilot '" + name + "'" ).c_str() ) )
                {
                    if ( const auto piloted = PilotCameraEntity( UUID ); !piloted )
                        LOG_WARN( "[Outliner] pilot of '{}' refused: {}", name, piloted.GetError() );
                }
            }
            // Pin / Unpin, as UE's outliner offers it on an actor of a World Partition world (the Pinned
            // column's toggle, for a record of the world's files).
            if ( m_WorldMode && m_WorldPartition != nullptr &&
                 m_WorldRecords.contains( static_cast<std::uint64_t>( UUID ) ) )
            {
                const bool pinned = m_WorldPartition->IsPinned( static_cast<std::uint64_t>( UUID ) );
                if ( ImGui::Selectable( pinned ? ICON_MDI_PIN_OUTLINE " Unpin" : ICON_MDI_PIN " Pin" ) )
                    m_PendingPin = std::make_pair( static_cast<std::uint64_t>( UUID ), !pinned );
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
            // whole selection is locked in one go. Both spellings call the same recorded edit
            // (Commands::SetLocked).
            if ( ImGui::Selectable( locked ? ICON_MDI_LOCK_OPEN_OUTLINE " Unlock"
                                           : ICON_MDI_LOCK " Lock (no picking, no gizmo)" ) )
            {
                if ( Core::SelectionManager::Count() > 1 && Core::SelectionManager::IsSelected( UUID ) )
                    Commands::SetLocked( Core::SelectionManager::GetSelection(), !locked );
                else
                    Commands::SetLocked( { UUID }, !locked );
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
                // Off the project's own Prefabs directory (the census), never a working-directory spelling.
                m_SavePrefabPath = ( Common::Constants::Path::PREFAB_PATH /
                                     ( stem + std::string( Common::Constants::Extensions::PREFAB_EXTENSION ) ) )
                                        .generic_string();
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

            if ( ImGui::Selectable( ICON_MDI_PACKAGE_VARIANT " Replace with Prefab..." ) )
            {
                // UE's "Replace Selected Actors with": the whole selection when this row is in it, else this row.
                m_ReplacePrefabTargets = Core::SelectionManager::IsSelected( UUID )
                                              ? Core::SelectionManager::GetSelection()
                                              : std::vector<Common::UUID>{ UUID };
                m_OpenReplacePrefab    = true; // deferred: OpenPopup at panel scope (see OnUIRender)
            }

            // UE's Create Level Instance: the selection (when this row is in it, else this row) becomes one new
            // prefab and one instance of it. Deferred like the replace above; this row is the Selected Entity
            // pivot.
            if ( ImGui::BeginMenu( ICON_MDI_PACKAGE_VARIANT " Create Prefab from Selection" ) )
            {
                for ( const Commands::PrefabPivot pivot :
                      { Commands::PrefabPivot::Center, Commands::PrefabPivot::CenterMin,
                        Commands::PrefabPivot::Entity, Commands::PrefabPivot::WorldOrigin } )
                    if ( ImGui::MenuItem(
                              std::format( "Pivot: {}", Commands::PrefabPivotLabel( pivot ) ).c_str() ) )
                        m_PendingPrefabFromSelection = std::make_pair( pivot, UUID );
                ImGui::EndMenu();
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
                if ( ImGui::Selectable( ICON_MDI_PACKAGE_VARIANT_CLOSED " Break Prefab (one level)" ) )
                {
                    m_PendingPrefabUnpack = UUID; // deferred: Commands::BreakPrefabInstance after iteration
                    m_PendingPrefabUnpackAllLevels = false;
                }
                if ( ImGui::Selectable( ICON_MDI_PACKAGE_VARIANT_CLOSED " Break Prefab (all levels)" ) )
                {
                    m_PendingPrefabUnpack          = UUID;
                    m_PendingPrefabUnpackAllLevels = true;
                }
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
            Commands::SetLocked( { entity.GetComponent<ECS::UUIDComponent>().UUID }, !locked );
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( locked ? "Unlock (allow picking and gizmo edits)"
                                      : "Lock (refuse picking and gizmo edits)" );

        // Columns 4 and 5 (World Partition mode): UE's Pinned gutter, on the world's records only (an entity made
        // since the last save has no file to keep loaded), and the entity's Data Layers.
        if ( m_WorldMode )
        {
            ImGui::TableSetColumnIndex( 4 );
            if ( m_WorldRecords.contains( static_cast<std::uint64_t>( UUID ) ) )
                DrawPinToggle( static_cast<std::uint64_t>( UUID ) );
            ImGui::TableSetColumnIndex( 5 );
            if ( entity.HasComponent<ECS::DataLayersComponent>() )
            {
                ImGui::PushStyleColor( ImGuiCol_Text, visible
                                                           ? ImGui::GetStyleColorVec4( ImGuiCol_Text )
                                                           : ImGui::GetStyleColorVec4( ImGuiCol_TextDisabled ) );
                ImGui::TextUnformatted(
                     JoinNames( entity.GetComponent<ECS::DataLayersComponent>().Data.Layers ).c_str() );
                ImGui::PopStyleColor();
            }
        }

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

    void SceneHierarchyPanel::RefreshWorldRows()
    {
        const auto& packages = *m_Scene->Packages();
        // Edit only: Play runs the whole world in this scene, so its records are all loaded there and its edits
        // are not the files' (UE's outliner shows the PIE world without the unloaded actors).
        m_WorldMode = m_Scene->GetWorldPartition().has_value() && packages.BaselinePath().has_value() &&
                      !( m_WorldPartition != nullptr && m_WorldPartition->Playing() );
        if ( !m_WorldMode )
        {
            m_WorldRowsValid = false;
            m_WorldRows.reset();
            m_WorldRowsStatus.clear();
            m_WorldRecords.clear();
            m_NotLoadedChildren.clear();
            m_NotLoadedRoots.clear();
            return;
        }
        // The records left on disk change only with a region change (AdoptRegion) or a new open (Forget).
        if ( m_WorldRowsValid && m_WorldRowsGeneration == packages.RegionGeneration() )
            return;
        m_WorldRowsValid      = true;
        m_WorldRowsGeneration = packages.RegionGeneration();
        m_WorldRecords.clear();
        m_NotLoadedChildren.clear();
        m_NotLoadedRoots.clear();
        auto described = ::Desert::Core::EditorRegions::DescribeWorld( *m_Scene );
        if ( !described )
        {
            m_WorldRows.reset();
            m_WorldRowsStatus = described.GetError();
            return;
        }
        m_WorldRowsStatus.clear();
        m_WorldRows = described.ExtractValue();
        m_WorldRecords.insert( m_WorldRows->Records.begin(), m_WorldRows->Records.end() );
        // A record left on disk hangs under its parent when the parent is left on disk too (a composite leaves
        // whole); otherwise it is a root row.
        std::unordered_set<std::uint64_t> notLoaded;
        for ( const ::Desert::Core::EditorRegions::NotLoadedRow& row : m_WorldRows->NotLoaded )
            notLoaded.insert( row.Id );
        for ( std::size_t r = 0; r < m_WorldRows->NotLoaded.size(); ++r )
        {
            const std::uint64_t parent = m_WorldRows->NotLoaded[r].Parent;
            if ( parent != 0 && notLoaded.contains( parent ) )
                m_NotLoadedChildren[parent].push_back( r );
            else
                m_NotLoadedRoots.push_back( r );
        }
    }

    void SceneHierarchyPanel::CollectUnsavedRows()
    {
        m_UnsavedShown.clear();
        if ( !m_WorldMode || !m_UnsavedOnly )
            return;
        auto&       registry = m_Scene->GetRegistry();
        const auto& packages = *m_Scene->Packages();
        // A dirty entity and every ancestor of it, so the tree still reaches it (UE's filters keep the parents).
        for ( const auto handle : registry.view<ECS::UUIDComponent>() )
        {
            if ( !packages.IsDirty( registry.get<ECS::UUIDComponent>( handle ).UUID ) )
                continue;
            entt::entity e = handle;
            while ( e != entt::null && registry.has<ECS::UUIDComponent>( e ) )
            {
                if ( !m_UnsavedShown
                           .insert( static_cast<std::uint64_t>( registry.get<ECS::UUIDComponent>( e ).UUID ) )
                           .second )
                    break; // its ancestors are in already
                if ( !registry.has<ECS::RelationshipComponent>( e ) )
                    break;
                e = registry.get<ECS::RelationshipComponent>( e ).Parent;
            }
        }
    }

    void SceneHierarchyPanel::DrawPinToggle( const std::uint64_t record )
    {
        if ( m_WorldPartition == nullptr )
            return;
        const bool pinned = m_WorldPartition->IsPinned( record );
        ImGui::PushStyleColor( ImGuiCol_Text, pinned ? ThemeManager::GetIconColor()
                                                     : ImGui::GetStyleColorVec4( ImGuiCol_TextDisabled ) );
        ImGui::TextUnformatted( pinned ? ICON_MDI_PIN : ICON_MDI_PIN_OUTLINE );
        ImGui::PopStyleColor();
        if ( ImGui::IsItemClicked() )
            m_PendingPin = std::make_pair( record, !pinned );
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( pinned ? "Unpin (left to the loaded regions again)"
                                      : "Pin (kept loaded whatever the regions)" );
    }

    void SceneHierarchyPanel::DrawNotLoadedRow( const std::size_t index )
    {
        const ::Desert::Core::EditorRegions::NotLoadedRow& row = m_WorldRows->NotLoaded[index];
        if ( m_HierarchyFilter.IsActive() && !m_HierarchyFilter.PassFilter( row.Name.c_str() ) )
            return;
        const auto  children    = m_NotLoadedChildren.find( row.Id );
        const bool  hasChildren = children != m_NotLoadedChildren.end();
        const void* nodeId      = reinterpret_cast<const void*>( static_cast<std::uintptr_t>( row.Id ) );
        const float rowHeight   = ImGui::GetFrameHeight() + 2.0f * ImGui::GetStyle().CellPadding.y;
        ImGui::TableNextRow( ImGuiTableRowFlags_None, rowHeight );
        ImGui::TableSetColumnIndex( 0 );
        // Outside the clip rect: the row's height and its open children only (as DrawEntityNode).
        if ( !ImGui::IsRectVisible( ImVec2( 1.0f, ImGui::GetFrameHeight() ) ) )
        {
            if ( hasChildren && ImGui::GetStateStorage()->GetInt( ImGui::GetID( nodeId ), 0 ) != 0 )
            {
                ImGui::TreePush( nodeId );
                for ( const std::size_t child : children->second )
                    DrawNotLoadedRow( child );
                ImGui::TreePop();
            }
            return;
        }
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_FramePadding |
                                   ImGuiTreeNodeFlags_AllowItemOverlap | ImGuiTreeNodeFlags_SpanAvailWidth;
        if ( !hasChildren )
            flags |= ImGuiTreeNodeFlags_Leaf;
        // Grey: UE draws an unloaded actor's row in the subdued text colour.
        const ImVec4 grey = ImGui::GetStyleColorVec4( ImGuiCol_TextDisabled );
        ImGui::PushStyleColor( ImGuiCol_Text, grey );
        const bool open = ImGui::TreeNodeEx( nodeId, flags, "%s", ICON_MDI_CUBE_OUTLINE );
        bool       load = ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked( ImGuiMouseButton_Left );
        ImGui::SameLine();
        ImGui::TextUnformatted( row.Name.c_str() );
        load = load || ( ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked( ImGuiMouseButton_Left ) );
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "Not loaded (cell %s). Double-click to load it: it is pinned.", row.Cell.c_str() );
        ImGui::SameLine();
        ImGui::TextUnformatted( std::format( "[{}]", row.Cell ).c_str() );
        ImGui::TableSetColumnIndex( 1 );
        ImGui::TextUnformatted( EntityTypeOf( ClassifyComponentKeys( row.Components ) ).Name );
        ImGui::TableSetColumnIndex( 5 );
        ImGui::TextUnformatted( JoinNames( row.DataLayers ).c_str() );
        ImGui::PopStyleColor();
        ImGui::TableSetColumnIndex( 4 );
        DrawPinToggle( row.Id );
        // UE loads an unloaded actor by pinning it (the pinned actors loader adapter).
        if ( load )
            m_PendingPin = std::make_pair( row.Id, true );

        if ( open )
        {
            if ( hasChildren )
                for ( const std::size_t child : children->second )
                    DrawNotLoadedRow( child );
            ImGui::TreePop();
        }
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
                for ( const Geometry::PrimitiveType type : Geometry::kAuthorablePrimitives )
                    if ( m_AddEntityFilter.PassFilter( Geometry::PrimitiveTypeName( type ) ) &&
                         ImGui::Selectable(
                              ( std::string( ICON_MDI_SHAPE ) + "  " + Geometry::PrimitiveTypeName( type ) )
                                   .c_str() ) )
                        SpawnPrimitive( *scene, type );
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

            // Shapes: every authorable primitive, listed from the one array the Details combo reads too.
            if ( ImGui::BeginMenu( ICON_MDI_SHAPE "  Shapes" ) )
            {
                for ( const Geometry::PrimitiveType type : Geometry::kAuthorablePrimitives )
                    if ( ImGui::Selectable(
                              ( std::string( ICON_MDI_SHAPE ) + "  " + Geometry::PrimitiveTypeName( type ) )
                                   .c_str() ) )
                        SpawnPrimitive( *scene, type );
                ImGui::EndMenu();
            }

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

        if ( m_OpenReplacePrefab )
        {
            ImGui::OpenPopup( "ReplacePrefabPopup" );
            m_OpenReplacePrefab = false;
        }
        DrawReplacePrefabPopup();

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

        RefreshWorldRows();
        CollectUnsavedRows();

        // Entity / selection counters (compact, above the table).
        {
            // The editor HLODs (WP-EDITOR-HLOD) and the transient entities are not the world's: counted no more
            // than they are listed.
            const size_t total = m_Scene->GetAllEntities().size() -
                                 m_Scene->GetRegistry().view<ECS::EditorHLODComponent>().size() -
                                 m_Scene->GetRegistry().view<ECS::TransientComponent>().size();
            const size_t sel   = Core::SelectionManager::Count();
            if ( sel > 0 )
                ImGui::TextDisabled( "%zu entities, %zu selected", total, sel );
            else
                ImGui::TextDisabled( "%zu entities", total );
            // World Partition mode: UE's Unsaved filter and its unloaded-actors toggle, and how much is on disk.
            if ( m_WorldMode )
            {
                ImGui::SameLine();
                ImGui::Checkbox( "Unsaved", &m_UnsavedOnly );
                if ( ImGui::IsItemHovered() )
                    ImGui::SetTooltip( "Only the entities that differ from their files (and their parents)" );
                ImGui::SameLine();
                ImGui::Checkbox( "Unloaded", &m_ShowUnloaded );
                if ( ImGui::IsItemHovered() )
                    ImGui::SetTooltip(
                         "List the world's entities that are not loaded (grey; double-click loads)" );
                if ( m_WorldRows.has_value() )
                {
                    ImGui::SameLine();
                    ImGui::TextDisabled( "%zu not loaded", m_WorldRows->NotLoaded.size() );
                }
                if ( !m_WorldRowsStatus.empty() )
                    ImGui::TextColored( ImVec4( 1.0f, 0.45f, 0.4f, 1.0f ), "%s", m_WorldRowsStatus.c_str() );
            }
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
            // UE's Outliner: Name FILLS and keeps a minimum, Type and the gutters are fixed and not resizable
            // (OutlinerColumns). A resizable Type column kept whatever width it was dragged or saved at, and a
            // fixed column that is not resizable is the only kind ImGui re-sizes every frame from the width
            // given here — so the split is recomputed per frame and Name never collapses to one character.
            constexpr ImGuiTableFlags tableFlags =
                 ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp;

            // A toggle gutter: one glyph plus the cell padding it is drawn inside. Measured on the WIDEST
            // glyph either gutter can show, so neither clips when its icon changes with the row's state.
            const float gutterWidth =
                 std::max( { ImGui::CalcTextSize( ICON_MDI_EYE_OUTLINE ).x, ImGui::CalcTextSize( ICON_MDI_LOCK ).x,
                             ImGui::CalcTextSize( ICON_MDI_LOCK_OPEN_OUTLINE ).x } ) +
                 8.0f;
            const float typeWidth =
                 TypeColumnWidth( []( const char* s ) { return ImGui::CalcTextSize( s ).x; }, 12.0f );

            constexpr float kCellPadX = 4.0f;
            // Name, Type, the eye and padlock gutters - and in World Partition mode UE's Pinned gutter and Data
            // Layers column. All six are always declared (Disabled outside the mode) so the table's id and its
            // column indices never change with the mode; the chrome counts the columns shown.
            constexpr int kColumns    = 6;
            const int     shown       = m_WorldMode ? 6 : 4;
            const float   layersWidth = m_WorldMode ? kOutlinerLayersEm * ImGui::GetFontSize() : 0.0f;
            const float   gutters     = ( m_WorldMode ? 3.0f : 2.0f ) * gutterWidth;
            const float   chrome =
                 ( static_cast<float>( shown ) * 2.0f * kCellPadX ) + static_cast<float>( shown - 1 );
            const ImGuiTableColumnFlags worldOnly =
                 m_WorldMode ? ImGuiTableColumnFlags_None : ImGuiTableColumnFlags_Disabled;
            const OutlinerColumnWidths split =
                 OutlinerColumns( ImGui::GetContentRegionAvail().x - chrome - gutters, typeWidth, layersWidth,
                                  kOutlinerNameMinEm * ImGui::GetFontSize() );

            ImGui::PushStyleVar( ImGuiStyleVar_CellPadding, ImVec2( kCellPadX, 2.0f ) );
            if ( ImGui::BeginTable( "##outliner", kColumns, tableFlags ) )
            {
                ImGui::TableSetupColumn( "Name", ImGuiTableColumnFlags_WidthStretch );
                // A zero width drops the column for the frame (Disabled), never an ImGui auto-fit to content:
                // a fixed column given no width measures its cells, which is the width Name had no room for.
                ImGui::TableSetupColumn(
                     "Type",
                     ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoResize |
                          ( split.Type > 0.0f ? ImGuiTableColumnFlags_None : ImGuiTableColumnFlags_Disabled ),
                     split.Type );
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
                ImGui::TableSetupColumn(
                     ICON_MDI_PIN "##pinned",
                     ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoResize | worldOnly, gutterWidth );
                ImGui::TableSetupColumn(
                     "Data Layers",
                     ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoResize |
                          ( split.Layers > 0.0f ? ImGuiTableColumnFlags_None : ImGuiTableColumnFlags_Disabled ),
                     split.Layers );
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
                    // An editor HLOD (WP-EDITOR-HLOD) or a transient entity is not the world's: the Outliner does
                    // not list it.
                    if ( !hasParent && !entity.HasComponent<ECS::EditorHLODComponent>() &&
                         !entity.HasComponent<ECS::TransientComponent>() )
                        DrawEntityNode( entity );
                }

                // UE's FActorDescTreeItem rows: the world's records left on disk, grey, after the loaded tree. Not
                // under the Unsaved filter - a record on disk is its file.
                if ( m_WorldMode && m_ShowUnloaded && !m_UnsavedOnly && m_WorldRows.has_value() )
                {
                    ImGui::PushID( "NotLoaded" );
                    for ( const std::size_t root : m_NotLoadedRoots )
                        DrawNotLoadedRow( root );
                    ImGui::PopID();
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
                            prefabAsset = const_cast<Assets::AssetManager&>( *m_AssetManager )
                                               .CreateAsset<Assets::PrefabAsset>( path );
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
        // A pin toggled or a record double-clicked: the editor holds the world again (an unpin that would
        // unload an unsaved entity opens the World Partition prompt instead).
        if ( m_PendingPin.has_value() )
        {
            const auto [record, pin] = *m_PendingPin;
            m_PendingPin.reset();
            if ( m_WorldPartition != nullptr )
                if ( const auto changed = m_WorldPartition->SetPinned( record, pin ); !changed )
                    LOG_WARN( "[Outliner] {} of record {} refused: {}", pin ? "pin" : "unpin", record,
                              changed.GetError() );
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
        if ( m_PendingPrefabFromSelection.has_value() )
        {
            // The whole selection when the clicked row is in it, else that row alone (Replace with Prefab's rule).
            const auto [pivot, clicked] = *m_PendingPrefabFromSelection;
            m_PendingPrefabFromSelection.reset();
            const std::vector<Common::UUID> members = Core::SelectionManager::IsSelected( clicked )
                                                           ? Core::SelectionManager::GetSelection()
                                                           : std::vector<Common::UUID>{ clicked };
            std::string                     stem    = "Prefab";
            if ( auto found = m_Scene->FindEntityByID( clicked );
                 found && found->get().HasComponent<ECS::TagComponent>() )
                stem = found->get().GetComponent<ECS::TagComponent>().Tag;
            if ( const auto created = Commands::CreatePrefabFromSelection(
                      members, Commands::UniquePrefabPath( stem ), pivot, clicked );
                 !created.IsSuccess() )
                LOG_ERROR( "[Prefab] {}", created.GetError() );
        }
        if ( m_PendingPrefabUnpack.has_value() )
        {
            // THE ONE ROUTE (Commands::BreakPrefabInstance): undoable, and it takes the record addresses off as
            // well as the link. The strip that stood here left them, so "plain" entities still claimed prefab
            // records, and a prefab cut from two of them wrote one record id twice.
            const auto broken = Commands::BreakPrefabInstance(
                 *m_PendingPrefabUnpack, m_PendingPrefabUnpackAllLevels ? Commands::BreakDepth::AllLevels
                                                                        : Commands::BreakDepth::OneLevel );
            if ( !broken.IsSuccess() )
                LOG_ERROR( "[Prefab] {}", broken.GetError() );
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

                const ECS::Entity root  = entityRef->get();
                const auto        saved = Assets::PrefabAsset::SaveNewFromEntity( root, *m_AssetManager,
                                                                                  Common::Filepath( m_SavePrefabPath ) );
                {
                    if ( !saved )
                    {
                        // The PrefabComponent below is NOT attached on a failed write. It would make
                        // the live entity declare itself an instance of a file that does not exist —
                        // the hierarchy draws it with a prefab badge, "Revert to Prefab" reverts it to
                        // nothing, and the next scene save persists the dangling reference.
                        s_error = "The prefab file was NOT written: " + saved.GetError();
                    }
                    else
                    {
                        // Mark the live entity as an instance of the prefab it was just saved as.
                        if ( !root.HasComponent<ECS::PrefabComponent>() )
                            root.AddComponent<ECS::PrefabComponent>();
                        root.GetComponent<ECS::PrefabComponent>().Prefab = saved.GetValue()->GetMetadata().Handle;
                        m_Scene->MarkModifiedSubtree( root );

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
                                           .CreateAsset<Assets::PrefabAsset>( m_PrefabInstantiatePath );
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

    void SceneHierarchyPanel::DrawReplacePrefabPopup()
    {
        if ( !ImGui::BeginPopupModal( "ReplacePrefabPopup", nullptr, ImGuiWindowFlags_AlwaysAutoResize ) )
            return;

        ImGui::Text( "Replace %zu entit%s (and their children) with an instance of:",
                     m_ReplacePrefabTargets.size(), m_ReplacePrefabTargets.size() == 1 ? "y" : "ies" );
        ImGui::SetNextItemWidth( 420.0f );
        Utils::ImGuiUtilities::InputText( m_PrefabInstantiatePath, "##PrefabReplacePath" );
        ImGui::Checkbox( "Keep each entity's static mesh", &m_ReplaceKeepMesh );
        ImGui::TextDisabled( "Same parent, transform and name. One undo step." );
        if ( !m_ReplacePrefabError.empty() )
            ImGui::TextColored( ImVec4( 1.0f, 0.4f, 0.4f, 1.0f ), "%s", m_ReplacePrefabError.c_str() );
        ImGui::Spacing();

        if ( ImGui::Button( "Replace", ImVec2( 130, 0 ) ) )
        {
            // The popup stays open on a refusal and says why; nothing in the scene has changed then.
            const auto replaced =
                 Commands::ReplaceWithPrefab( m_ReplacePrefabTargets, m_PrefabInstantiatePath,
                                              m_ReplaceKeepMesh ? Commands::ReplaceMeshSource::KeepEntityMesh
                                                                : Commands::ReplaceMeshSource::Prefab );
            if ( !replaced.IsSuccess() )
            {
                m_ReplacePrefabError = replaced.GetError();
                LOG_ERROR( "{}", m_ReplacePrefabError );
            }
            else
            {
                m_ReplacePrefabError.clear();
                m_ReplacePrefabTargets.clear();
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::SameLine();
        if ( ImGui::Button( "Cancel", ImVec2( 90, 0 ) ) )
        {
            m_ReplacePrefabError.clear();
            m_ReplacePrefabTargets.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

} // namespace Desert::Editor
