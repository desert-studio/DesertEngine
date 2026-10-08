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
    void DrawPathWidget( ElementFrame& frame )
    {
        auto& ctx   = frame.Ctx;
        auto& reg   = frame.Reg;
        auto& e     = frame.E;
        auto& scale = frame.Scale;
        auto& dl    = frame.Dl;
        auto& mn    = frame.Mn;
        auto& mx    = frame.Mx;

        const UIPathData& path        = reg.get<ECS::UIPathComponent>( e ).Data;
        const glm::vec2 slots[8]    = { path.P0, path.P1, path.P2, path.P3, path.P4, path.P5, path.P6, path.P7 };
        const int       count       = std::clamp( path.PointCount, 2, 8 );

        // Points are fractions of the element's own rect, so the line follows its anchors.
        std::array<glm::vec2, 8> control{};
        for ( int i = 0; i < count; ++i )
            control[static_cast<size_t>( i )] = mn + slots[i] * ( mx - mn );

        const UIPathPolyline line =
             TessellateUIPath( std::span<const glm::vec2>( control.data(), static_cast<size_t>( count ) ),
                               path.Curve == UIPathCurve::Smooth );

        // A keyed clip REPLACES the authored Reveal while it drives it (never written back).
        float      reveal = path.Reveal;
        const auto clip   = ctx.View.AnimClips.Samples.find( e );
        if ( clip != ctx.View.AnimClips.Samples.end() )
            reveal = clip->second.Reveal.value_or( reveal );

        const std::vector<glm::vec2> shown = RevealUIPath( line, reveal );
        if ( shown.size() >= 2 )
        {
            const float thick = path.Thickness * scale;
            if ( path.Glow && path.GlowRadius > 0.0f && path.GlowStrength > 0.0f )
                dl.AddPolyline( shown.data(), static_cast<uint32_t>( shown.size() ),
                                Tinted( ctx, glm::vec4( path.GlowColor, path.GlowStrength ) ), thick,
                                path.GlowRadius * scale, path.RoundCaps );
            dl.AddPolyline( shown.data(), static_cast<uint32_t>( shown.size() ),
                            Tinted( ctx, glm::vec4( path.Color, path.Opacity ) ), thick, path.Feather,
                            path.RoundCaps );
        }
    }
} // namespace Desert::UI::Walk
