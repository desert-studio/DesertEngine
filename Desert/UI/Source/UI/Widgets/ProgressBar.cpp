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
    void DrawProgressBarWidget( ElementFrame& frame )
    {
        auto& ctx     = frame.Ctx;
        auto& tree    = frame.Tree;
        auto& e       = frame.E;
        auto& scale   = frame.Scale;
        auto& dl      = frame.Dl;
        auto& st      = frame.St;
        auto& rect    = frame.ElementRect;
        auto& binding = frame.Binding;
        auto& mn      = frame.Mn;
        auto& mx      = frame.Mx;

        UIProgressBarData pb = *tree.Get<UIProgressBarData>( e );
        if ( binding.Value )
            pb.Value = *binding.Value; // bound: the store drives the fill
        const float r = st.Metric( StyleSlot::ProgressCornerRadius, pb.CornerRadius ) * scale;
        dl.AddRectFilled(
             mn, mx, Tinted( ctx, glm::vec4( st.Color( StyleSlot::ProgressBackground, pb.Background ), 1.0f ) ),
             r );
        const float t = std::clamp( pb.Value, 0.0f, 1.0f );
        if ( t > 0.0f )
            dl.AddRectFilled( mn, { mn.x + rect.W * t, mx.y },
                              glm::vec4( st.Color( StyleSlot::ProgressFill, pb.Fill ), 1.0f ), r );
    }
} // namespace Desert::UI::Walk
