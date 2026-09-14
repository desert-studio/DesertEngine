#include "FontBaker.hpp"

#include "Msdf.hpp"

#include <stb_truetype/stb_truetype.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace Desert::Text
{
    namespace
    {
        // One rasterized glyph before packing: its MSDF bitmap (RGB floats) + metrics.
        struct RawGlyph
        {
            uint32_t           Codepoint = 0;
            std::vector<float> Rgb; // W*H*3, [0,1] with 0.5 on the outline
            int                W = 0, H = 0;
            int                XOff = 0, YOff = 0; // glyph top-left relative to the pen baseline (Y down)
            float              Advance = 0;        // scaled pixels
        };

        // stb hands back the outline in FONT UNITS with Y up; the atlas is texels with Y down. Both the
        // scale and the flip happen here, once, so nothing downstream carries a second convention.
        //
        // The flip also reverses the outline's handedness, and the distance field's SIGN follows the
        // handedness — get it wrong and every glyph is inside-out. Rather than hard-coding TrueType's
        // winding (CFF outlines, which .ttf files may also carry, use the opposite one), the orientation
        // is MEASURED from the shape's own signed area and corrected. A glyph whose area comes out zero
        // is empty and has no sign to get wrong.
        bool BuildShape( const stbtt_fontinfo& font, uint32_t codepoint, float scale, int ix0, int iy0,
                         Msdf::Shape& out )
        {
            stbtt_vertex* verts     = nullptr;
            const int     vertCount = stbtt_GetCodepointShape( &font, static_cast<int>( codepoint ), &verts );
            if ( vertCount <= 0 || !verts )
            {
                if ( verts )
                    stbtt_FreeShape( &font, verts );
                return false;
            }

            const double offX = -static_cast<double>( ix0 ) + kGlyphPadding;
            const double offY = -static_cast<double>( iy0 ) + kGlyphPadding;
            auto         xf   = [&]( double fx, double fy ) -> Msdf::Vec2
            { return { fx * scale + offX, -fy * scale + offY }; };

            Msdf::Contour current;
            Msdf::Vec2    contourStart{};
            Msdf::Vec2    pen{};
            bool          open = false;

            auto closeContour = [&]()
            {
                if ( !open )
                    return;
                if ( std::fabs( pen.X - contourStart.X ) > 1e-9 || std::fabs( pen.Y - contourStart.Y ) > 1e-9 )
                {
                    Msdf::EdgeSegment e;
                    e.PointCount = 2;
                    e.P[0]       = pen;
                    e.P[1]       = contourStart;
                    current.Edges.push_back( e );
                }
                if ( !current.Edges.empty() )
                    out.Contours.push_back( std::move( current ) );
                current = Msdf::Contour{};
                open    = false;
            };

            for ( int i = 0; i < vertCount; ++i )
            {
                const stbtt_vertex& v = verts[i];
                const Msdf::Vec2    p = xf( v.x, v.y );
                switch ( v.type )
                {
                    case STBTT_vmove:
                        closeContour();
                        contourStart = p;
                        pen          = p;
                        open         = true;
                        break;
                    case STBTT_vline:
                    {
                        Msdf::EdgeSegment e;
                        e.PointCount = 2;
                        e.P[0]       = pen;
                        e.P[1]       = p;
                        current.Edges.push_back( e );
                        pen = p;
                        break;
                    }
                    case STBTT_vcurve:
                    {
                        Msdf::EdgeSegment e;
                        e.PointCount = 3;
                        e.P[0]       = pen;
                        e.P[1]       = xf( v.cx, v.cy );
                        e.P[2]       = p;
                        current.Edges.push_back( e );
                        pen = p;
                        break;
                    }
                    case STBTT_vcubic:
                    {
                        Msdf::EdgeSegment e;
                        e.PointCount = 4;
                        e.P[0]       = pen;
                        e.P[1]       = xf( v.cx, v.cy );
                        e.P[2]       = xf( v.cx1, v.cy1 );
                        e.P[3]       = p;
                        current.Edges.push_back( e );
                        pen = p;
                        break;
                    }
                    default:
                        break;
                }
            }
            closeContour();
            stbtt_FreeShape( &font, verts );

            if ( out.Contours.empty() )
                return false;

            // Shoelace over the control polygons. Its SIGN is all that is read, and a control polygon has
            // the same orientation as the curve it bounds, so sampling the curves would cost time and
            // change nothing.
            double area = 0.0;
            for ( const Msdf::Contour& c : out.Contours )
                for ( const Msdf::EdgeSegment& e : c.Edges )
                    for ( int i = 0; i + 1 < e.PointCount; ++i )
                        area += e.P[i].X * e.P[i + 1].Y - e.P[i + 1].X * e.P[i].Y;

            // Positive area means the interior sits on the LEFT of the travel direction, and the field's
            // sign convention puts positive on the right — so reverse.
            if ( area > 0.0 )
            {
                for ( Msdf::Contour& c : out.Contours )
                {
                    std::reverse( c.Edges.begin(), c.Edges.end() );
                    for ( Msdf::EdgeSegment& e : c.Edges )
                        std::reverse( e.P, e.P + e.PointCount );
                }
            }
            return true;
        }
    } // namespace

    BakedFont BakeFontMSDF( const uint8_t* ttf, size_t ttfSize, float pixelHeight,
                            const std::vector<uint32_t>& extraCodepoints )
    {
        BakedFont out;
        if ( !ttf || ttfSize == 0 || pixelHeight <= 0.0f )
            return out;

        stbtt_fontinfo font;
        // stbtt_GetFontOffsetForIndex returns -1 for non-font data. stbtt_InitFont does NOT re-check it and
        // stbtt__find_table would then read the table directory at (data - 1 + ...) — a heap-buffer-overflow
        // on garbage/too-short input (ASan caught this on the RejectsGarbage test). Reject a bad offset first.
        const int fontOffset = stbtt_GetFontOffsetForIndex( ttf, 0 );
        if ( fontOffset < 0 || !stbtt_InitFont( &font, ttf, fontOffset ) )
            return out;

        const float scale = stbtt_ScaleForPixelHeight( &font, pixelHeight );

        int ascent = 0, descent = 0, lineGap = 0;
        stbtt_GetFontVMetrics( &font, &ascent, &descent, &lineGap );
        out.PixelHeight         = pixelHeight;
        out.Ascent              = ascent * scale;
        out.Descent             = descent * scale;
        out.LineGap             = lineGap * scale;
        out.DistanceRangeTexels = kDistanceRangeTexels;

        // Printable ASCII, then whatever else was asked for. Duplicates and codepoints this font has no
        // glyph for are dropped, so a caller can request optimistically.
        std::vector<uint32_t> codepoints;
        codepoints.reserve( 95 + extraCodepoints.size() );
        for ( uint32_t cp = 32; cp <= 126; ++cp )
            codepoints.push_back( cp );
        for ( uint32_t cp : extraCodepoints )
        {
            if ( cp >= 32 && cp <= 126 )
                continue;
            if ( stbtt_FindGlyphIndex( &font, static_cast<int>( cp ) ) == 0 )
                continue;
            if ( std::find( codepoints.begin(), codepoints.end(), cp ) == codepoints.end() )
                codepoints.push_back( cp );
        }

        std::vector<RawGlyph> raws;
        raws.reserve( codepoints.size() );
        for ( uint32_t cp : codepoints )
        {
            int advance = 0, lsb = 0;
            stbtt_GetCodepointHMetrics( &font, static_cast<int>( cp ), &advance, &lsb );

            RawGlyph rg;
            rg.Codepoint = cp;
            rg.Advance   = advance * scale;

            int ix0 = 0, iy0 = 0, ix1 = 0, iy1 = 0;
            stbtt_GetCodepointBitmapBox( &font, static_cast<int>( cp ), scale, scale, &ix0, &iy0, &ix1, &iy1 );
            const int w = ( ix1 - ix0 ) + 2 * kGlyphPadding;
            const int h = ( iy1 - iy0 ) + 2 * kGlyphPadding;

            // Space (and any empty glyph) has no box — keep it for its advance only.
            Msdf::Shape shape;
            if ( w > 2 * kGlyphPadding && h > 2 * kGlyphPadding && BuildShape( font, cp, scale, ix0, iy0, shape ) )
            {
                Msdf::ColorEdges( shape );
                Msdf::GenerateMSDF( rg.Rgb, w, h, shape, kDistanceRangeTexels );
                rg.W    = w;
                rg.H    = h;
                rg.XOff = ix0 - kGlyphPadding;
                rg.YOff = iy0 - kGlyphPadding;
            }
            raws.push_back( std::move( rg ) );
        }

        // Shelf pack: left-to-right rows of fixed atlas width, wrap to a new shelf when the row is full.
        const uint32_t spacing = 1; // 1px gutter so bilinear sampling never bleeds a neighbour
        uint32_t       penX = spacing, penY = spacing, shelfH = 0, usedH = 0;
        struct Placed
        {
            uint32_t        X, Y;
            const RawGlyph* G;
        };
        std::vector<Placed> placed;
        placed.reserve( raws.size() );

        for ( const auto& rg : raws )
        {
            if ( rg.W <= 0 || rg.H <= 0 )
                continue; // empty glyph (space) — no atlas cell needed
            const uint32_t gw = static_cast<uint32_t>( rg.W );
            const uint32_t gh = static_cast<uint32_t>( rg.H );
            if ( penX + gw + spacing > kAtlasWidth )
            {
                penX = spacing;
                penY += shelfH + spacing;
                shelfH = 0;
            }
            placed.push_back( { penX, penY, &rg } );
            penX += gw + spacing;
            shelfH = std::max( shelfH, gh );
            usedH  = std::max( usedH, penY + gh + spacing );
        }

        out.AtlasWidth  = kAtlasWidth;
        out.AtlasHeight = std::max<uint32_t>( usedH, 1 );
        // Cleared to "far outside" (0) in RGB and opaque in alpha: an unwritten texel must read as empty
        // space, and a zeroed alpha would make the atlas's own debug view a black rectangle.
        out.AtlasRGBA.assign( static_cast<size_t>( out.AtlasWidth ) * out.AtlasHeight * 4, 0 );
        for ( size_t i = 3; i < out.AtlasRGBA.size(); i += 4 )
            out.AtlasRGBA[i] = 255;

        const auto invW = 1.0f / static_cast<float>( out.AtlasWidth );
        const auto invH = 1.0f / static_cast<float>( out.AtlasHeight );

        // Copy each packed bitmap into the atlas and record UVs + metrics.
        for ( const auto& p : placed )
        {
            const RawGlyph& rg = *p.G;
            for ( int y = 0; y < rg.H; ++y )
            {
                for ( int x = 0; x < rg.W; ++x )
                {
                    const float* src = &rg.Rgb[( static_cast<size_t>( y ) * rg.W + x ) * 3];
                    uint8_t*     dst =
                         &out.AtlasRGBA[( ( static_cast<size_t>( p.Y + y ) * out.AtlasWidth ) + ( p.X + x ) ) * 4];
                    for ( int c = 0; c < 3; ++c )
                        dst[c] = static_cast<uint8_t>( std::lround( std::clamp( src[c], 0.0f, 1.0f ) * 255.0f ) );
                }
            }

            Glyph g;
            g.U0                     = p.X * invW;
            g.V0                     = p.Y * invH;
            g.U1                     = ( p.X + rg.W ) * invW;
            g.V1                     = ( p.Y + rg.H ) * invH;
            g.Width                  = static_cast<float>( rg.W );
            g.Height                 = static_cast<float>( rg.H );
            g.OffsetX                = static_cast<float>( rg.XOff );
            g.OffsetY                = static_cast<float>( rg.YOff );
            g.Advance                = rg.Advance;
            out.Glyphs[rg.Codepoint] = g;
        }

        // Empty glyphs (space) carry only an advance so layout can step past them.
        for ( const auto& rg : raws )
        {
            if ( out.Glyphs.count( rg.Codepoint ) )
                continue;
            Glyph g;
            g.Advance                = rg.Advance;
            out.Glyphs[rg.Codepoint] = g;
        }

        return out;
    }

    namespace
    {
        template <class T>
        void PutPod( std::vector<uint8_t>& out, const T& v )
        {
            const auto* b = reinterpret_cast<const uint8_t*>( &v );
            out.insert( out.end(), b, b + sizeof( T ) );
        }

        // Bounded reader over a byte span: every Get checks the remaining size, so a truncated/corrupt
        // cache file can never over-read — it just makes DeserializeBakedFont report Corrupt.
        struct Reader
        {
            const uint8_t* p;
            size_t         remaining;
            bool           ok = true;

            template <class T>
            T Get()
            {
                T v{};
                if ( remaining < sizeof( T ) )
                {
                    ok = false;
                    return v;
                }
                std::memcpy( &v, p, sizeof( T ) );
                p += sizeof( T );
                remaining -= sizeof( T );
                return v;
            }

            void GetBytes( void* dst, size_t n )
            {
                if ( remaining < n )
                {
                    ok = false;
                    return;
                }
                std::memcpy( dst, p, n );
                p += n;
                remaining -= n;
            }
        };

        constexpr char kFontMagic[4] = { 'D', 'F', 'N', 'T' };
    } // namespace

    std::vector<uint8_t> SerializeBakedFont( const BakedFont& font )
    {
        std::vector<uint8_t> out;
        out.insert( out.end(), kFontMagic, kFontMagic + 4 );
        PutPod( out, kBakedFontCacheVersion );
        PutPod( out, font.AtlasWidth );
        PutPod( out, font.AtlasHeight );
        PutPod( out, font.PixelHeight );
        PutPod( out, font.Ascent );
        PutPod( out, font.Descent );
        PutPod( out, font.LineGap );
        PutPod( out, font.DistanceRangeTexels );
        PutPod( out, static_cast<uint32_t>( font.Glyphs.size() ) );
        for ( const auto& [codepoint, glyph] : font.Glyphs )
        {
            PutPod( out, codepoint );
            PutPod( out, glyph ); // Glyph is a POD of floats — no padding to worry about
        }
        PutPod( out, static_cast<uint64_t>( font.AtlasRGBA.size() ) );
        out.insert( out.end(), font.AtlasRGBA.begin(), font.AtlasRGBA.end() );
        return out;
    }

    FontDecodeStatus DeserializeBakedFont( const uint8_t* data, size_t size, BakedFont& out,
                                           uint32_t* fileVersion )
    {
        Reader r{ data, size };
        char   magic[4] = {};
        r.GetBytes( magic, 4 );
        if ( !r.ok || std::memcmp( magic, kFontMagic, 4 ) != 0 )
            return FontDecodeStatus::BadMagic;

        const uint32_t version = r.Get<uint32_t>();
        if ( !r.ok )
            return FontDecodeStatus::BadMagic; // four bytes of "DFNT" and nothing behind them
        if ( fileVersion )
            *fileVersion = version;
        if ( version != kBakedFontCacheVersion )
            return FontDecodeStatus::VersionMismatch;

        BakedFont f;
        f.AtlasWidth          = r.Get<uint32_t>();
        f.AtlasHeight         = r.Get<uint32_t>();
        f.PixelHeight         = r.Get<float>();
        f.Ascent              = r.Get<float>();
        f.Descent             = r.Get<float>();
        f.LineGap             = r.Get<float>();
        f.DistanceRangeTexels = r.Get<float>();

        const uint32_t glyphCount = r.Get<uint32_t>();
        if ( !r.ok )
            return FontDecodeStatus::Corrupt;
        for ( uint32_t i = 0; i < glyphCount; ++i )
        {
            const uint32_t codepoint = r.Get<uint32_t>();
            const Glyph    glyph     = r.Get<Glyph>();
            if ( !r.ok )
                return FontDecodeStatus::Corrupt;
            f.Glyphs.emplace( codepoint, glyph );
        }

        const uint64_t atlasSize = r.Get<uint64_t>();
        if ( !r.ok || atlasSize != r.remaining )
            return FontDecodeStatus::Corrupt; // trailing bytes must be exactly the atlas
        if ( atlasSize != static_cast<uint64_t>( f.AtlasWidth ) * f.AtlasHeight * 4 )
            return FontDecodeStatus::Corrupt; // header and payload must agree on the atlas's own size
        f.AtlasRGBA.resize( static_cast<size_t>( atlasSize ) );
        r.GetBytes( f.AtlasRGBA.data(), f.AtlasRGBA.size() );

        if ( !r.ok || !f.Valid() )
            return FontDecodeStatus::Corrupt;
        out = std::move( f );
        return FontDecodeStatus::Ok;
    }
} // namespace Desert::Text
