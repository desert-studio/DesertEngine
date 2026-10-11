#include <UI/UIRichText.hpp>

#include <UI/UICanvasRenderer2D.hpp>
#include <UI/UIOverlay.hpp>
#include <UI/Args/UIArgs.hpp>
#include <Common/Core/AssetHandle.hpp>
#include <Render2DCore/Text/BakedFont.hpp>
#include <UI/UIStyleResolver.hpp>
#include <Render2DCore/Text/Utf8.hpp>
#include <UI/UICanvasLayout.hpp>
#include <UI/UIDataStore.hpp>
#include <UI/UIPathGeometry.hpp>

#include <Common/Core/Logger.hpp>

#include <algorithm>
#include <span>
#include <unordered_set>
#include <array>
#include <optional>
#include <cstdint>
#include <charconv>
#include <cmath>
#include <cstdlib>
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

        // What one laid-out item is: a glyph of the font, or an inline picture riding in the line the way
        // UE's URichTextBlock decorators put one there (RichTextBlockImageDecorator.h:39) — [icon=] an .svg
        // vector icon, [img=] a sprite. A picture is an ordinary item to the layout: it has an advance,
        // it wraps with its word, it takes the line's size.
        enum class InlineKind : uint8_t
        {
            Glyph,
            Icon,
            Image,
        };

        // One item carrying its resolved rich-text style. `bold` => faux-bold (the glyph is drawn a second
        // time nudged in X, since the atlas has a single weight); `italic` => the glyph quad is sheared
        // about its baseline (a render transform, since the atlas has a single slant too). `size` is the
        // item's font size as a RATIO of the element's FontSize, so Auto Size rescales a mixed-size block
        // as one. `link` indexes the block's link messages ([link=msg]), -1 = not a link.
        struct StyledChar
        {
            uint32_t   ch     = 0; // Unicode codepoint (NOT a byte — see Engine/Text/Utf8); U+FFFC for a picture
            glm::vec4  color  = glm::vec4( 1.0f );
            bool       bold   = false;
            bool       italic = false;
            float      size   = 1.0f;
            int        link   = -1;
            InlineKind kind   = InlineKind::Glyph;
            uint64_t   asset  = 0; // the icon / sprite handle of a picture
        };

        // A laid-out line: its items plus the total advance width in EM units (baked-pixel space, before
        // the on-screen scale `s` is applied). Kept in em so auto-size can rescale without re-measuring
        // glyphs. `size` is the largest item ratio on the line: the line box grows to its tallest item.
        struct TextLine
        {
            std::vector<StyledChar> chars;
            float                   width = 0.0f;
            float                   size  = 1.0f;
        };

        // A positioned glyph quad in pixel space — computed once, then drawn for shadow / outline / main so
        // every pass stays in perfect lock-step. `baseline` is what an italic glyph is sheared about.
        struct PlacedGlyph
        {
            float     x0, y0, x1, y1;
            float     u0, v0, u1, v1;
            glm::vec4 color;
            bool      bold;
            bool      italic;
            float     baseline;
        };

        // A positioned inline picture — drawn in the main pass only (shadow, outline and glow are text
        // effects, as in UE where a decorator's widget does not inherit the text style's shadow).
        struct PlacedInline
        {
            Rect       box;
            InlineKind kind;
            uint64_t   asset;
            glm::vec4  color;
        };

        // One link run on one line, in pixels — the underline and the hit box of [link=msg].
        struct PlacedLink
        {
            int   link;
            float x0, x1, top, bottom, baseline;
        };

        // The slant of [i]: horizontal shift per pixel of height above the baseline (≈ 12°, the usual
        // synthetic oblique).
        constexpr float kItalicShear = 0.21f;

        // Shear about `baseline`, y-down: a point above the baseline (smaller y) moves right.
        glm::mat3 ItalicShear( float baseline )
        {
            glm::mat3 m( 1.0f );
            m[1][0] = -kItalicShear;
            m[2][0] = kItalicShear * baseline;
            return m;
        }

        // A tag's numeric argument: the whole string must be the number, or the tag is not a tag.
        bool ParseFloatArg( const std::string& s, float& out )
        {
            if ( s.empty() )
                return false;
            char*       end = nullptr;
            const float v   = std::strtof( s.c_str(), &end );
            if ( end != s.c_str() + s.size() || !std::isfinite( v ) || v <= 0.0f )
                return false;
            out = v;
            return true;
        }

        bool ParseHandleArg( const std::string& s, uint64_t& out )
        {
            uint64_t v           = 0;
            const auto [ptr, ec] = std::from_chars( s.data(), s.data() + s.size(), v );
            if ( ec != std::errc{} || ptr != s.data() + s.size() || v == 0 )
                return false;
            out = v;
            return true;
        }

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

        // Expand text into styled items. When `rich`, parse the BBCode subset — [color=#rrggbb]..[/color],
        // [b]..[/b], [i]..[/i], [size=N]..[/size] (N in design px, like Font Size), [link=msg]..[/link]
        // (all nestable), and the self-closing pictures [icon=H] / [img=H] (H = the asset handle) —
        // consuming the tags; an unrecognised or malformed '[' is emitted literally. Link messages land in
        // `links`, which the items index.
        std::vector<StyledChar> BuildStyledChars( const std::string& text, const glm::vec4& baseColor,
                                                  float baseSize, bool rich, std::vector<std::string>& links )
        {
            std::vector<StyledChar> out;
            out.reserve( text.size() );
            if ( !rich )
            {
                for ( size_t i = 0; i < text.size(); )
                    out.push_back( { Text::Utf8Next( text, i ), baseColor } );
                return out;
            }

            std::vector<glm::vec4> colorStack{ baseColor };
            std::vector<float>     sizeStack{ 1.0f };
            std::vector<int>       linkStack{ -1 };
            int                    boldDepth   = 0;
            int                    italicDepth = 0;
            auto                   styled      = [&]( uint32_t ch )
            {
                StyledChar c;
                c.ch     = ch;
                c.color  = colorStack.back();
                c.bold   = boldDepth > 0;
                c.italic = italicDepth > 0;
                c.size   = sizeStack.back();
                c.link   = linkStack.back();
                return c;
            };
            for ( size_t i = 0; i < text.size(); )
            {
                if ( text[i] == '[' )
                {
                    const size_t close = text.find( ']', i );
                    if ( close != std::string::npos )
                    {
                        const std::string tag     = text.substr( i + 1, close - i - 1 );
                        bool              handled = true;
                        float             size    = 0.0f;
                        uint64_t          asset   = 0;
                        if ( tag == "b" )
                            ++boldDepth;
                        else if ( tag == "/b" )
                            boldDepth = std::max( 0, boldDepth - 1 );
                        else if ( tag == "i" )
                            ++italicDepth;
                        else if ( tag == "/i" )
                            italicDepth = std::max( 0, italicDepth - 1 );
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
                        else if ( tag == "/size" )
                        {
                            if ( sizeStack.size() > 1 )
                                sizeStack.pop_back();
                        }
                        else if ( tag.rfind( "size=", 0 ) == 0 && baseSize > 0.0f &&
                                  ParseFloatArg( tag.substr( 5 ), size ) )
                            sizeStack.push_back( size / baseSize );
                        else if ( tag == "/link" )
                        {
                            if ( linkStack.size() > 1 )
                                linkStack.pop_back();
                        }
                        else if ( tag.rfind( "link=", 0 ) == 0 && tag.size() > 5 )
                        {
                            links.push_back( tag.substr( 5 ) );
                            linkStack.push_back( static_cast<int>( links.size() ) - 1 );
                        }
                        else if ( tag.rfind( "icon=", 0 ) == 0 && ParseHandleArg( tag.substr( 5 ), asset ) )
                        {
                            StyledChar c = styled( 0xFFFC );
                            c.kind       = InlineKind::Icon;
                            c.asset      = asset;
                            out.push_back( c );
                        }
                        else if ( tag.rfind( "img=", 0 ) == 0 && ParseHandleArg( tag.substr( 4 ), asset ) )
                        {
                            StyledChar c = styled( 0xFFFC );
                            c.kind       = InlineKind::Image;
                            c.asset      = asset;
                            out.push_back( c );
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
                out.push_back( styled( Text::Utf8Next( text, i ) ) );
            }
            return out;
        }

        // Greedy word-wrap into lines. `adv(item)` gives an item's em advance; explicit '\n' always breaks.
        // A word longer than maxWidthEm overflows its own line rather than being character-split. Each
        // line records its largest item size, which its line box is sized by.
        template <typename AdvFn>
        std::vector<TextLine> LayoutLines( const std::vector<StyledChar>& chars, AdvFn adv, float maxWidthEm,
                                           bool wrap )
        {
            std::vector<TextLine>   lines;
            TextLine                cur;
            std::vector<StyledChar> word;
            float                   wordW = 0.0f;
            cur.size                      = 0.0f;

            auto flushWord = [&]()
            {
                for ( const auto& c : word )
                {
                    cur.chars.push_back( c );
                    cur.width += adv( c );
                    cur.size = std::max( cur.size, c.size );
                }
                word.clear();
                wordW = 0.0f;
            };
            auto trimTrailingSpaces = [&]()
            {
                while ( !cur.chars.empty() && cur.chars.back().ch == ' ' )
                {
                    cur.width -= adv( cur.chars.back() );
                    cur.chars.pop_back();
                }
            };
            auto pushLine = [&]()
            {
                if ( cur.size <= 0.0f )
                    cur.size = 1.0f;
                lines.push_back( std::move( cur ) );
                cur      = TextLine{};
                cur.size = 0.0f;
            };

            for ( const StyledChar& c : chars )
            {
                if ( c.ch == '\n' && c.kind == InlineKind::Glyph )
                {
                    flushWord();
                    cur.size = std::max( cur.size, c.size );
                    pushLine();
                    continue;
                }
                if ( c.ch == ' ' && c.kind == InlineKind::Glyph )
                {
                    flushWord();
                    cur.chars.push_back( c );
                    cur.width += adv( c );
                    cur.size = std::max( cur.size, c.size );
                    continue;
                }
                word.push_back( c );
                wordW += adv( c );
                if ( wrap && !cur.chars.empty() && ( cur.width + wordW ) > maxWidthEm )
                {
                    trimTrailingSpaces();
                    pushLine(); // the pending word carries over and starts the fresh line
                }
            }
            flushWord();
            trimTrailingSpaces();
            pushLine();
            return lines;
        }

    } // namespace

    // @p tint is the caller's accumulated element tint (UICanvasContext::Tint), passed in rather than
    // read from a global so this helper stays a pure function of its arguments.
    // @p viewSeconds is the view's UI time (UIViewContext::Time) — the marquee's phase, so it scrolls
    // by frame steps the host handed in rather than by a wall clock, and frame N of a fixed-step run
    // draws the same scroll every run.
    void DrawText2D( IUICanvasResources& res, Graphic::Render2D::DrawList2D& dl, const UITextData& t,
                     const Rect& rect, float scale, const glm::vec4& tint, double viewSeconds,
                     RichTextLinks* links )
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

        std::vector<std::string>      linkMessages;
        const std::vector<StyledChar> chars =
             BuildStyledChars( t.Text, glm::vec4( t.Color, 1.0f ) * tint, t.FontSize, t.RichText, linkMessages );

        // The font's line box in em; an item of size ratio r stands r times as tall.
        const float lineBoxEm = bf.Ascent - bf.Descent;

        // A picture is as tall as its line box at its own size and as wide as its source's aspect says —
        // the image decorator's default (RichTextBlockImageDecorator.h:39 sizes the brush to the font's
        // height). Asked once per handle per block: the layout measures every item more than once.
        std::unordered_map<uint64_t, float> aspects;
        auto                                pictureAspect = [&]( const StyledChar& c ) -> float
        {
            const auto [it, fresh] = aspects.try_emplace( c.asset, 1.0f );
            if ( fresh )
            {
                if ( c.kind == InlineKind::Icon )
                {
                    const IconRef icon = res.Icon( c.asset );
                    if ( !icon.Layers.empty() && icon.Atlas != nullptr && icon.Aspect > 0.0f )
                        it->second = icon.Aspect;
                }
                else if ( const TextureRef tex = ResolveSpriteImage( res, Common::AssetHandle( c.asset ) );
                          tex && tex.Width > 0 && tex.Height > 0 )
                    it->second = static_cast<float>( tex.Width ) / static_cast<float>( tex.Height );
            }
            return it->second;
        };
        // An item's advance in em at the element's base size.
        auto adv = [&]( const StyledChar& c ) -> float
        {
            if ( c.kind != InlineKind::Glyph )
                return lineBoxEm * c.size * pictureAspect( c );
            const Text::Glyph* g = glyph( c.ch );
            return g ? g->Advance * c.size : 0.0f;
        };

        // --- Placement: every item becomes a glyph quad, a picture box or a stretch of a link run --------
        std::vector<PlacedGlyph>  placed;
        std::vector<PlacedInline> pictures;
        std::vector<PlacedLink>   linkRuns;
        std::vector<glm::vec4>    linkColors;

        // Place one item with its pen at `penX` on the line's `baselineY`, at on-screen scale `s`; returns
        // its advance in px.
        auto place = [&]( const StyledChar& c, float penX, float baselineY, float s ) -> float
        {
            const float k       = s * c.size;
            const float top     = baselineY - bf.Ascent * k;
            const float bottom  = baselineY - bf.Descent * k;
            float       advance = 0.0f;
            if ( c.kind != InlineKind::Glyph )
            {
                advance = lineBoxEm * k * pictureAspect( c );
                pictures.push_back( { Rect{ penX, top, advance, bottom - top }, c.kind, c.asset, c.color } );
            }
            else
            {
                const Text::Glyph* g = glyph( c.ch );
                if ( !g )
                    return 0.0f;
                if ( g->Width > 0.0f && g->Height > 0.0f )
                {
                    // OffsetY is the glyph top relative to the baseline, Y-down (negative above baseline).
                    const float x0 = penX + g->OffsetX * k;
                    const float y0 = baselineY + g->OffsetY * k;
                    placed.push_back( { x0, y0, x0 + g->Width * k, y0 + g->Height * k, g->U0, g->V0, g->U1, g->V1,
                                        c.color, c.bold, c.italic, baselineY } );
                }
                advance = g->Advance * k;
            }
            if ( c.link >= 0 )
            {
                PlacedLink* run = linkRuns.empty() ? nullptr : &linkRuns.back();
                if ( run && run->link == c.link && run->baseline == baselineY &&
                     std::abs( run->x1 - penX ) < 0.5f )
                {
                    run->x1     = penX + advance;
                    run->top    = std::min( run->top, top );
                    run->bottom = std::max( run->bottom, bottom );
                }
                else
                {
                    linkRuns.push_back( { c.link, penX, penX + advance, top, bottom, baselineY } );
                    linkColors.push_back( c.color );
                }
            }
            return advance;
        };

        // --- Drawing: glow, shadow, outline, then the text itself, its pictures and its link underlines --
        auto drawPlaced = [&]()
        {
            // An italic glyph is drawn under a shear about its own baseline, so every pass (shadow and
            // outline included) slants the same way and stays in lock-step with the main one.
            auto quad = [&]( const PlacedGlyph& g, const glm::vec2& off, const glm::vec4& col )
            {
                if ( g.italic )
                    dl.PushTransform( ItalicShear( g.baseline + off.y ) );
                dl.AddText( atlas, { g.x0 + off.x, g.y0 + off.y }, { g.x1 + off.x, g.y1 + off.y }, { g.u0, g.v0 },
                            { g.u1, g.v1 }, col );
                if ( g.italic )
                    dl.PopTransform();
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

            // An icon takes the text colour at its place (a monochrome icon is one white layer, so
            // [color] tints it); a sprite is a picture and keeps its own colours — only the element's
            // tint reaches it.
            for ( const PlacedInline& p : pictures )
            {
                if ( p.kind == InlineKind::Icon )
                {
                    UIIconData icon;
                    icon.Icon  = Common::AssetHandle( p.asset );
                    icon.Color = glm::vec3( p.color );
                    icon.Scale = 1.0f;
                    DrawIcon( res, dl, icon, p.box, glm::vec4( 1.0f, 1.0f, 1.0f, p.color.a ) );
                }
                else
                    DrawBox( res, dl, { p.box.X, p.box.Y }, { p.box.X + p.box.W, p.box.Y + p.box.H }, tint,
                             Common::AssetHandle( p.asset ), glm::vec4( 0.0f ), scale, 0.0f );
            }

            // A link is underlined in its own colour; the one under the pointer gets a heavier line and a
            // faint wash, the hover state of UE's hyperlink decorator.
            for ( size_t i = 0; i < linkRuns.size(); ++i )
            {
                const PlacedLink& run = linkRuns[i];
                const bool        hovered =
                     links && links->Pointer &&
                     PointIn( Rect{ run.x0, run.top, run.x1 - run.x0, run.bottom - run.top }, *links->Pointer );
                const float     thick = std::max( 1.0f, scale ) * ( hovered ? 2.0f : 1.0f );
                const float     y     = run.baseline + std::max( 1.0f, scale );
                const glm::vec4 col   = linkColors[i];
                dl.AddRectFilled( { run.x0, y }, { run.x1, y + thick }, col, 0.0f );
                if ( hovered )
                    dl.AddRectFilled( { run.x0, run.top }, { run.x1, run.bottom },
                                      glm::vec4( glm::vec3( col ), 0.15f * col.a ), 0.0f );
            }
            if ( links )
                for ( const PlacedLink& run : linkRuns )
                    links->Hits.push_back( { Rect{ run.x0, run.top, run.x1 - run.x0, run.bottom - run.top },
                                             linkMessages[static_cast<size_t>( run.link )] } );
        };

        // Marquee: a single clipped line scrolling leftward, repeated seamlessly across the width. A news
        // ticker / running banner. Pure function of the shared clock, so it needs no per-frame state.
        if ( t.Marquee )
        {
            const float sM       = ( t.FontSize * scale ) / bf.PixelHeight;
            float       contentW = 0.0f;
            float       lineSize = 0.0f;
            for ( const StyledChar& sc : chars )
            {
                contentW += adv( sc ) * sM;
                lineSize = std::max( lineSize, sc.size );
            }
            lineSize           = lineSize > 0.0f ? lineSize : 1.0f;
            const float gap    = std::max( 40.0f * scale, rect.W * 0.35f );
            const float period = std::max( 1.0f, contentW + gap );
            const auto  off    = static_cast<float>( std::fmod( viewSeconds * t.MarqueeSpeed * scale, period ) );
            const float blockH = lineBoxEm * lineSize * sM;
            const float baseY  = rect.Y + ( rect.H - blockH ) * 0.5f + bf.Ascent * lineSize * sM;

            for ( float startX = rect.X - off; startX < rect.X + rect.W; startX += period )
            {
                float penX = startX;
                for ( const StyledChar& sc : chars )
                    penX += place( sc, penX, baseY, sM );
            }
            dl.PushClipRect( { rect.X, rect.Y }, { rect.X + rect.W, rect.Y + rect.H } );
            drawPlaced();
            dl.PopClipRect();
            return;
        }

        const float pad     = 6.0f;
        const float availW  = std::max( 1.0f, rect.W - 2.0f * pad );
        const float availH  = std::max( 1.0f, rect.H - 2.0f * pad );
        const float spacing = std::max( 0.1f, t.LineSpacing );
        // A line's vertical advance in px: its line box (sized by its tallest item) times the user's
        // line-spacing multiplier.
        auto lineStepPx = [&]( const TextLine& ln, float s ) { return lineBoxEm * ln.size * spacing * s; };
        // The block's height: every line's step but the last, plus the last line's own box.
        auto blockHeightPx = [&]( const std::vector<TextLine>& ls, float s ) -> float
        {
            if ( ls.empty() )
                return 0.0f;
            float h = 0.0f;
            for ( size_t i = 0; i + 1 < ls.size(); ++i )
                h += lineStepPx( ls[i], s );
            return h + lineBoxEm * ls.back().size * s;
        };

        auto layoutAt = [&]( float s ) -> std::vector<TextLine>
        {
            const float threshold = t.Wrap ? availW / s : std::numeric_limits<float>::max();
            return LayoutLines( chars, adv, threshold, t.Wrap );
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
                const bool fitsH = blockHeightPx( lines, s ) <= availH;
                const bool fitsW = t.Wrap || ( maxLineEm * s <= availW );
                if ( ( fitsH && fitsW ) || s <= minS )
                    break;
                s     = std::max( minS, s * 0.92f );
                lines = layoutAt( s );
            }
        }

        if ( lineBoxEm * spacing * s <= 0.0f )
            return;

        // Overflow: drop the lines that fall past the bottom (Ellipsis/Clip), tagging the tail as truncated.
        // A line is visible while the steps up to and including it fit — at least one always is.
        bool truncated = false;
        if ( t.Overflow != UITextOverflow::Overflow )
        {
            size_t maxVisible = 0;
            float  used       = 0.0f;
            for ( const TextLine& ln : lines )
            {
                used += lineStepPx( ln, s );
                if ( used > availH )
                    break;
                ++maxVisible;
            }
            maxVisible = std::max<size_t>( 1, maxVisible );
            if ( lines.size() > maxVisible )
            {
                lines.resize( maxVisible );
                truncated = ( t.Overflow == UITextOverflow::Ellipsis );
            }
        }

        // Ellipsis: trim trailing items off any over-wide line (and the truncated tail) and append "...",
        // styled like the text it replaces.
        if ( t.Overflow == UITextOverflow::Ellipsis )
        {
            auto ellipsize = [&]( TextLine& ln )
            {
                StyledChar dot =
                     ln.chars.empty() ? StyledChar{ '.', glm::vec4( t.Color, 1.0f ) } : ln.chars.back();
                dot.ch           = '.';
                dot.kind         = InlineKind::Glyph;
                dot.asset        = 0;
                const float dots = 3.0f * adv( dot );
                while ( !ln.chars.empty() && ( ln.width + dots ) * s > availW )
                {
                    ln.width -= adv( ln.chars.back() );
                    ln.chars.pop_back();
                }
                for ( int k = 0; k < 3; ++k )
                {
                    ln.chars.push_back( dot );
                    ln.width += adv( dot );
                }
            };
            if ( adv( StyledChar{ '.' } ) > 0.0f )
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
        const float blockH   = blockHeightPx( lines, s );
        float       blockTop = rect.Y + pad; // Top
        if ( t.VerticalAlign == UITextVAlign::Middle )
            blockTop = rect.Y + ( rect.H - blockH ) * 0.5f;
        else if ( t.VerticalAlign == UITextVAlign::Bottom )
            blockTop = rect.Y + rect.H - pad - blockH;

        // Position every item (per-line horizontal alignment; each line's baseline sits under its tallest
        // item), collecting quads for the draw passes.
        float lineTop = blockTop;
        for ( const TextLine& ln : lines )
        {
            const float lineWpx = ln.width * s;
            float       penX    = rect.X + pad; // Left
            if ( t.Align == UITextAlign::Center )
                penX = rect.X + ( rect.W - lineWpx ) * 0.5f;
            else if ( t.Align == UITextAlign::Right )
                penX = rect.X + rect.W - pad - lineWpx;
            const float baselineY = lineTop + bf.Ascent * ln.size * s;
            for ( const StyledChar& sc : ln.chars )
                penX += place( sc, penX, baselineY, s );
            lineTop += lineStepPx( ln, s );
        }

        const bool clip = ( t.Overflow == UITextOverflow::Clip );
        if ( clip )
            dl.PushClipRect( { rect.X, rect.Y }, { rect.X + rect.W, rect.Y + rect.H } );
        drawPlaced();
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
