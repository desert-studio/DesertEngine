#include "ComponentWidgetRegistry.hpp"

#include <Editor/Widgets/PreviewViewport.hpp>

#include <Editor/Core/IconsMaterialDesignIcons.hpp>
#include <Editor/Core/Commands/SceneCommands.hpp>

#include <Engine/ECS/Entity.hpp>
#include <Engine/ECS/Components.hpp>

#include <ImGui/imgui.h>

#include <cstring>
#include <string>

namespace Desert::Editor
{
    ComponentWidgetRegistry& ComponentWidgetRegistry::Get()
    {
        static ComponentWidgetRegistry instance;
        return instance;
    }

    int ComponentWidgetRegistry::Register( ComponentEditorEntry entry )
    {
        m_Entries.push_back( std::move( entry ) );
        return static_cast<int>( m_Entries.size() );
    }

    const ComponentEditorEntry* ComponentWidgetRegistry::Find( const std::string_view name ) const
    {
        for ( const ComponentEditorEntry& entry : m_Entries )
            if ( entry.Name == name )
                return &entry;
        return nullptr;
    }

    namespace
    {
        namespace ImGui = ::ImGui;

        // Add-Component menu categories (order = menu order). Keyword-bucketed by display name so it stays
        // robust to the exact registered names.
        struct AddCategory
        {
            const char* Icon;
            const char* Name;
        };
        constexpr AddCategory kAddCategories[] = {
             { ICON_MDI_SHAPE, "Rendering" },   { ICON_MDI_LIGHTBULB, "Lighting" },
             { ICON_MDI_VIEW_DASHBOARD, "UI" }, { ICON_MDI_ATOM, "Physics" },
             { ICON_MDI_RUN, "Animation" },     { ICON_MDI_VOLUME_HIGH, "Audio" },
             { ICON_MDI_VIDEO, "Camera" },      { ICON_MDI_DOTS_HORIZONTAL, "Other" },
        };

        const char* CategoryOf( const std::string& name )
        {
            const auto has = [&]( const char* s ) { return name.find( s ) != std::string::npos; };
            if ( has( "UI " ) || has( "Panel" ) || has( "Button" ) || has( "Layout" ) || has( "Canvas" ) ||
                 has( "Anchor" ) )
                return "UI";
            // Before the Skybox branch: the atmosphere IS the scene's key light source (it drives the sun
            // and the IBL bake), and it must not be swallowed by a "Sky" substring landing in Rendering.
            if ( has( "Atmosphere" ) || has( "Light" ) )
                return "Lighting";
            if ( has( "Mesh" ) || has( "Text" ) || has( "Particle" ) || has( "Skybox" ) || has( "Landscape" ) ||
                 has( "Sprite" ) || has( "Decal" ) || has( "Foliage" ) || has( "Fog" ) || has( "Cloud" ) )
                return "Rendering";
            if ( has( "Collider" ) || has( "Rigid" ) || has( "Character" ) || has( "Physics" ) ||
                 has( "Projectile" ) )
                return "Physics";
            if ( has( "Anim" ) || has( "Skeleton" ) || has( "Socket" ) || has( "Locomotion" ) )
                return "Animation"; // a socket follows a BONE; locomotion picks clips
            if ( has( "Audio" ) || has( "Sound" ) )
                return "Audio";
            if ( has( "Camera" ) )
                return "Camera";
            return "Other";
        }
    } // namespace

    void DrawAddComponentMenu( ECS::Entity& entity, ImGuiTextFilter& filter )
    {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted( ICON_MDI_MAGNIFY );
        ImGui::SameLine();

        float filterSize = ImGui::GetContentRegionAvail().x - ImGui::GetStyle().IndentSpacing;
        filterSize       = filterSize < 220 ? 220 : filterSize;
        filter.Draw( "##ComponentFilter", filterSize );
        ImGui::Separator();

        const auto addEntry = [&]( const ComponentEditorEntry& entry )
        {
            if ( ImGui::Selectable( entry.Name.c_str() ) && entry.Add )
                Commands::MutateEntityUndoable( entity.GetComponent<ECS::UUIDComponent>().UUID,
                                                [&] { entry.Add( entity ); } );
        };

        if ( filter.IsActive() )
        {
            // Flat filtered list while searching — categories only get in the way of a text query.
            for ( const auto& entry : ComponentWidgetRegistry::Get().Entries() )
            {
                if ( entry.Has && entry.Has( entity ) )
                    continue;
                if ( filter.PassFilter( entry.Name.c_str() ) )
                    addEntry( entry );
            }
            return;
        }

        for ( const auto& cat : kAddCategories )
        {
            bool any = false;
            for ( const auto& entry : ComponentWidgetRegistry::Get().Entries() )
                if ( ( !entry.Has || !entry.Has( entity ) ) &&
                     std::strcmp( CategoryOf( entry.Name ), cat.Name ) == 0 )
                {
                    any = true;
                    break;
                }
            if ( !any )
                continue;

            if ( ImGui::BeginMenu( ( std::string( cat.Icon ) + "  " + cat.Name ).c_str() ) )
            {
                for ( const auto& entry : ComponentWidgetRegistry::Get().Entries() )
                {
                    if ( entry.Has && entry.Has( entity ) )
                        continue;
                    if ( std::strcmp( CategoryOf( entry.Name ), cat.Name ) == 0 )
                        addEntry( entry );
                }
                ImGui::EndMenu();
            }
        }
    }

    bool ComponentEditContext::DrawPreview( const ImVec2& size ) const
    {
        if ( !Preview || !PreviewUI )
            return false;

        // Draw()'s own bool means "it has content yet", NOT "it drew something": it always submits an
        // item of `size` and paints its own frame (with a "No preview" label when empty). So the caller
        // must never add a placeholder of its own — hence `true` regardless.
        Preview->Draw( *PreviewUI, size );
        if ( PreviewUsed )
            *PreviewUsed = true; // the panel pays for next frame's offscreen render only while it is drawn
        return true;
    }

} // namespace Desert::Editor
