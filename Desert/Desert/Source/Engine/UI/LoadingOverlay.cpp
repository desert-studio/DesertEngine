#include <Engine/UI/LoadingOverlay.hpp>

#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Runtime/Services/Font/FontService.hpp>
#include <Engine/Text/Utf8.hpp>

#include <algorithm>
#include <cmath>
#include <string>

namespace Desert::UI
{
    void DrawLoadingStrip( Graphic::Render2D::DrawList2D& dl, float width, float height, std::uint32_t frame )
    {
        constexpr float kTrackFraction = 0.34f;
        constexpr float kBlockFraction = 0.18f;
        constexpr float kPeriodFrames  = 48.0f;

        const float trackW = width * kTrackFraction;
        const float blockW = trackW * kBlockFraction;
        const float x0     = ( width - trackW ) * 0.5f;
        const float y0     = height * 0.82f;
        const float thick  = std::max( 2.0f, height * 0.004f );

        dl.AddRectFilled( { x0, y0 }, { x0 + trackW, y0 + thick }, glm::vec4( 1.0f, 1.0f, 1.0f, 0.16f ) );

        // A ping-pong rather than a wrap, so the block is never cut in half at the ends of the track.
        const float phase = std::fmod( static_cast<float>( frame ), kPeriodFrames * 2.0f );
        const float tri   = phase < kPeriodFrames ? phase / kPeriodFrames : 2.0f - phase / kPeriodFrames;
        const float bx    = x0 + tri * ( trackW - blockW );
        dl.AddRectFilled( { bx, y0 }, { bx + blockW, y0 + thick }, glm::vec4( 1.0f, 1.0f, 1.0f, 0.85f ) );
    }

    namespace
    {
        // The label in the project's default UI font, through the same font service and glyph atlas a UI Text
        // element draws with (UICanvasRenderer2D.cpp, the Text branch). Centred on @p centreX, baseline-free:
        // the glyph boxes are placed from the pen as that branch places them. Draws nothing when the service
        // or its default font is not up — the strip below still moves, so the wait is never a still frame.
        void DrawLabel( Graphic::Render2D::DrawList2D& dl, const std::string& text, float centreX, float top,
                        float sizePx )
        {
            auto* fonts = Runtime::ResourceRegistry::GetFontService();
            if ( fonts == nullptr )
                return;
            const std::uint64_t handle = fonts->DefaultFontHandle();
            fonts->RequestGlyphs( handle, Text::Utf8Decode( text ) );
            Runtime::Font* font = fonts->Get( handle, Text::kDefaultBakePixelHeight );
            if ( font == nullptr || !font->Atlas || !font->Baked.Valid() || font->Baked.PixelHeight <= 0.0f )
                return;
            const Text::BakedFont& baked = font->Baked;
            const float            scale = sizePx / baked.PixelHeight;

            float widthPx = 0.0f;
            for ( std::size_t i = 0; i < text.size(); )
                if ( const auto it = baked.Glyphs.find( Text::Utf8Next( text, i ) ); it != baked.Glyphs.end() )
                    widthPx += it->second.Advance * scale;

            float       pen   = centreX - widthPx * 0.5f;
            const float base  = top + sizePx;
            const void* atlas = font->Atlas.get();
            for ( std::size_t i = 0; i < text.size(); )
            {
                const auto it = baked.Glyphs.find( Text::Utf8Next( text, i ) );
                if ( it == baked.Glyphs.end() )
                    continue;
                const Text::Glyph& g = it->second;
                if ( g.Width > 0.0f && g.Height > 0.0f )
                {
                    const float x0 = pen + g.OffsetX * scale;
                    const float y0 = base + g.OffsetY * scale;
                    dl.AddText( atlas, { x0, y0 }, { x0 + g.Width * scale, y0 + g.Height * scale }, { g.U0, g.V0 },
                                { g.U1, g.V1 }, glm::vec4( 1.0f, 1.0f, 1.0f, 0.92f ) );
                }
                pen += g.Advance * scale;
            }
        }
    } // namespace

    void DrawStreamingWaitOverlay( Graphic::Render2D::DrawList2D& dl, float width, float height,
                                   std::uint32_t frame )
    {
        // DIMMED, NOT OPAQUE: unlike a level switch there is no previous level to hide — what is behind is the
        // world the player is in, with its far cells on their HLODs, and the dim says "not playable yet".
        dl.AddRectFilled( { 0.0f, 0.0f }, { width, height }, glm::vec4( 0.0f, 0.0f, 0.0f, 0.72f ) );
        const float sizePx = std::max( 14.0f, height * 0.045f );
        DrawLabel( dl, "Loading…", width * 0.5f, height * 0.82f - sizePx * 2.2f, sizePx );
        DrawLoadingStrip( dl, width, height, frame );
    }
} // namespace Desert::UI
