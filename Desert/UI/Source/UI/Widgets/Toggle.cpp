#include <UI/Widgets/UIWidgets.hpp>

#include <UI/UICanvasRenderer2D.hpp>
#include <UI/UIOverlay.hpp>
#include <Common/Core/AssetHandle.hpp>
#include <Render2DCore/Text/BakedFont.hpp>
#include <UI/UIStyleResolver.hpp>
#include <Render2DCore/Text/Utf8.hpp>
#include <UI/UICanvasLayout.hpp>
#include <UI/UIDataStore.hpp>
#include <UI/UIPathGeometry.hpp>
#include <UI/UIRichText.hpp>

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
    void DrawToggleWidget( ElementFrame& frame )
    {
        auto& ctx         = frame.Ctx;
        auto& tree        = frame.Tree;
        auto& e           = frame.E;
        auto& scale       = frame.Scale;
        auto& dl          = frame.Dl;
        auto& input       = frame.Input;
        auto& focused     = frame.Focused;
        auto& st          = frame.St;
        auto& rect        = frame.ElementRect;
        auto& interactive = frame.Interactive;
        auto& mn          = frame.Mn;
        auto& mx          = frame.Mx;
        auto& hot         = frame.Hot;

        auto&       tg    = *tree.GetState<UIToggleData>( e );
        const bool  hover = input && hot;
        const float r     = st.Metric( StyleSlot::ToggleCornerRadius, tg.CornerRadius ) * scale;
        dl.AddRectFilled( mn, mx, Tinted( ctx, glm::vec4( st.Color( StyleSlot::ToggleBox, tg.BoxColor ), 1.0f ) ),
                          r );
        if ( tg.Value )
        {
            const float pad = std::min( rect.W, rect.H ) * 0.22f; // inset "check" fill
            dl.AddRectFilled( { mn.x + pad, mn.y + pad }, { mx.x - pad, mx.y - pad },
                              glm::vec4( st.Color( StyleSlot::ToggleCheck, tg.CheckColor ), 1.0f ), r * 0.5f );
        }
        const bool isFocused = interactive && focused && *focused == e;
        if ( input &&
             ( ( hover && input->MouseReleased ) || ( isFocused && input->Pressed( Common::KeyCode::Enter ) ) ) )
            tg.Value = !tg.Value;
    }
} // namespace Desert::UI::Walk
