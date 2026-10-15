#include "RenderMethodsOverlay.hpp"

#include <Engine/Graphic/RenderMethods.hpp>
#include <Engine/Graphic/View/ViewFrame.hpp>

#include <array>
#include <string>
#include <vector>

namespace Desert::Editor
{
    void DrawRenderMethodsOverlay( const Graphic::RenderMethods&      methods,
                                   const Graphic::TemporalViewStatus& temporal, const ImVec2& viewportMin,
                                   const ImVec2& viewportMax )
    {
        std::vector<std::string> rows = Graphic::FormatRenderMethods( methods );
        rows.push_back( Graphic::FormatTemporalViewStatus( temporal ) );
        const std::array<bool, 4> lowered = { methods.GlobalIllumination.Lowered(), methods.Reflections.Lowered(),
                                              methods.Shadows.Lowered(),
                                              temporal.Method != Graphic::TemporalMethod::None &&
                                                   temporal.HistoryReset != Graphic::HistoryResetReason::None };

        constexpr float kMargin = 12.0f;
        const float     lineH   = ImGui::GetTextLineHeight();
        ImDrawList*     dl      = ImGui::GetWindowDrawList();
        float           y       = viewportMax.y - kMargin - lineH * static_cast<float>( rows.size() );
        for ( std::size_t i = 0; i < rows.size(); ++i, y += lineH )
        {
            const ImVec2 at( viewportMin.x + kMargin, y );
            // One dark tap behind the text instead of a plate, as the viewport's other labels: readable over a
            // white sky without hiding the scene in the corner.
            dl->AddText( ImVec2( at.x + 1.0f, at.y + 1.0f ), IM_COL32( 0, 0, 0, 200 ), rows[i].c_str() );
            dl->AddText( at, lowered[i] ? IM_COL32( 240, 190, 80, 255 ) : IM_COL32( 225, 230, 240, 210 ),
                         rows[i].c_str() );
        }
    }
} // namespace Desert::Editor
