#include "PerfHudOverlay.hpp"

#include <Common/Core/Profiler.hpp>

#include <algorithm>
#include <cstdio>
#include <vector>
#include <Engine/Graphic/DrawCounters.hpp>

namespace Desert::Editor
{
    void PerfHudOverlay::Draw( const ImVec2& viewportMin, const ImVec2& viewportMax )
    {
        // Feed the history from ImGui's frame delta (same clock the user perceives).
        const float frameMs = ImGui::GetIO().DeltaTime * 1000.0f;
        m_FrameMs[m_Head]   = frameMs;
        m_Head              = ( m_Head + 1 ) % kHistory;
        if ( m_Head == 0 )
            m_Filled = true;

        const int count = m_Filled ? kHistory : m_Head;
        if ( count == 0 )
            return;

        float avgMs = 0.0f, maxMs = 0.0f;
        for ( int i = 0; i < count; ++i )
        {
            avgMs += m_FrameMs[i];
            maxMs = std::max( maxMs, m_FrameMs[i] );
        }
        avgMs /= static_cast<float>( count );
        const float fps = avgMs > 0.0f ? 1000.0f / avgMs : 0.0f;

        // Top CPU scopes from the engine aggregator (already averaged over its window).
        auto scopes = Common::Profiling::Profiler::Get().LastFrame();
        std::sort( scopes.begin(), scopes.end(),
                   []( const auto& a, const auto& b ) { return a.TotalMs > b.TotalMs; } );
        const int scopeRows = std::min<int>( 5, static_cast<int>( scopes.size() ) );

        constexpr float kPad    = 8.0f;
        constexpr float kWidth  = 240.0f;
        constexpr float kGraphH = 42.0f;
        const float     lineH   = ImGui::GetTextLineHeightWithSpacing();
        // +lineH для строки отрисовок: рамка считается по тому, ЧТО рисуется, иначе новая строка
        // вылезает за подложку — и это видно только на кадре, чего ни один тест не скажет.
        const float     height =
             kPad * 2.0f + lineH /*fps*/ + kGraphH + 4.0f + lineH /*draws*/ + scopeRows * lineH;

        const ImVec2 p0( viewportMax.x - kWidth - 12.0f, viewportMin.y + 12.0f );
        const ImVec2 p1( p0.x + kWidth, p0.y + height );

        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled( p0, p1, IM_COL32( 15, 15, 18, 200 ), 6.0f );
        dl->AddRect( p0, p1, IM_COL32( 255, 255, 255, 30 ), 6.0f );

        // Header: colour-coded FPS (green >= 55, yellow >= 25, red below).
        const ImU32 fpsCol = fps >= 55.0f   ? IM_COL32( 120, 230, 120, 255 )
                             : fps >= 25.0f ? IM_COL32( 235, 200, 90, 255 )
                                            : IM_COL32( 240, 110, 100, 255 );
        char        header[64];
        std::snprintf( header, sizeof( header ), "%.0f FPS  %.2f ms", fps, avgMs );
        dl->AddText( ImVec2( p0.x + kPad, p0.y + kPad ), fpsCol, header );

        // Frame-time graph: scale to max(33ms, observed max) so vsync'd 16.7ms sits mid-graph.
        const ImVec2 g0( p0.x + kPad, p0.y + kPad + lineH );
        const ImVec2 g1( p1.x - kPad, g0.y + kGraphH );
        dl->AddRectFilled( g0, g1, IM_COL32( 255, 255, 255, 10 ), 2.0f );
        const float scaleMs = std::max( 33.3f, maxMs );
        const float stepX   = ( g1.x - g0.x ) / static_cast<float>( kHistory - 1 );
        ImVec2      prev;
        for ( int i = 0; i < count; ++i )
        {
            const int   idx = m_Filled ? ( m_Head + i ) % kHistory : i;
            const float t   = std::clamp( m_FrameMs[idx] / scaleMs, 0.0f, 1.0f );
            const ImVec2 pt( g0.x + stepX * i, g1.y - t * ( g1.y - g0.y ) );
            if ( i > 0 )
                dl->AddLine( prev, pt, IM_COL32( 120, 200, 255, 200 ), 1.0f );
            prev = pt;
        }
        // 16.7 ms reference line.
        const float refY = g1.y - std::clamp( 16.7f / scaleMs, 0.0f, 1.0f ) * ( g1.y - g0.y );
        dl->AddLine( ImVec2( g0.x, refY ), ImVec2( g1.x, refY ), IM_COL32( 120, 230, 120, 60 ) );

        // DRAW CALLS, and this line is the point of the counter rather than a decoration. The
        // world-scale scene (50 179 entities) could name its entity count, its file size, its load time
        // and its resident set — and could name NEITHER a frame time nor a draw call, because nothing
        // counted them. `Docs/World/PROGRAMME.md` step 2: the programme is measured against detectors,
        // and this is the number that tells "we draw everything" apart from "we draw too much".
        //
        // Instances are shown BESIDE draws, never instead of them: an ISM batch is one draw and many
        // instances, so a single figure would report the batch as either one object or as thousands of
        // calls — and step 3 (culling) is judged on exactly that difference.
        float y = g1.y + 4.0f;
        {
            const Graphic::DrawCounters draws = Graphic::DrawCounter::LastFrame();
            char                        row[96];
            std::snprintf( row, sizeof( row ), "%-18.18s %6u", "draws / instances", draws.Draws );
            dl->AddText( ImVec2( p0.x + kPad, y ), IM_COL32( 190, 200, 235, 255 ), row );
            char tail[32];
            std::snprintf( tail, sizeof( tail ), " / %u", draws.Instances );
            dl->AddText( ImVec2( p0.x + kPad + 160.0f, y ), IM_COL32( 150, 160, 190, 255 ), tail );
            y += lineH;
        }

        // Top scopes.
        for ( int i = 0; i < scopeRows; ++i )
        {
            char row[96];
            std::snprintf( row, sizeof( row ), "%-18.18s %6.2f ms", scopes[i].Name.c_str(),
                           scopes[i].TotalMs );
            dl->AddText( ImVec2( p0.x + kPad, y ), IM_COL32( 210, 210, 215, 255 ), row );
            y += lineH;
        }
    }
} // namespace Desert::Editor
