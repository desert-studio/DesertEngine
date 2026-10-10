#include <Engine/UI/UIRichText.hpp>

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
    namespace
    {
        // --- Text layout (Phase E: rich text + word-wrap + auto-size + vertical align + overflow) --------
        // Lays a UIText string into SDF glyph quads inside `rect`. `scale` maps design px -> screen px; the SDF
        // stays crisp at any resulting size. Handles the per-element font asset, multi-line + word-wrap,
        // line-spacing, horizontal/vertical alignment, auto-size, overflow (ellipsis/clip) and rich text.

        // One character carrying its resolved rich-text style. `bold` => faux-bold (the glyph is drawn a
        // second time nudged in X, since the atlas has a single weight).
        struct StyledChar
        {
            uint32_t  ch    = 0; // Unicode codepoint (NOT a byte — see Engine/Text/Utf8)
            glm::vec4 color = glm::vec4( 1.0f );
            bool      bold  = false;
        };

        // A laid-out line: its characters plus the total advance width in EM units (baked-pixel space, before
        // the on-screen scale `s` is applied). Kept in em so auto-size can rescale without re-measuring glyphs.
        struct TextLine
        {
            std::vector<StyledChar> chars;
            float                   width = 0.0f;
        };

        // A positioned glyph quad in pixel space — computed once, then drawn for shadow / outline / main so
        // every pass stays in perfect lock-step.
        struct PlacedGlyph
        {
            float     x0, y0, x1, y1;
            float     u0, v0, u1, v1;
            glm::vec4 color;
            bool      bold;
        };

        // "rrggbb" or "rrggbbaa" -> vec4. false (and out untouched) on any malformed input.
        bool ParseHexColor( const std::string& hex, glm::vec4& out )
        {
            auto nib = []( char c ) -> int
            {
                if ( c >= '0' && c <= '9' )
                    return c - '0';
                if ( c >= 'a' && c <= 'f' )
                    return c - 'a' + 10;
                if ( c >= 'A' && c <= 'F' )
                    return c - 'A' + 10;
                return -1;
            };
            if ( hex.size() != 6 && hex.size() != 8 )
                return false;
            // The nibble is stored and THEN tested. Written as an assignment inside the condition it reads
            // as a comparison at a glance, which is the one place a reader must not have to look twice: a
            // `==` slipped in here would parse, compile, and quietly accept every malformed colour.
            int v[8] = { 0 };
            for ( size_t k = 0; k < hex.size(); ++k )
            {
                v[k] = nib( hex[k] );
                if ( v[k] < 0 )
                {
                    return false;
                }
            }
            out.r = ( v[0] * 16 + v[1] ) / 255.0f;
            out.g = ( v[2] * 16 + v[3] ) / 255.0f;
            out.b = ( v[4] * 16 + v[5] ) / 255.0f;
            out.a = hex.size() == 8 ? ( v[6] * 16 + v[7] ) / 255.0f : 1.0f;
            return true;
        }

        // Expand text into styled characters. When `rich`, parse a BBCode subset — [color=#rrggbb]..[/color]
        // (nestable) and [b]..[/b] — consuming the tags; an unrecognised '[' is emitted literally.
        std::vector<StyledChar> BuildStyledChars( const std::string& text, const glm::vec4& baseColor, bool rich )
        {
            std::vector<StyledChar> out;
            out.reserve( text.size() );
            if ( !rich )
            {
                for ( size_t i = 0; i < text.size(); )
                    out.push_back( { Text::Utf8Next( text, i ), baseColor, false } );
                return out;
            }

            std::vector<glm::vec4> colorStack{ baseColor };
            int                    boldDepth = 0;
            for ( size_t i = 0; i < text.size(); )
            {
                if ( text[i] == '[' )
                {
                    const size_t close = text.find( ']', i );
                    if ( close != std::string::npos )
                    {
                        const std::string tag     = text.substr( i + 1, close - i - 1 );
                        bool              handled = true;
                        if ( tag == "b" )
                            ++boldDepth;
                        else if ( tag == "/b" )
                            boldDepth = std::max( 0, boldDepth - 1 );
                        else if ( tag == "/color" )
                        {
                            if ( colorStack.size() > 1 )
                                colorStack.pop_back();
                        }
                        else if ( tag.rfind( "color=#", 0 ) == 0 )
                        {
                            glm::vec4 c = colorStack.back();
                            ParseHexColor( tag.substr( 7 ), c ); // keep parent colour on malformed hex
                            colorStack.push_back( c );
                        }
                        else
                            handled = false;

                        if ( handled )
                        {
                            i = close + 1;
                            continue;
                        }
                    }
                }
                out.push_back( { Text::Utf8Next( text, i ), colorStack.back(), boldDepth > 0 } );
            }
            return out;
        }

        // Greedy word-wrap into lines. `adv(ch)` gives a glyph's em advance; explicit '\n' always breaks.
        // A word longer than maxWidthEm overflows its own line rather than being character-split.
        template <typename AdvFn>
        std::vector<TextLine> LayoutLines( const std::vector<StyledChar>& chars, AdvFn adv, float maxWidthEm,
                                           bool wrap )
        {
            std::vector<TextLine>   lines;
            TextLine                cur;
            std::vector<StyledChar> word;
            float                   wordW = 0.0f;

            auto flushWord = [&]()
            {
                for ( const auto& c : word )
                {
                    cur.chars.push_back( c );
                    cur.width += adv( c.ch );
                }
                word.clear();
                wordW = 0.0f;
            };
            auto trimTrailingSpaces = [&]()
            {
                while ( !cur.chars.empty() && cur.chars.back().ch == ' ' )
                {
                    cur.width -= adv( ' ' );
                    cur.chars.pop_back();
                }
            };
            auto pushLine = [&]()
            {
                lines.push_back( std::move( cur ) );
                cur = TextLine{};
            };

            for ( const StyledChar& c : chars )
            {
                if ( c.ch == '\n' )
                {
                    flushWord();
                    pushLine();
                    continue;
                }
                if ( c.ch == ' ' )
                {
                    flushWord();
                    cur.chars.push_back( c );
                    cur.width += adv( ' ' );
                    continue;
                }
                word.push_back( c );
                wordW += adv( c.ch );
                if ( wrap && !cur.chars.empty() && ( cur.width + wordW ) > maxWidthEm )
                {
                    trimTrailingSpaces();
                    pushLine(); // the pending word carries over and starts the fresh line
                }
            }
            flushWord();
            trimTrailingSpaces();
            lines.push_back( std::move( cur ) );
            return lines;
        }

    } // namespace

    // @p tint is the caller's accumulated element tint (UICanvasContext::Tint), passed in rather than
    // read from a global so this helper stays a pure function of its arguments.
    // @p viewSeconds is the view's UI time (UIViewContext::Time) — the marquee's phase, so it scrolls
    // by frame steps the host handed in rather than by a wall clock, and frame N of a fixed-step run
    // draws the same scroll every run.
    void DrawText2D( IUICanvasResources& res, Graphic::Render2D::DrawList2D& dl, const UITextData& t,
                     const Rect& rect, float scale, const glm::vec4& tint, double viewSeconds )
    {
        if ( t.Text.empty() )
            return;

        // Font is an asset handle on the element (drag-drop / preloaded); unset falls back to the default.
        const uint64_t fontHandle =
             static_cast<uint64_t>( t.Font ) != 0 ? static_cast<uint64_t>( t.Font ) : res.DefaultFontHandle();
        // Non-ASCII (Cyrillic, CJK, …) is only in the atlas if it was asked for: request this string's
        // codepoints first, so a re-bake — if any — happens before the font is resolved and the text
        // draws correctly on its very first frame instead of a frame late.
        res.RequestGlyphs( fontHandle, Text::Utf8Decode( t.Text ) );

        const FontFace font = res.Font( fontHandle, Text::kDefaultBakePixelHeight );
        if ( font.Baked == nullptr || font.Atlas == nullptr || !font.Baked->Valid() ||
             font.Baked->PixelHeight <= 0.0f )
            return;

        const Text::BakedFont& bf    = *font.Baked;
        const void*            atlas = font.Atlas;

        auto glyph = [&]( uint32_t ch ) -> const Text::Glyph*
        {
            const auto it = bf.Glyphs.find( ch );
            return it == bf.Glyphs.end() ? nullptr : &it->second;
        };
        auto advEm = [&]( uint32_t ch ) -> float
        {
            const Text::Glyph* g = glyph( ch );
            return g ? g->Advance : 0.0f;
        };

        const std::vector<StyledChar> chars =
             BuildStyledChars( t.Text, glm::vec4( t.Color, 1.0f ) * tint, t.RichText );

        // Marquee: a single clipped line scrolling leftward, repeated seamlessly across the width. A news
        // ticker / running banner. Pure function of the shared clock, so it needs no per-frame state.
        if ( t.Marquee )
        {
            const float sM       = ( t.FontSize * scale ) / bf.PixelHeight;
            float       contentW = 0.0f;
            for ( const StyledChar& sc : chars )
                contentW += advEm( sc.ch ) * sM;
            const float gap    = std::max( 40.0f * scale, rect.W * 0.35f );
            const float period = std::max( 1.0f, contentW + gap );
            const auto  off    = static_cast<float>( std::fmod( viewSeconds * t.MarqueeSpeed * scale, period ) );
            const float blockH = ( bf.Ascent - bf.Descent ) * sM;
            const float baseY  = rect.Y + ( rect.H - blockH ) * 0.5f + bf.Ascent * sM;

            dl.PushClipRect( { rect.X, rect.Y }, { rect.X + rect.W, rect.Y + rect.H } );
            for ( float startX = rect.X - off; startX < rect.X + rect.W; startX += period )
            {
                float penX = startX;
                for ( const StyledChar& sc : chars )
                {
                    const Text::Glyph* g = glyph( sc.ch );
                    if ( !g )
                        continue;
                    if ( g->Width > 0.0f && g->Height > 0.0f )
                    {
                        const float x0 = penX + g->OffsetX * sM;
                        const float y0 = baseY + g->OffsetY * sM;
                        dl.AddText( atlas, { x0, y0 }, { x0 + g->Width * sM, y0 + g->Height * sM },
                                    { g->U0, g->V0 }, { g->U1, g->V1 }, sc.color );
                    }
                    penX += g->Advance * sM;
                }
            }
            dl.PopClipRect();
            return;
        }

        const float pad    = 6.0f;
        const float availW = std::max( 1.0f, rect.W - 2.0f * pad );
        const float availH = std::max( 1.0f, rect.H - 2.0f * pad );
        // Em-space vertical advance per line: the font's line box times the user's line-spacing multiplier.
        const float lineHemEm = ( bf.Ascent - bf.Descent ) * std::max( 0.1f, t.LineSpacing );

        auto layoutAt = [&]( float s ) -> std::vector<TextLine>
        {
            const float threshold = t.Wrap ? availW / s : std::numeric_limits<float>::max();
            return LayoutLines( chars, advEm, threshold, t.Wrap );
        };

        float                 s     = ( t.FontSize * scale ) / bf.PixelHeight;
        std::vector<TextLine> lines = layoutAt( s );

        if ( t.AutoSize )
        {
            // Shrink toward the floor until the block fits height (and width when not wrapping).
            const float minS = std::max( 0.01f, ( t.MinFontSize * scale ) / bf.PixelHeight );
            for ( int iter = 0; iter < 24; ++iter )
            {
                float maxLineEm = 0.0f;
                for ( const auto& ln : lines )
                    maxLineEm = std::max( maxLineEm, ln.width );
                const float blockH = lines.empty() ? 0.0f
                                                   : ( static_cast<float>( lines.size() - 1 ) * lineHemEm * s +
                                                       ( bf.Ascent - bf.Descent ) * s );
                const bool  fitsH  = blockH <= availH;
                const bool  fitsW  = t.Wrap || ( maxLineEm * s <= availW );
                if ( ( fitsH && fitsW ) || s <= minS )
                    break;
                s     = std::max( minS, s * 0.92f );
                lines = layoutAt( s );
            }
        }

        const float lineStep = lineHemEm * s;
        if ( lineStep <= 0.0f )
            return;

        // Overflow: drop the lines that fall past the bottom (Ellipsis/Clip), tagging the tail as truncated.
        bool truncated = false;
        if ( t.Overflow != UITextOverflow::Overflow )
        {
            const size_t maxVisible =
                 std::max<size_t>( 1, static_cast<size_t>( std::floor( availH / lineStep ) ) );
            if ( lines.size() > maxVisible )
            {
                lines.resize( maxVisible );
                truncated = ( t.Overflow == UITextOverflow::Ellipsis );
            }
        }

        // Ellipsis: trim trailing glyphs off any over-wide line (and the truncated tail) and append "...".
        if ( t.Overflow == UITextOverflow::Ellipsis )
        {
            const float dotAdv    = advEm( '.' );
            auto        ellipsize = [&]( TextLine& ln )
            {
                const float dots = 3.0f * dotAdv;
                while ( !ln.chars.empty() && ( ln.width + dots ) * s > availW )
                {
                    ln.width -= advEm( ln.chars.back().ch );
                    ln.chars.pop_back();
                }
                const glm::vec4 col = ln.chars.empty() ? glm::vec4( t.Color, 1.0f ) : ln.chars.back().color;
                for ( int k = 0; k < 3; ++k )
                {
                    ln.chars.push_back( { '.', col, false } );
                    ln.width += dotAdv;
                }
            };
            if ( dotAdv > 0.0f )
            {
                if ( !t.Wrap )
                    for ( auto& ln : lines )
                        if ( ln.width * s > availW )
                            ellipsize( ln );
                if ( truncated && !lines.empty() )
                    ellipsize( lines.back() );
            }
        }

        // Vertical placement of the whole block within the rect.
        const float blockH =
             lines.empty()
                  ? 0.0f
                  : ( static_cast<float>( lines.size() - 1 ) * lineStep + ( bf.Ascent - bf.Descent ) * s );
        float blockTop = rect.Y + pad; // Top
        if ( t.VerticalAlign == UITextVAlign::Middle )
            blockTop = rect.Y + ( rect.H - blockH ) * 0.5f;
        else if ( t.VerticalAlign == UITextVAlign::Bottom )
            blockTop = rect.Y + rect.H - pad - blockH;

        // Position every glyph (per-line horizontal alignment), collecting quads for the draw passes.
        std::vector<PlacedGlyph> placed;
        for ( size_t i = 0; i < lines.size(); ++i )
        {
            const float lineWpx = lines[i].width * s;
            float       penX    = rect.X + pad; // Left
            if ( t.Align == UITextAlign::Center )
                penX = rect.X + ( rect.W - lineWpx ) * 0.5f;
            else if ( t.Align == UITextAlign::Right )
                penX = rect.X + rect.W - pad - lineWpx;
            const float baselineY = blockTop + static_cast<float>( i ) * lineStep + bf.Ascent * s;

            for ( const StyledChar& sc : lines[i].chars )
            {
                const Text::Glyph* g = glyph( sc.ch );
                if ( !g )
                    continue;
                if ( g->Width > 0.0f && g->Height > 0.0f )
                {
                    // OffsetY is the glyph top relative to the baseline, Y-down (negative above baseline).
                    const float x0 = penX + g->OffsetX * s;
                    const float y0 = baselineY + g->OffsetY * s;
                    placed.push_back( { x0, y0, x0 + g->Width * s, y0 + g->Height * s, g->U0, g->V0, g->U1, g->V1,
                                        glm::vec4( glm::vec3( sc.color ), sc.color.a ), sc.bold } );
                }
                penX += g->Advance * s;
            }
        }

        const bool clip = ( t.Overflow == UITextOverflow::Clip );
        if ( clip )
            dl.PushClipRect( { rect.X, rect.Y }, { rect.X + rect.W, rect.Y + rect.H } );

        auto quad = [&]( const PlacedGlyph& g, const glm::vec2& off, const glm::vec4& col )
        {
            dl.AddText( atlas, { g.x0 + off.x, g.y0 + off.y }, { g.x1 + off.x, g.y1 + off.y }, { g.u0, g.v0 },
                        { g.u1, g.v1 }, col );
        };

        if ( t.Glow && t.GlowRadius > 0.0f && t.GlowStrength > 0.0f )
        {
            // Rings of the glyphs at growing radii, each fainter: the summed coverage falls off with
            // distance from the glyph edge, which is a halo. Twelve directions keep the ring round at
            // the radii a title uses; three rings make the falloff a slope rather than a step.
            constexpr int kDirections = 12;
            constexpr int kRings      = 3;
            const float   radius      = t.GlowRadius * scale;
            for ( int ring = kRings; ring >= 1; --ring )
            {
                const float r = radius * static_cast<float>( ring ) / kRings;
                const float a = t.GlowStrength * ( 1.0f - static_cast<float>( ring - 1 ) / kRings ) /
                                static_cast<float>( kDirections ) * 2.0f;
                const glm::vec4 gc( t.GlowColor, std::clamp( a, 0.0f, 1.0f ) );
                for ( int k = 0; k < kDirections; ++k )
                {
                    const float     ang = 6.28318530718f * static_cast<float>( k ) / kDirections;
                    const glm::vec2 off = { std::cos( ang ) * r, std::sin( ang ) * r };
                    for ( const auto& g : placed )
                        quad( g, off, gc );
                }
            }
        }
        if ( t.Shadow )
        {
            const glm::vec4 sc = glm::vec4( t.ShadowColor, 1.0f );
            const glm::vec2 so = t.ShadowOffset * scale;
            for ( const auto& g : placed )
                quad( g, so, sc );
        }
        if ( t.Outline )
        {
            const glm::vec4 oc( t.OutlineColor, 1.0f );
            const float     ow = std::max( 1.0f, scale );
            for ( int ox = -1; ox <= 1; ++ox )
                for ( int oy = -1; oy <= 1; ++oy )
                    if ( ox != 0 || oy != 0 )
                        for ( const auto& g : placed )
                            quad( g, { ox * ow, oy * ow }, oc );
        }
        for ( const auto& g : placed )
        {
            quad( g, { 0.0f, 0.0f }, g.color );
            if ( g.bold ) // faux-bold: a second pass nudged in X thickens the stroke
                quad( g, { std::max( 0.5f, 0.6f * scale ), 0.0f }, g.color );
        }

        if ( clip )
            dl.PopClipRect();
    }

    // Width in px of `text` at `fontSizePx` in the default font (for the input caret). 0 if no font.
    float MeasureTextPx( IUICanvasResources& res, const std::string& text, float fontSizePx )
    {
        const FontFace font = res.Font( res.DefaultFontHandle(), Text::kDefaultBakePixelHeight );
        if ( font.Baked == nullptr || !font.Baked->Valid() )
            return 0.0f;
        const Text::BakedFont& bf = *font.Baked;
        const float            s  = bf.PixelHeight > 0.0f ? fontSizePx / bf.PixelHeight : 0.0f;
        float                  w  = 0.0f;
        for ( size_t i = 0; i < text.size(); )
        {
            const auto it = bf.Glyphs.find( Text::Utf8Next( text, i ) );
            if ( it != bf.Glyphs.end() )
                w += it->second.Advance * s;
        }
        return w;
    }
} // namespace Desert::UI::Walk
