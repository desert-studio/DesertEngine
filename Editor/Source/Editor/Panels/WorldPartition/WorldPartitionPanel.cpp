// Ported from UE 5.8
// Engine/Source/Editor/WorldPartitionEditor/Private/WorldPartition/SWorldPartitionEditorGrid2D.cpp :1190-1212
// (mouse: wheel zoom, right-drag pan), :1220-1262 (PaintGrid: the level's grid lines and the X/Y axes at 40 %
// opacity), :1480-1530 (PaintStreamingSources: the source's position, view direction and loading circle), and
// Engine/Source/Runtime/Engine/Private/WorldPartition/WorldPartitionRuntimeSpatialHash.cpp:1040-1075 (Draw2D:
// one filled tile per cell, outlined), adapted: Slate painting becomes an ImGui draw list; the cell tiles and
// their states come from our partition plan and residency (WorldPartitionMap.hpp) instead of a runtime hash;
// UE's unload band (the source's range times the unload margin) is drawn dashed as its own circle; the map's
// vertical axis is world Z (the ground plane of a Y-up engine) instead of UE's Y.

#include "WorldPartitionPanel.hpp"

#include <Engine/Core/Camera.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/Core/Serialize/SceneSerializer.hpp>
#include <Engine/Core/WorldStreamer.hpp>

#include <ImGui/imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <numbers>

namespace Desert::Editor
{
    namespace ImGui = ::ImGui;
    namespace Map   = WorldPartitionMap;
    using ::Desert::Core::Rules::WorldPartitionPlan;

    namespace
    {
        // UE's minimum on-screen grid cell (SWorldPartitionEditorGrid2D PaintGrid): finer levels would be noise.
        constexpr double kMinGridPixels = 24.0;
        constexpr float  kDashSegments  = 64.0f;

        ImU32 ToU32( glm::vec4 rgba )
        {
            return ImGui::ColorConvertFloat4ToU32( ImVec4( rgba.r, rgba.g, rgba.b, rgba.a ) );
        }

        ImVec2 ToScreen( const Map::View& view, glm::dvec2 size, ImVec2 origin, glm::dvec2 world )
        {
            const glm::dvec2 local = Map::WorldToScreen( view, size, world );
            return { origin.x + static_cast<float>( local.x ), origin.y + static_cast<float>( local.y ) };
        }

        void DashedCircle( ImDrawList& list, ImVec2 centre, float radius, ImU32 colour )
        {
            // Every other segment of a 2·kDashSegments polygon.
            const float step = std::numbers::pi_v<float> / kDashSegments;
            for ( int i = 0; i < static_cast<int>( 2.0f * kDashSegments ); i += 2 )
            {
                const float a0 = step * static_cast<float>( i );
                const float a1 = a0 + step;
                list.AddLine( ImVec2( centre.x + radius * std::cos( a0 ), centre.y + radius * std::sin( a0 ) ),
                              ImVec2( centre.x + radius * std::cos( a1 ), centre.y + radius * std::sin( a1 ) ),
                              colour, 1.5f );
            }
        }

        constexpr std::array kLegendStates = { Map::CellState::Unstreamed, Map::CellState::Unloaded,
                                               Map::CellState::Loading,    Map::CellState::Loaded,
                                               Map::CellState::Resident,   Map::CellState::Failed };
    } // namespace

    WorldPartitionPanel::WorldPartitionPanel( std::shared_ptr<::Desert::Core::Scene> scene,
                                              const ::Desert::Assets::AssetManager*  assets,
                                              StreamerGetter                         streamer )
         : IPanel( "World Partition", false ), m_Scene( std::move( scene ) ), m_Assets( assets ),
           m_Streamer( std::move( streamer ) )
    {
    }

    void WorldPartitionPanel::SetScene( const std::shared_ptr<::Desert::Core::Scene>& scene )
    {
        if ( scene == m_Scene )
            return;
        m_Scene         = scene;
        m_EditPlanStale = true;
        m_FocusPending  = true;
    }

    void WorldPartitionPanel::RebuildEditPlan()
    {
        m_EditPlanStale = false;
        m_EditPlan.reset();
        if ( !m_Scene )
        {
            m_EditPlanStatus = "No active scene.";
            return;
        }
        // The loader's own path from a scene to a plan, so the map shows the partition Play and the cook make.
        const std::string json   = ::Desert::Core::SceneSerializer( m_Scene.get(), m_Assets ).SerializeToJson();
        auto              parsed = ::Desert::Core::ParseLoadableScene( "<World Partition panel>", json );
        if ( !parsed )
        {
            m_EditPlanStatus = "The scene could not be planned: " + parsed.GetError();
            return;
        }
        const ::Desert::Core::SceneSerialized& scene = parsed.GetValue();
        if ( !scene.WorldPartition )
        {
            m_EditPlanStatus = "This scene is not partitioned: it states no WorldPartition block.";
            return;
        }
        if ( scene.WorldPartition->Grids.empty() )
        {
            m_EditPlanStatus = "This scene's WorldPartition block states no grid.";
            return;
        }
        m_EditPartition = *scene.WorldPartition;
        m_EditPlan      = ::Desert::Core::Rules::PlanWorldPartition( scene.Entities, m_EditPartition,
                                                                     ::Desert::Core::RegistryMeshBounds() );
        m_EditPlanStatus.clear();
        m_FocusPending = true;
    }

    void WorldPartitionPanel::OnUIRender()
    {
        const ::Desert::Core::WorldStreamer* streamer = ( m_Streamer && m_Scene ) ? m_Streamer() : nullptr;
        if ( streamer && !streamer->Streams( *m_Scene ) )
            streamer = nullptr; // it streams another scene view's world, not the one this panel follows
        const bool playing = streamer != nullptr;
        if ( playing != m_WasPlaying )
        {
            m_WasPlaying   = playing;
            m_FocusPending = true;
            if ( !playing )
                m_EditPlanStale = true; // Stop rebuilt the scene: the old plan's indices describe nothing now
        }

        if ( !playing && m_EditPlanStale )
            RebuildEditPlan();

        // ── toolbar ──
        if ( playing )
        {
            ImGui::TextUnformatted( "Play: streaming" );
            ImGui::SameLine();
            ImGui::Checkbox( "Follow camera", &m_Follow );
        }
        else
        {
            ImGui::TextUnformatted( "Edit: partition plan" );
            ImGui::SameLine();
            if ( ImGui::Button( "Refresh" ) )
                RebuildEditPlan();
        }
        ImGui::SameLine();
        if ( ImGui::Button( "Focus" ) )
        {
            m_FocusPending = true;
            m_Follow       = false;
        }

        const WorldPartitionPlan*                       plan      = nullptr;
        const ::Desert::Core::WorldPartitionSerialized* partition = nullptr;
        const ::Desert::Core::Rules::ResidencyState*    residency = nullptr;
        if ( playing )
        {
            plan      = &streamer->Plan();
            partition = &streamer->Partition();
            residency = &streamer->Residency();
        }
        else if ( m_EditPlan )
        {
            plan      = &*m_EditPlan;
            partition = &m_EditPartition;
        }

        if ( !plan || partition->Grids.empty() )
        {
            ImGui::TextDisabled( "%s",
                                 playing ? "The streamed partition states no grid." : m_EditPlanStatus.c_str() );
            return;
        }

        ImGui::SameLine();
        ImGui::SetNextItemWidth( 120.0f );
        const int maxLevel = std::max( plan->LevelCount - 1, 0 );
        ImGui::SliderInt( "Level", &m_Level, -1, maxLevel, m_Level < 0 ? "all" : "L%d" );
        ImGui::SameLine();
        ImGui::TextDisabled( "%zu cells, %zu always-loaded, %d level(s), cell %.0f m", plan->Cells.size(),
                             plan->AlwaysLoaded.size(), plan->LevelCount,
                             static_cast<double>( partition->Grids[0].CellSize ) / 100.0 );

        DrawMap( *plan, *partition, residency, streamer );
    }

    void WorldPartitionPanel::DrawMap( const WorldPartitionPlan&                       plan,
                                       const ::Desert::Core::WorldPartitionSerialized& partition,
                                       const ::Desert::Core::Rules::ResidencyState*    residency,
                                       const ::Desert::Core::WorldStreamer*            streamer )
    {
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        ImVec2       avail  = ImGui::GetContentRegionAvail();
        avail.x             = std::max( avail.x, 64.0f );
        avail.y             = std::max( avail.y, 64.0f );
        const glm::dvec2 size( avail.x, avail.y );
        const float      cellSize = partition.Grids[0].CellSize;
        const float      range    = partition.Grids[0].LoadingRange;

        ImGui::InvisibleButton( "##wpmap", avail,
                                ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                                     ImGuiButtonFlags_MouseButtonMiddle );
        const bool     hovered = ImGui::IsItemHovered();
        const ImGuiIO& io      = ImGui::GetIO();

        // Where the streaming source is: the streamer's last one in Play, the scene's camera in Edit.
        std::optional<glm::vec3> sourcePos;
        float                    rangeScale = 1.0f;
        if ( streamer && streamer->LastSource() )
        {
            sourcePos  = streamer->LastSource()->Position;
            rangeScale = streamer->LastSource()->RangeScale;
        }
        std::shared_ptr<::Desert::Core::Camera> camera = m_Scene ? m_Scene->GetActiveCamera() : nullptr;
        if ( !sourcePos && camera )
            sourcePos = camera->GetPosition();

        // ── view: focus, follow, wheel, drag ──
        if ( m_FocusPending )
        {
            m_FocusPending = false;
            if ( streamer && m_Follow && sourcePos )
                m_View = Map::Follow( size, { sourcePos->x, sourcePos->z } );
            else if ( const auto bounds = Map::PlanBounds( plan ) )
                Map::Focus( m_View, size, *bounds );
        }
        if ( streamer && m_Follow && sourcePos )
            m_View = Map::Follow( size, { sourcePos->x, sourcePos->z } );
        if ( hovered && io.MouseWheel != 0.0f )
        {
            Map::Zoom( m_View, size, { io.MousePos.x - origin.x, io.MousePos.y - origin.y }, io.MouseWheel );
            m_Follow = false;
        }
        if ( ImGui::IsItemActive() && ( ImGui::IsMouseDragging( ImGuiMouseButton_Right ) ||
                                        ImGui::IsMouseDragging( ImGuiMouseButton_Middle ) ) )
        {
            Map::Pan( m_View, { io.MouseDelta.x, io.MouseDelta.y } );
            m_Follow = false;
        }

        ImDrawList&  list = *ImGui::GetWindowDrawList();
        const ImVec2 corner( origin.x + avail.x, origin.y + avail.y );
        list.PushClipRect( origin, corner, true );
        list.AddRectFilled( origin, corner, IM_COL32( 22, 22, 24, 255 ) );

        // ── background grid of the level legible at this zoom ──
        {
            const int        level = Map::GridLineLevel( m_View, cellSize, kMinGridPixels );
            const double     edge  = ::Desert::Core::Rules::LevelCellSize( cellSize, level );
            const glm::dvec2 w0    = Map::ScreenToWorld( m_View, size, { 0.0, 0.0 } );
            const glm::dvec2 w1    = Map::ScreenToWorld( m_View, size, size );
            const ImU32      line  = IM_COL32( 255, 255, 255, 20 );
            for ( double x = std::floor( std::min( w0.x, w1.x ) / edge ) * edge; x <= std::max( w0.x, w1.x );
                  x += edge )
            {
                const float sx = ToScreen( m_View, size, origin, { x, 0.0 } ).x;
                list.AddLine( ImVec2( sx, origin.y ), ImVec2( sx, corner.y ), line );
            }
            for ( double z = std::floor( std::min( w0.y, w1.y ) / edge ) * edge; z <= std::max( w0.y, w1.y );
                  z += edge )
            {
                const float sy = ToScreen( m_View, size, origin, { 0.0, z } ).y;
                list.AddLine( ImVec2( origin.x, sy ), ImVec2( corner.x, sy ), line );
            }
        }

        // ── cells, coarse levels first ──
        std::array<std::size_t, kLegendStates.size()> stateCounts{};
        for ( const std::size_t index : Map::VisibleCells( plan, m_View, size, m_Level ) )
        {
            const auto&          cell  = plan.Cells[index];
            const Map::CellState state = Map::StateOf( plan, residency, index );
            stateCounts[static_cast<std::size_t>( state )]++;
            const glm::vec4 fill = Map::ColorOf( state );
            const ImVec2    a    = ToScreen( m_View, size, origin, { cell.Square.MinX, cell.Square.MinZ } );
            const ImVec2    b    = ToScreen( m_View, size, origin, { cell.Square.MaxX, cell.Square.MaxZ } );
            const ImVec2    lo( std::min( a.x, b.x ), std::min( a.y, b.y ) );
            const ImVec2    hi( std::max( a.x, b.x ), std::max( a.y, b.y ) );
            list.AddRectFilled( lo, hi, ToU32( fill ) );
            list.AddRect( lo, hi, ToU32( { fill.r, fill.g, fill.b, 0.9f } ) );
        }

        // ── world axes: X red, Z green, at UE's 40 % ──
        {
            const ImVec2 zero = ToScreen( m_View, size, origin, { 0.0, 0.0 } );
            list.AddLine( ImVec2( origin.x, zero.y ), ImVec2( corner.x, zero.y ), IM_COL32( 255, 0, 0, 102 ) );
            list.AddLine( ImVec2( zero.x, origin.y ), ImVec2( zero.x, corner.y ), IM_COL32( 0, 255, 0, 102 ) );
        }

        // ── the streaming source: its loading circle, its unload band, where it looks ──
        if ( sourcePos )
        {
            const ImVec2 at     = ToScreen( m_View, size, origin, { sourcePos->x, sourcePos->z } );
            const double loadCm = static_cast<double>( range ) * static_cast<double>( rangeScale );
            const float  margin = streamer ? streamer->Settings().UnloadMargin
                                           : ::Desert::Core::Rules::ResidencySettings{}.UnloadMargin;
            const ImU32  ring   = IM_COL32( 255, 255, 255, 200 );
            list.AddCircle( at, static_cast<float>( Map::RadiusPixels( m_View, loadCm ) ), ring, 96, 1.5f );
            DashedCircle( list, at, static_cast<float>( Map::RadiusPixels( m_View, loadCm * ( 1.0 + margin ) ) ),
                          IM_COL32( 255, 255, 255, 120 ) );
            if ( camera )
            {
                // The view's forward is the third row of the view rotation, negated.
                const glm::mat4  view = camera->GetViewMatrix();
                const glm::dvec2 forward( -view[0][2], -view[2][2] );
                if ( const double length = glm::length( forward ); length > 1e-6 )
                {
                    const glm::dvec2 dir = forward / length;
                    const glm::dvec2 tip = glm::dvec2( sourcePos->x, sourcePos->z ) + dir * loadCm * 0.5;
                    list.AddLine( at, ToScreen( m_View, size, origin, tip ), IM_COL32( 255, 220, 64, 255 ), 2.0f );
                }
            }
            list.AddCircleFilled( at, 5.0f, IM_COL32( 255, 220, 64, 255 ) );
        }

        // ── legend: the states on screen ──
        {
            ImVec2 row( origin.x + 8.0f, corner.y - 8.0f );
            for ( std::size_t i = kLegendStates.size(); i-- > 0; )
            {
                if ( stateCounts[i] == 0 )
                    continue;
                row.y -= ImGui::GetTextLineHeight() + 2.0f;
                const float h = ImGui::GetTextLineHeight();
                list.AddRectFilled( row, ImVec2( row.x + h, row.y + h ),
                                    ToU32( Map::ColorOf( kLegendStates[i] ) ) );
                const std::string text = std::format( "{} ({})", Map::NameOf( kLegendStates[i] ), stateCounts[i] );
                list.AddText( ImVec2( row.x + h + 6.0f, row.y ), IM_COL32( 230, 230, 230, 255 ), text.c_str() );
            }
        }
        list.PopClipRect();

        // ── tooltip: the cell under the cursor ──
        if ( hovered )
        {
            const glm::dvec2 world =
                 Map::ScreenToWorld( m_View, size, { io.MousePos.x - origin.x, io.MousePos.y - origin.y } );
            if ( const auto index = Map::CellAt( plan, world, m_Level ) )
            {
                const auto& cell = plan.Cells[*index];
                ImGui::BeginTooltip();
                ImGui::Text( "%s  (%d, %d)", Map::LevelLabel( cellSize, cell.Level ).c_str(), cell.Cell.X,
                             cell.Cell.Z );
                ImGui::Text( "%s, %zu composite(s)", Map::NameOf( Map::StateOf( plan, residency, *index ) ),
                             cell.Composites.size() );
                ImGui::EndTooltip();
            }
        }
    }
} // namespace Desert::Editor
