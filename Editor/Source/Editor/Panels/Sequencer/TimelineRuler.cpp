#include "TimelineRuler.hpp"

#include <cstdio>

namespace Desert::Editor::Sequencer
{
    CurveViewport TimeAxis( const float laneX0, const float laneW, const float durationSeconds )
    {
        CurveViewport axis;
        axis.X0        = laneX0;
        axis.X1        = laneX0 + laneW;
        axis.TimeStart = 0.0;
        axis.TimeEnd   = durationSeconds > 0.0f ? static_cast<double>( durationSeconds ) : 1.0;
        return axis;
    }

    // What this replaced in the ruler was `for (int u = 0; u <= duration; ++u)`: one line per whole SECOND,
    // labelled with an integer second. So the grid an animator SAW and the grid their dragged key LANDED ON
    // were different grids, and the display rate was visible nowhere in the window that owns it.
    void DrawFrameGrid( ImDrawList* dl, const CurveViewport& axis, const float yTop, const float yBottom,
                        const Animation::FrameNumber durationTicks, const Animation::FrameRate tickRate,
                        const Animation::FrameRate displayRate, const bool labels, const ImU32 lineColour )
    {
        const double fps = displayRate.AsDouble();
        if ( !( fps > 0.0 ) )
        {
            return;
        }
        const double  secondsPerFrame = 1.0 / fps;
        const int32_t step = ChooseFrameStep( axis.PixelsPerSecond() * secondsPerFrame, labels ? 44.0f : 9.0f );
        const int32_t last = Animation::DisplayFrameIndex( durationTicks, tickRate, displayRate );

        for ( int32_t frame = 0; frame <= last; frame += step )
        {
            const float x = axis.TimeToX( static_cast<double>( frame ) * secondsPerFrame );
            dl->AddLine( ImVec2( x, yTop ), ImVec2( x, yBottom ), lineColour );
            if ( labels )
            {
                char buf[16];
                std::snprintf( buf, sizeof( buf ), "%d", frame );
                dl->AddText( ImVec2( x + 3.0f, yTop + 3.0f ), IM_COL32( 190, 190, 190, 160 ), buf );
            }
        }
    }

    void DrawPlayhead( ImDrawList* dl, const CurveViewport& axis, const double seconds, const float yTop,
                       const float yBottom, const float rulerBottom )
    {
        const float x = axis.TimeToX( seconds );
        dl->AddLine( ImVec2( x, yTop ), ImVec2( x, yBottom ), IM_COL32( 255, 90, 90, 190 ), 1.5f );
        if ( rulerBottom > yTop )
        {
            dl->AddTriangleFilled( ImVec2( x - 6.0f, yTop ), ImVec2( x + 6.0f, yTop ), ImVec2( x, rulerBottom ),
                                   IM_COL32( 255, 90, 90, 230 ) );
        }
    }
} // namespace Desert::Editor::Sequencer
