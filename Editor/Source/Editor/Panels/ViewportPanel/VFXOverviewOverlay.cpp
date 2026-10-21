#include "VFXOverviewOverlay.hpp"

#include <Engine/VFX/VFXWorld.hpp>

#include <cstdint>
#include <format>
#include <string>
#include <vector>

namespace Desert::Editor
{
    void DrawVFXOverviewOverlay( const VFX::VFXDebugStats& stats, const ImVec2& viewportMin,
                                 const ImVec2& viewportMax )
    {
        std::uint64_t emitters = 0, alive = 0, capacity = 0, overflow = 0;
        for ( const VFX::VFXSystemDebugStats& system : stats.Systems )
            for ( const VFX::VFXEmitterDebugStats& e : system.Emitters )
            {
                ++emitters;
                alive += e.Alive;
                capacity += e.Capacity;
                overflow += e.ChannelOverflowTotal;
            }

        std::vector<std::string> rows;
        rows.push_back( std::format( "VFX: {} system(s), {} emitter(s), alive {} / {}{}{}", stats.Systems.size(),
                                     emitters, alive, capacity, overflow > 0 ? std::format( ", overflow {}", overflow ) : "",
                                     stats.Frozen ? " - FROZEN" : "" ) );
        if ( !stats.GpuCounts )
            rows.emplace_back( "  (GPU counts not read back yet)" );
        for ( const VFX::VFXSystemDebugStats& system : stats.Systems )
        {
            rows.push_back( std::format( "  {}", system.EntityUuid ) );
            for ( const VFX::VFXEmitterDebugStats& e : system.Emitters )
                rows.push_back( std::format( "    {}: {} / {}{}{}", e.Name,
                                             e.GpuRead ? std::to_string( e.Alive ) : std::string( "-" ), e.Capacity,
                                             e.Runs ? "" : " (inactive)",
                                             e.Channel.Overflow > 0 ? std::format( ", overflow {}", e.Channel.Overflow )
                                                                    : "" ) );
        }

        // Top-right under the axis triad (render methods hold the bottom-left, the ray tracing rows the
        // bottom-right, the perf HUD the top-left).
        constexpr float kMargin = 12.0f;
        constexpr float kTriad  = 110.0f;
        const float     lineH   = ImGui::GetTextLineHeight();
        ImDrawList*     dl      = ImGui::GetWindowDrawList();
        float           y       = viewportMin.y + kTriad;
        for ( const std::string& row : rows )
        {
            const ImVec2 at( viewportMax.x - kMargin - ImGui::CalcTextSize( row.c_str() ).x, y );
            dl->AddText( ImVec2( at.x + 1.0f, at.y + 1.0f ), IM_COL32( 0, 0, 0, 200 ), row.c_str() );
            dl->AddText( at, stats.Frozen ? IM_COL32( 120, 200, 255, 255 ) : IM_COL32( 225, 230, 240, 210 ),
                         row.c_str() );
            y += lineH;
        }
    }
} // namespace Desert::Editor
