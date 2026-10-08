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
    void DrawPanelWidget( ElementFrame& frame )
    {
        auto& ctx   = frame.Ctx;
        auto& reg   = frame.Reg;
        auto& e     = frame.E;
        auto& scale = frame.Scale;
        auto& dl    = frame.Dl;
        auto& st    = frame.St;
        auto& rect  = frame.ElementRect;
        auto& mn    = frame.Mn;
        auto& mx    = frame.Mx;

        const auto& p = reg.get<ECS::UIPanelComponent>( e ).Data;

        // Pulse breathes the whole panel's opacity between PulseMin and full (live dot / CTA glow).
        const float op =
             p.Pulse ? p.Opacity * ( p.PulseMin + ( 1.0f - p.PulseMin ) *
                                                       ( 0.5f + 0.5f * static_cast<float>( std::sin(
                                                                            ctx.View.Time * p.PulseSpeed ) ) ) )
                     : p.Opacity;

        // Resolved once: the corner radius is read by the glow, the shadow and the fill, and a
        // per-site lookup is three chances for two of them to disagree about one rounding.
        const glm::vec3 panelColor  = st.Color( StyleSlot::PanelColor, p.Color );
        const float     cornerPx    = st.Metric( StyleSlot::PanelCornerRadius, p.CornerRadius );
        const float     borderWidth = st.Metric( StyleSlot::PanelBorderWidth, p.BorderWidth );

        if ( p.Glow && p.GlowSize > 0.0f )
        {
            const int   layers = 6;
            const float gs     = p.GlowSize * scale;
            for ( int i = 0; i < layers; ++i ) // large faint -> small; overlap into a soft glow
            {
                const float ex = gs * ( 1.0f - static_cast<float>( i ) / layers );
                dl.AddRectFilled( { mn.x - ex, mn.y - ex }, { mx.x + ex, mx.y + ex },
                                  glm::vec4( st.Color( StyleSlot::PanelGlow, p.GlowColor ), 0.10f * op ),
                                  cornerPx + ex );
            }
        }

        if ( p.Shadow )
            dl.AddRectFilled( { mn.x + p.ShadowOffset.x * scale, mn.y + p.ShadowOffset.y * scale },
                              { mx.x + p.ShadowOffset.x * scale, mx.y + p.ShadowOffset.y * scale },
                              glm::vec4( st.Color( StyleSlot::PanelShadow, p.ShadowColor ), op ),
                              cornerPx * scale );

        // Circle forces full rounding (radius = half the shorter side) for avatars / badges / dots.
        const float rounding = p.Circle ? std::min( rect.W, rect.H ) * 0.5f : cornerPx * scale;

        // A streamed video fills the panel (its stable texture is updated outside the pass by the
        // VideoService); it takes precedence over the sprite/gradient fill while a path is set.
        const TextureRef video = HandleSet( p.Video )
                                      ? ctx.View.Resources().VideoFrame( static_cast<uint64_t>( p.Video ),
                                                                         p.VideoVolume, p.VideoMuted )
                                      : TextureRef{};
        // Frosted glass: the fill IS the blurred scene behind the panel, tinted by Color/Opacity.
        // Checked before the sprite/video fills — a glass panel is defined by what is behind it,
        // so an image on top of it would be a different element (draw one as a child).
        // A UI-domain material IS the fill and is asked first, ahead of glass, video, the
        // gradient and the sprite: those are the fixed list this replaces, and letting one of
        // them win would make the material's presence depend on which other field happened to
        // be set. Resolve() never answers null for a set handle — a slot the UI path cannot
        // execute comes back as the magenta error entry, named once in the log.
        const auto* uiMaterial = ResolveUIMaterial( ctx, e, p.Material );
        if ( uiMaterial )
            dl.AddMaterialRect( uiMaterial, mn, mx, Tinted( ctx, glm::vec4( panelColor, op ) ) );
        else if ( p.BackdropBlur > 0.0f && !video && !HandleSet( p.Sprite ) )
            dl.AddGlassRect( mn, mx, Tinted( ctx, glm::vec4( panelColor, op ) ), rounding, p.BackdropBlur );
        else if ( video )
            dl.AddImage( video.Id, mn, mx, { 0.0f, 0.0f }, { 1.0f, 1.0f },
                         Tinted( ctx, glm::vec4( panelColor, op ) ) );
        else if ( p.UseGradient && !HandleSet( p.Sprite ) )
            dl.AddRectFilledMultiColor( mn, mx, Tinted( ctx, glm::vec4( panelColor, op ) ),
                                        glm::vec4( st.Color( StyleSlot::PanelGradient, p.GradientColor ), op ) );
        else
            DrawBox( ctx.View.Resources(), dl, mn, mx, Tinted( ctx, glm::vec4( panelColor, op ) ), p.Sprite,
                     p.SpriteBorder, scale, rounding );

        // Gradient ring hugging the edge (avatar / status / progress ring).
        if ( p.RingWidth > 0.0f )
        {
            const glm::vec2 c      = ( mn + mx ) * 0.5f;
            const float     outerR = std::min( rect.W, rect.H ) * 0.5f;
            const float     rw     = std::max( 1.0f, p.RingWidth * scale );
            dl.AddRing( c, outerR, outerR - rw, glm::vec4( st.Color( StyleSlot::PanelRingA, p.RingColorA ), 1.0f ),
                        glm::vec4( st.Color( StyleSlot::PanelRingB, p.RingColorB ), 1.0f ) );
        }

        if ( borderWidth > 0.0f )
            dl.AddRect( mn, mx, glm::vec4( st.Color( StyleSlot::PanelBorder, p.BorderColor ), 1.0f ),
                        borderWidth * scale );
    }
} // namespace Desert::UI::Walk
