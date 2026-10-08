#include <Engine/UI/Widgets/UIWidgets.hpp>

#include <Engine/UI/UICanvasRenderer2D.hpp>
#include <Engine/UI/UIOverlay.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/Assets/Common.hpp>
#include <Engine/Graphic/Texture.hpp>
#include <Engine/Graphic/Image.hpp>
#include <Engine/Runtime/Services/Font/FontService.hpp>
#include <Engine/Runtime/Services/Icon/IconService.hpp>
#include <Engine/Localization/LocalizationService.hpp>
#include <Engine/Text/FontBaker.hpp>
#include <Engine/UI/UIStyleResolver.hpp>
#include <Engine/Runtime/Services/UITheme/UIThemeService.hpp>
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
    void DrawProgressBarWidget( ElementFrame& frame )
    {
        auto& ctx     = frame.Ctx;
        auto& reg     = frame.Reg;
        auto& e       = frame.E;
        auto& scale   = frame.Scale;
        auto& dl      = frame.Dl;
        auto& st      = frame.St;
        auto& rect    = frame.ElementRect;
        auto& binding = frame.Binding;
        auto& mn      = frame.Mn;
        auto& mx      = frame.Mx;

        UIProgressBarData pb = reg.get<ECS::UIProgressBarComponent>( e ).Data;
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
