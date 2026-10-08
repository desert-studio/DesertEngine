#include <Engine/UI/Widgets/UIWidgets.hpp>

#include <Engine/UI/UICanvasRenderer2D.hpp>
#include <Engine/UI/UIOverlay.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/Assets/Common.hpp>
#include <Engine/Text/BakedFont.hpp>
#include <Engine/UI/UIStyleResolver.hpp>
#include <Engine/Text/Utf8.hpp>
#include <Engine/UI/UICanvasLayout.hpp>
#include <Engine/UI/UIDataStore.hpp>
#include <Engine/UI/UIPathGeometry.hpp>
#include <Engine/UI/UIRichText.hpp>

#include <Common/Core/Logger.hpp>

#include <algorithm>
#include <span>
#include <unordered_set>
#include <array>
#include <optional>
#include <cstdint>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <vector>

namespace Desert::UI::Walk
{
    void DrawSliderWidget( ElementFrame& frame )
    {
        auto& ctx       = frame.Ctx;
        auto& reg       = frame.Reg;
        auto& e         = frame.E;
        auto& dl        = frame.Dl;
        auto& input     = frame.Input;
        auto& st        = frame.St;
        auto& rect      = frame.ElementRect;
        auto& pointerPx = frame.PointerPx;
        auto& mn        = frame.Mn;
        auto& mx        = frame.Mx;
        auto& hot       = frame.Hot;

        auto&       sl    = reg.get<ECS::UISliderComponent>( e ).Data;
        const float range = std::max( 0.0001f, sl.MaxValue - sl.MinValue );
        const float t     = std::clamp( ( sl.Value - sl.MinValue ) / range, 0.0f, 1.0f );
        const float pill  = rect.H * 0.5f; // fully-rounded track ends
        const float fillX = mn.x + rect.W * t;
        const float cy    = ( mn.y + mx.y ) * 0.5f;
        const float hs    = rect.H * 0.6f; // handle half-size (circle via rounding)
        dl.AddRectFilled(
             mn, mx, Tinted( ctx, glm::vec4( st.Color( StyleSlot::SliderTrack, sl.TrackColor ), 1.0f ) ), pill );
        if ( t > 0.0f )
            dl.AddRectFilled( mn, { fillX, mx.y },
                              glm::vec4( st.Color( StyleSlot::SliderFill, sl.FillColor ), 1.0f ), pill );
        dl.AddRectFilled( { fillX - hs, cy - hs }, { fillX + hs, cy + hs },
                          glm::vec4( st.Color( StyleSlot::SliderHandle, sl.HandleColor ), 1.0f ), hs );

        const bool hover = input && hot;
        if ( hover && input->MouseDown )
        {
            // The slider's fraction is measured along ITS OWN track, so it takes the undone
            // pointer: dragging a rotated slider follows the track you can see rather than
            // the screen's x axis.
            const float nt = std::clamp( ( pointerPx.x - mn.x ) / std::max( 1.0f, rect.W ), 0.0f, 1.0f );
            sl.Value       = sl.MinValue + nt * range;
        }
    }
} // namespace Desert::UI::Walk
