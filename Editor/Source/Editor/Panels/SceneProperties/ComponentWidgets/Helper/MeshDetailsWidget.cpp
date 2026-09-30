#include "MeshDetailsWidget.hpp"

#include <Editor/Core/IconsMaterialDesignIcons.hpp>
#include <Editor/Core/ImGuiUtilities.hpp>
#include <Editor/Core/NumberFormat.hpp>
#include <Editor/Import/ImportOptionsDialog.hpp>
#include <ImGui/imgui.h>

#include <Engine/Core/Scene.hpp>
#include <Engine/ECS/Entity.hpp>
#include <Engine/Geometry/LODSelection.hpp>
#include <Engine/Geometry/Mesh.hpp>
#include <Engine/Geometry/MeshStats.hpp>

#include <algorithm>
#include <string>

namespace Desert::Editor
{
    namespace ImGui = ::ImGui;

    namespace
    {
        // "1 234 567" via the shared, pure formatter (Editor/Core/NumberFormat.hpp). Kept as a local
        // alias so the call sites below read as they always did; the grouping rule itself is tested in
        // Desert/Tests/Editor/NumberFormat, where the wrap bug that used to split "12" into "1 2" is
        // pinned.
        inline std::string FormatCount( uint64_t value )
        {
            return ::Desert::Editor::FormatThousands( value );
        }

        // One read-only fact, on the panel's shared property row: same label column, same rules, same
        // hover band as an editable field. A stat that draws its own two-column table is exactly how a
        // Details panel ends up looking like several editors stitched together.
        void StatRow( const char* label, const std::string& value, const char* tooltip = nullptr )
        {
            Utils::ImGuiUtilities::BeginPropertyRow( label, tooltip );
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted( value.c_str() );
            if ( tooltip )
                Utils::ImGuiUtilities::Tooltip( tooltip );
            Utils::ImGuiUtilities::EndPropertyRow();
        }

        std::string FormatExtent( const glm::vec3& e )
        {
            char buf[96];
            std::snprintf( buf, sizeof( buf ), "%.1f x %.1f x %.1f cm", e.x, e.y, e.z );
            return buf;
        }

        // Per-axis scale of a world transform (basis vector lengths) — the mesh's local bounds times
        // this is the size the object actually occupies in the level.
        glm::vec3 TransformScale( const glm::mat4& m )
        {
            return { glm::length( glm::vec3( m[0] ) ), glm::length( glm::vec3( m[1] ) ),
                     glm::length( glm::vec3( m[2] ) ) };
        }

        std::string DescribeCollision( const ECS::Entity& entity )
        {
            if ( !entity.HasComponent<ECS::ColliderComponent>() )
                return "None";

            const auto& c = entity.GetComponent<ECS::ColliderComponent>().Data;
            char        buf[128];
            switch ( c.Shape )
            {
                case Physics::ShapeType::Sphere:
                    std::snprintf( buf, sizeof( buf ), "Sphere - radius %.1f cm", c.Radius );
                    break;
                case Physics::ShapeType::Capsule:
                    std::snprintf( buf, sizeof( buf ), "Capsule - radius %.1f cm, half height %.1f cm", c.Radius,
                                   c.HalfHeight );
                    break;
                case Physics::ShapeType::Mesh:
                    return "Mesh - the mesh's own triangles (static bodies only)";
                case Physics::ShapeType::ConvexHull:
                    return "Convex hull of the mesh";
                case Physics::ShapeType::Box:
                default:
                    std::snprintf( buf, sizeof( buf ), "Box - %.1f x %.1f x %.1f cm", c.HalfExtents.x * 2.0f,
                                   c.HalfExtents.y * 2.0f, c.HalfExtents.z * 2.0f );
                    break;
            }
            return buf;
        }

        // "Triangles: 1 458   Vertices: 1 186" — UE's headline on the LOD header, so a COLLAPSED Mesh
        // section still answers the one question a mesh is usually opened for.
        std::string HeadlineOf( const Geometry::MeshStats& stats )
        {
            return "Triangles: " + FormatCount( stats.Triangles ) +
                   "   Vertices: " + FormatCount( stats.Vertices );
        }

        // The name shown in the source row: the file, not the whole path (which goes in the tooltip).
        std::string SourceName( const Common::Filepath& path )
        {
            const std::string name = path.filename().string();
            return name.empty() ? path.string() : name;
        }

        // The per-element fold. UE's Material Slots list is the model: one line per element, identified
        // by index and name, with its own weight beside it.
        void DrawElements( const ::Desert::Mesh& mesh )
        {
            if ( !ImGui::TreeNodeEx( ICON_MDI_HEXAGON_MULTIPLE "  Elements", ImGuiTreeNodeFlags_SpanAvailWidth ) )
                return;

            const ImGuiTableFlags flags = ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg |
                                          ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_NoSavedSettings;

            if ( ImGui::BeginTable( "##mesh_elements", 4, flags ) )
            {
                ImGui::TableSetupColumn( "Element", ImGuiTableColumnFlags_WidthStretch, 0.40f );
                ImGui::TableSetupColumn( "Tris", ImGuiTableColumnFlags_WidthStretch, 0.22f );
                ImGui::TableSetupColumn( "Verts", ImGuiTableColumnFlags_WidthStretch, 0.22f );
                ImGui::TableSetupColumn( "LODs", ImGuiTableColumnFlags_WidthStretch, 0.16f );
                ImGui::TableHeadersRow();

                uint32_t index = 0;
                for ( const auto& sm : mesh.GetSubmeshes() )
                {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    // The index leads, the way UE labels a slot "Element 0" — the index is what the
                    // material slot mapping actually uses; the imported name is a hint.
                    std::string name = "Element " + std::to_string( index );
                    if ( !sm.Name.empty() )
                        name += "  " + sm.Name;
                    ImGui::TextUnformatted( name.c_str() );
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted( FormatCount( sm.IndexCount / 3 ).c_str() );
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted( FormatCount( sm.VertexCount ).c_str() );
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted( FormatCount( std::max<size_t>( sm.LODs.size(), 1 ) ).c_str() );
                    ++index;
                }
                ImGui::EndTable();
            }
            ImGui::TreePop();
        }
    } // namespace

    void MeshDetailsWidget::Show( const Context& ctx )
    {
        ShowMeshSection( ctx );
        // UE's Import Settings category, under the mesh facts: the source's options and Reimport (THM1l).
        if ( ctx.Asset )
            ImportOptions::DrawImportSettingsSection( ctx.Asset->GetMetadata().Filepath );
    }

    void MeshDetailsWidget::ShowMeshSection( const Context& ctx )
    {
        // The RUNTIME (GPU) mesh builds lazily, so it can genuinely be missing for a frame or two after a
        // mesh is assigned. Say WHICH state this is instead of a bare ellipsis: "no mesh assigned" and
        // "assigned, still building" are different problems and the fix differs.
        if ( !ctx.RuntimeMesh )
        {
            if ( !Utils::ImGuiUtilities::SectionHeader( ICON_MDI_SHAPE "  Mesh" ) )
                return;
            Utils::ImGuiUtilities::ResetPropertyRows();
            StatRow( "Status", ctx.Asset ? "Building the GPU mesh..." : "No mesh assigned" );
            return;
        }

        const Geometry::MeshStats stats = Geometry::ComputeMeshStats( ctx.RuntimeMesh->GetSubmeshes() );

        // The stats are computed BEFORE the header so the header can carry them, UE-style.
        const std::string headline = HeadlineOf( stats );
        if ( !Utils::ImGuiUtilities::SectionHeader( ICON_MDI_SHAPE "  Mesh", true, headline.c_str() ) )
            return;

        Utils::ImGuiUtilities::ResetPropertyRows();

        // What this mesh IS, first — the row UE puts at the top of a mesh component. A primitive has no
        // source file, and saying so beats an empty row.
        if ( ctx.Asset )
        {
            const Common::Filepath path = ctx.Asset->GetMetadata().Filepath;
            StatRow( "Source", ICON_MDI_CUBE_OUTLINE "  " + SourceName( path ), path.string().c_str() );
        }
        else
        {
            StatRow( "Source", "Procedural (no source asset)",
                     "Primitives and in-editor edited meshes are generated, not imported" );
        }

        // Wording follows UE's Static Mesh Editor stat block, so the numbers mean the same thing to
        // anyone who has read that panel: Triangles / Vertices / UV Channels / Approx Size. They repeat
        // the header's headline on purpose — the header's copy is dropped when the panel is too narrow.
        StatRow( "Triangles", FormatCount( stats.Triangles ), "Triangle count of LOD 0 (all elements)" );
        StatRow( "Vertices", FormatCount( stats.Vertices ) );
        StatRow( "Elements", FormatCount( stats.Elements ),
                 "Submeshes. Each one takes its material from the slot of the same index." );

        // LOD: how many levels the chain has and which one is on screen RIGHT NOW — computed with
        // the renderer's own policy (Geometry::SelectLOD), so it can't drift from what is drawn.
        {
            std::string lod = FormatCount( stats.LODLevels ) + ( stats.LODLevels == 1 ? " level" : " levels" );
            // With a single level there is nothing to resolve — "drawing LOD 0" would be noise.
            if ( stats.LODLevels > 1 )
            {
                if ( ctx.ForcedLOD >= 0 )
                {
                    const uint32_t forced =
                         std::min( static_cast<uint32_t>( ctx.ForcedLOD ), stats.LODLevels - 1 );
                    lod += "  -  drawing LOD " + std::to_string( forced ) + " (forced)";
                }
                else if ( ctx.Scene && ctx.Entity )
                {
                    if ( const auto& camera = ctx.Scene->GetActiveCamera() )
                    {
                        const uint32_t active = std::min(
                             Geometry::SelectLOD( ctx.Entity->GetWorldTransform(), ctx.RuntimeMesh->GetSubmeshes(),
                                                  camera->GetPosition(), -1, ctx.LODBias ),
                             stats.LODLevels - 1 );
                        lod += "  -  drawing LOD " + std::to_string( active ) + " (auto)";
                    }
                }
            }
            StatRow( "LOD", lod, "Levels in the chain, and the one the current view resolves to" );
        }

        if ( stats.HasBounds )
        {
            StatRow( "Approx Size", FormatExtent( stats.Extent ), "Local-space extent of the mesh" );

            if ( ctx.Entity )
            {
                const glm::vec3 scale = TransformScale( ctx.Entity->GetWorldTransform() );
                if ( glm::any( glm::greaterThan( glm::abs( scale - glm::vec3( 1.0f ) ), glm::vec3( 0.001f ) ) ) )
                    StatRow( "World Size", FormatExtent( stats.Extent * scale ),
                             "Bounds after the entity's world scale" );
            }
        }

        StatRow( "UV Channels", "1",
                 "The engine vertex format carries a single UV set (Vertex::TexCoord), so a mesh "
                 "exposes one channel however many the source file had." );

        if ( ctx.Entity )
            StatRow( "Collision", DescribeCollision( *ctx.Entity ),
                     "Collider component on this entity (physics uses this shape, not the mesh)" );

        // Per-element detail, folded: which element is which, and how heavy each one is.
        if ( stats.Elements > 0 )
        {
            ImGui::Dummy( ImVec2( 0.0f, 2.0f ) );
            DrawElements( *ctx.RuntimeMesh );
        }
    }

} // namespace Desert::Editor
