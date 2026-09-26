#include <Editor/Core/DetailsNavigation.hpp>
#include "ComponentEditor.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include <Editor/Core/EditorPreferences.hpp>
#include <Editor/Core/IconsMaterialDesignIcons.hpp>
#include <Editor/Core/ImGuiUtilities.hpp>
#include <Editor/Core/Commands/SceneCommands.hpp>
#include <Editor/Panels/PropertyEditor/PropertyEditorBuilder.hpp>
#include <Editor/Widgets/Controls/Controls.hpp>

#include <Engine/ECS/Entity.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/Reflection/ReflectionRegistry.hpp>

namespace Desert::Editor
{
    namespace ImGui = ::ImGui;

    namespace
    {
        bool ContainsCI( std::string_view haystack, std::string_view needle )
        {
            if ( needle.empty() )
                return true;
            const auto it = std::search( haystack.begin(), haystack.end(), needle.begin(), needle.end(),
                                         []( unsigned char a, unsigned char b )
                                         { return std::tolower( a ) == std::tolower( b ); } );
            return it != haystack.end();
        }

        // Header glyph per component, by its registered name. A component missing from this table simply
        // gets the neutral one — the icon is chrome, so a new component never has to touch this list to
        // work; add a row when it deserves a distinct glyph.
        const char* ComponentIcon( const std::string& name )
        {
            struct Row
            {
                const char* Name;
                const char* Icon;
            };
            static const Row kIcons[] = {
                 { "Transform", ICON_MDI_AXIS_ARROW },
                 { "3D Model", ICON_MDI_CUBE_OUTLINE },
                 { "Skinned Mesh", ICON_MDI_HUMAN },
                 { "Animation", ICON_MDI_RUN_FAST },
                 { "Camera", ICON_MDI_CAMERA },
                 { "Directional Light", ICON_MDI_WEATHER_SUNNY },
                 { "Point Light", ICON_MDI_LIGHTBULB },
                 { "Spot Light", ICON_MDI_SPOTLIGHT },
                 { "Skybox", ICON_MDI_EARTH },
                 { "Landscape Material", ICON_MDI_TERRAIN },
                 { "Text", ICON_MDI_FORMAT_TEXT },
                 { "Collider", ICON_MDI_SHAPE_OUTLINE },
                 { "Rigid Body", ICON_MDI_WEIGHT },
                 { "Character Controller", ICON_MDI_WALK },
                 { "Audio Source", ICON_MDI_VOLUME_HIGH },
                 { "Script", ICON_MDI_LANGUAGE_LUA },
                 { "Particle System", ICON_MDI_CREATION },
                 { "Prefab", ICON_MDI_CUBE_SCAN },
                 { "UI Layout", ICON_MDI_VIEW_DASHBOARD },
            };
            for ( const auto& row : kIcons )
                if ( name == row.Name )
                    return row.Icon;
            return ICON_MDI_PUZZLE;
        }

        // Where a component sits in the panel. Components self-register at static init, so without this
        // the order is link order — it changes when a file is added and puts Transform wherever it lands.
        // UE's Details is read top-down in a fixed order: what the object IS (transform, then the thing it
        // renders and its materials), then how it BEHAVES (physics, audio, script). A component missing
        // from the table keeps its registration order after the ranked ones, so a new component never has
        // to be listed here to appear.
        int ComponentRank( const std::string& name )
        {
            static const char* const kOrder[] = {
                 "Transform",
                 "3D Model",
                 "Skinned Mesh",
                 "Animation",
                 "Particle System",
                 "Text",
                 "UI Layout",
                 "Camera",
                 "Skybox",
                 "Landscape Material",
                 "Directional Light",
                 "Point Light",
                 "Spot Light",
                 "Collider",
                 "Rigid Body",
                 "Character Controller",
                 "Locomotion",
                 "Socket",
                 "Projectile",
                 "Foliage Type",
                 "Audio Source",
                 "Script",
                 "Prefab",
            };
            for ( int i = 0; i < static_cast<int>( std::size( kOrder ) ); ++i )
                if ( name == kOrder[i] )
                    return i;
            return static_cast<int>( std::size( kOrder ) );
        }

        // The reflected type behind a component entry, or null for hand-written widgets.
        const Reflection::TypeInfo* ReflectedTypeOf( const ComponentEditorEntry& entry )
        {
            if ( entry.ReflectedTypeName.empty() )
                return nullptr;
            return Reflection::ReflectionRegistry::Get().Find( entry.ReflectedTypeName );
        }
    } // namespace

    ComponentEditor::ComponentEditor( const std::shared_ptr<Assets::AssetManager>& assetManager,
                                      const Animation::AnimationLibrary*           animationLibrary )
         : m_AssetManager( assetManager ), m_AnimationLibrary( animationLibrary )
    {
    }

    ComponentEditContext ComponentEditor::MakeContext() const
    {
        ComponentEditContext ctx;
        ctx.AssetManager     = m_AssetManager;
        ctx.AnimationLibrary = m_AnimationLibrary;
        ctx.UIHelper         = m_ThumbnailUI;
        ctx.Preview          = m_Preview;
        ctx.PreviewUI        = m_ThumbnailUI;
        ctx.PreviewUsed      = m_PreviewUsed;
        return ctx;
    }

    bool ComponentEditor::EntryMatchesFilter( const ComponentEditorEntry& entry, const char* filter )
    {
        if ( !filter || !*filter )
            return true;
        if ( ContainsCI( entry.Name, filter ) )
            return true;
        // A hand-written widget has no field metadata to search — it matches by name only.
        const auto* type = ReflectedTypeOf( entry );
        return type && PropertyEditorBuilder::MatchesFilter( *type, filter );
    }

    void ComponentEditor::DrawPinnedFields( ECS::Entity& entity, const ComponentEditContext& ctx )
    {
        if ( EditorPreferences::Get().FavouriteFields.empty() )
            return;

        // Resolve the pins that exist on THIS entity. A pin naming a component the entity does not have
        // (or a field since renamed) is skipped, never dropped — it is still valid for the next entity.
        struct PinnedRow
        {
            void*                        Object;
            const Reflection::TypeInfo*  Type;
            const Reflection::FieldInfo* Field;
        };
        std::vector<PinnedRow> rows;

        for ( const auto& entry : ComponentWidgetRegistry::Get().Entries() )
        {
            if ( !entry.Has || !entry.Has( entity ) || !entry.DataPtr )
                continue;
            const auto* type = ReflectedTypeOf( entry );
            if ( !type )
                continue;

            for ( const auto& field : type->Fields )
            {
                if ( field.Meta.Hidden )
                    continue;
                if ( EditorPreferences::IsFavouriteField( PropertyEditorBuilder::FieldKey( *type, field ) ) )
                    rows.push_back( { entry.DataPtr( entity ), type, &field } );
            }
        }

        if ( rows.empty() )
            return;

        // "###" keeps the id stable while the count in the label changes.
        const std::string label =
             std::string( ICON_MDI_STAR "  Pinned (" ) + std::to_string( rows.size() ) + ")###PinnedFields";
        if ( !Utils::ImGuiUtilities::SectionHeader( label.c_str() ) )
            return;

        ImGui::PushID( "pinned" );
        ImGui::Indent();
        for ( const auto& row : rows )
            PropertyEditorBuilder::DrawPinnedRow( row.Object, *row.Type, *row.Field, ctx.AssetMgr(),
                                                  ctx.UIHelper );
        ImGui::Unindent();
        ImGui::PopID();
        ImGui::Spacing();
    }

    void ComponentEditor::Render( ECS::Entity& entity, ::Desert::Core::Scene* scene, const char* fieldFilter )
    {
        auto ctx        = MakeContext();
        ctx.FieldFilter = ( fieldFilter && *fieldFilter ) ? fieldFilter : nullptr;

        // Pinned fields are a shortcut through a full panel; while searching, the search IS the shortcut.
        if ( !ctx.FieldFilter )
            DrawPinnedFields( entity, ctx );

        // Ranked, not registration order — see ComponentRank. stable_sort keeps the unranked ones in the
        // order they registered, so the tail is at least deterministic per build.
        std::vector<const ComponentEditorEntry*> shown;
        for ( const auto& entry : ComponentWidgetRegistry::Get().Entries() )
        {
            if ( !entry.Has || !entry.Has( entity ) )
                continue;
            if ( !EntryMatchesFilter( entry, ctx.FieldFilter ) )
                continue;
            shown.push_back( &entry );
        }
        std::stable_sort( shown.begin(), shown.end(),
                          []( const ComponentEditorEntry* a, const ComponentEditorEntry* b )
                          { return ComponentRank( a->Name ) < ComponentRank( b->Name ); } );

        const bool anyShown = !shown.empty();
        for ( const auto* entry : shown )
            RenderComponentHeader( *entry, entity, scene, ctx );

        if ( ctx.FieldFilter && !anyShown )
            ImGui::TextDisabled( "No property matches '%s'", ctx.FieldFilter );

        if ( ImGui::Button( ICON_MDI_PLUS_BOX_OUTLINE " Add Component",
                            ImVec2( ImGui::GetContentRegionAvail().x, 0.0f ) ) )
        {
            ImGui::OpenPopup( "addComponent" );
        }

        RenderAddComponentPopup( entity );
    }

    void ComponentEditor::RenderAddComponentPopup( ECS::Entity& entity )
    {
        // Shared grouped menu (single source of truth in ComponentWidgetRegistry) — the same one the scene
        // outliner reuses, so the categorization is never written in two places.
        if ( ImGui::BeginPopup( "addComponent" ) )
        {
            DrawAddComponentMenu( entity, m_ComponentFilter );
            ImGui::EndPopup();
        }
    }

    void ComponentEditor::RenderComponentHeader( const ComponentEditorEntry& entry, ECS::Entity& entity,
                                                 ::Desert::Core::Scene* scene, const ComponentEditContext& ctx )
    {
        bool       removed   = false;
        const bool filtering = ctx.FieldFilter != nullptr;

        // Expand/collapse survives restarts (EditorPreferences). ImGui owns the live state — seed it once
        // per window, then mirror the user's toggles back. While searching everything opens instead, and
        // that forced state is deliberately NOT written back.
        const bool storedOpen = !EditorPreferences::IsComponentCollapsed( entry.Name );
        // "Show field: <component>" opens the section as a click would (and so is remembered like one).
        GetDetailsNavigation().NoteComponent( entry.Name );
        const bool revealed = GetDetailsNavigation().TakeReveal( entry.Name );
        if ( revealed )
            ImGui::SetScrollHereY( 0.0f );
        ImGui::SetNextItemOpen( filtering || revealed ? true : storedOpen,
                                filtering || revealed ? ImGuiCond_Always : ImGuiCond_Once );

        // "###" fixes the id to the component name, so the icon and summary in front of it can change
        // without resetting the section.
        const std::string label =
             std::string( ComponentIcon( entry.Name ) ) + "  " + entry.Name + "###" + entry.Name;
        // The SHARED section bar (flat grey). A raw CollapsingHeader paints itself with ImGuiCol_Header,
        // which is the SELECTION colour — every component header would read as a selected row.
        const bool open = Utils::ImGuiUtilities::SectionHeader( label.c_str(), true );

        if ( !filtering && open != storedOpen )
            EditorPreferences::SetComponentCollapsed( entry.Name, !open );

        // One line of state beside the header (from the type's PROPERTY(Summary) fields) so a COLLAPSED
        // component still says what it is. Right-aligned, ahead of the remove button.
        if ( const auto* type = ReflectedTypeOf( entry ); type && entry.DataPtr )
        {
            const std::string summary = PropertyEditorBuilder::BuildSummary( entry.DataPtr( entity ), *type );
            if ( !summary.empty() )
            {
                const float textW = ImGui::CalcTextSize( summary.c_str() ).x;
                const float buttonW =
                     entry.CanRemove ? ImGui::GetFontSize() + ImGui::GetStyle().ItemSpacing.x : 0.0f;
                const float x = ImGui::GetContentRegionAvail().x - buttonW - textW - 8.0f;
                // Only when it fits: a squeezed panel keeps the name, not the summary.
                if ( x > ImGui::CalcTextSize( entry.Name.c_str() ).x + ImGui::GetFontSize() * 2.0f )
                {
                    ImGui::SameLine( x );
                    ImGui::TextDisabled( "%s", summary.c_str() );
                }
            }
        }

        if ( entry.CanRemove )
        {
            ImGui::SameLine( ImGui::GetContentRegionAvail().x - ImGui::GetFontSize() -
                             ImGui::GetStyle().ItemSpacing.x );
            ImGui::PushStyleColor( ImGuiCol_Button, ImVec4( 0.7f, 0.7f, 0.7f, 0.0f ) );

            if ( ImGui::Button( ICON_MDI_TUNE "##RemoveButton" ) )
            {
                ImGui::OpenPopup( "RemoveComponentPopup" );
            }

            ImGui::PopStyleColor();

            if ( ImGui::BeginPopup( "RemoveComponentPopup" ) )
            {
                if ( ImGui::Selectable( "Remove" ) )
                {
                    removed = true;
                }
                ImGui::EndPopup();
            }
        }

        if ( removed )
        {
            if ( entry.Remove )
            {
                // Undoable: Ctrl+Z brings the component (with its data) back.
                Commands::MutateEntityUndoable( entity.GetComponent<ECS::UUIDComponent>().UUID,
                                                [&] { entry.Remove( entity ); } );
            }
        }
        else if ( open )
        {
            // Indent every component body uniformly so nested widgets read as children of the header
            // (consistent tabulation across Transform / lights / mesh / skybox / reflected components).
            ImGui::PushID( entry.Name.c_str() );
            ImGui::Indent();
            if ( entry.Draw )
                entry.Draw( entity, scene, ctx );
            ImGui::Unindent();
            ImGui::PopID();
        }
    }

} // namespace Desert::Editor
