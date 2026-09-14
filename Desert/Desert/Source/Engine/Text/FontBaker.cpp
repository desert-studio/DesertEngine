#include "FontBaker.hpp"

#include "Msdf.hpp"

#include <stb_truetype/stb_truetype.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Desert::Text
{
    namespace
    {
        constexpr uint32_t kFirstPrintableAscii = 32;
        constexpr uint32_t kLastPrintableAscii  = 126;
        constexpr uint32_t kAsciiGlyphCount     = kLastPrintableAscii - kFirstPrintableAscii + 1;

        constexpr uint8_t kOpaqueAlpha = 255;
        constexpr float   kByteScale   = 255.0F;

        constexpr size_t kChannelsPerTexel = 4; // the atlas is RGBA8, the one sampled format we have
        constexpr size_t kFieldChannels    = 3; // …of which RGB carry the multi-channel distance field

        constexpr uint32_t kCellGutter = 1; // 1 texel between packed cells so bilinear cannot bleed

        // A contour is closed when the pen has come back to where it started. Font units are integers
        // scaled by a float, so "the same point" is a tolerance rather than an equality.
        constexpr double kContourCloseTolerance = 1e-9;

        // One rasterized glyph before packing: its MSDF bitmap (RGB floats) + metrics.
        struct RawGlyph
        {
            uint32_t           Codepoint = 0;
            std::vector<float> Rgb; // Width*Height*kFieldChannels, [0,1] with 0.5 on the outline
            int                Width   = 0;
            int                Height  = 0;
            int                OffsetX = 0; // glyph top-left relative to the pen baseline (Y down)
            int                OffsetY = 0;
            float              Advance = 0; // scaled pixels
        };

        struct Placed
        {
            uint32_t X     = 0;
            uint32_t Y     = 0;
            size_t   Glyph = 0; // index into the raw glyph vector — an index, so a resize cannot dangle
        };

        // The shoelace SIGN of one control polygon. Sampling the curves would cost time and change
        // nothing: a control polygon has the same orientation as the curve it bounds.
        double PolygonArea( const Msdf::EdgeSegment& edge )
        {
            double area = 0.0;
            for ( int idx = 0; idx + 1 < edge.PointCount; ++idx )
            {
                const Msdf::Vec2 here = edge.Points.at( static_cast<size_t>( idx ) );
                const Msdf::Vec2 next = edge.Points.at( static_cast<size_t>( idx ) + 1 );
                area += ( here.X * next.Y ) - ( next.X * here.Y );
            }
            return area;
        }

        void ReverseShape( Msdf::Shape& shape )
        {
            for ( Msdf::Contour& contour : shape.Contours )
            {
                std::reverse( contour.Edges.begin(), contour.Edges.end() );
                for ( Msdf::EdgeSegment& edge : contour.Edges )
                {
                    std::reverse( edge.Points.begin(),
                                  edge.Points.begin() + static_cast<std::ptrdiff_t>( edge.PointCount ) );
                }
            }
        }

        // Turn one stb vertex stream into contours, in the atlas's own texel space. Kept apart from the
        // winding correction below so neither is long enough to hide anything.
        void AppendContours( const std::vector<stbtt_vertex>& verts, float scale, double offsetX, double offsetY,
                             Msdf::Shape& out )
        {
            const auto transform = [scale, offsetX, offsetY]( double fontX, double fontY ) -> Msdf::Vec2
            { return { ( fontX * scale ) + offsetX, ( -fontY * scale ) + offsetY }; };

            Msdf::Contour current;
            Msdf::Vec2    contourStart{};
            Msdf::Vec2    pen{};
            bool          open = false;

            const auto closeContour = [&]()
            {
                if ( !open )
                {
                    return;
                }
                if ( std::fabs( pen.X - contourStart.X ) > kContourCloseTolerance ||
                     std::fabs( pen.Y - contourStart.Y ) > kContourCloseTolerance )
                {
                    Msdf::EdgeSegment edge;
                    edge.PointCount = 2;
                    edge.Points[0]  = pen;
                    edge.Points[1]  = contourStart;
                    current.Edges.push_back( edge );
                }
                if ( !current.Edges.empty() )
                {
                    out.Contours.push_back( std::move( current ) );
                }
                current = Msdf::Contour{};
                open    = false;
            };

            for ( const stbtt_vertex& vertex : verts )
            {
                const Msdf::Vec2  point = transform( vertex.x, vertex.y );
                Msdf::EdgeSegment edge;
                switch ( vertex.type )
                {
                    case STBTT_vmove:
                        closeContour();
                        contourStart = point;
                        pen          = point;
                        open         = true;
                        continue;
                    case STBTT_vline:
                        edge.PointCount = 2;
                        edge.Points[0]  = pen;
                        edge.Points[1]  = point;
                        break;
                    case STBTT_vcurve:
                        edge.PointCount = 3;
                        edge.Points[0]  = pen;
                        edge.Points[1]  = transform( vertex.cx, vertex.cy );
                        edge.Points[2]  = point;
                        break;
                    case STBTT_vcubic:
                        edge.PointCount = 4;
                        edge.Points[0]  = pen;
                        edge.Points[1]  = transform( vertex.cx, vertex.cy );
                        edge.Points[2]  = transform( vertex.cx1, vertex.cy1 );
                        edge.Points[3]  = point;
                        break;
                    default:
                        continue;
                }
                current.Edges.push_back( edge );
                pen = point;
            }
            closeContour();
        }

        // stb hands back the outline in FONT UNITS with Y up; the atlas is texels with Y down. Both the
        // scale and the flip happen here, once, so nothing downstream carries a second convention.
        //
        // The flip also reverses the outline's handedness, and the distance field's SIGN follows the
        // handedness — get it wrong and every glyph is inside-out. Rather than hard-coding TrueType's
        // winding (CFF outlines, which .ttf files may also carry, use the opposite one), the orientation
        // is MEASURED from the shape's own signed area and corrected.
        bool BuildShape( const stbtt_fontinfo& font, uint32_t codepoint, float scale, int boxX0, int boxY0,
                         Msdf::Shape& out )
        {
            stbtt_vertex* verts     = nullptr;
            const int     vertCount = stbtt_GetCodepointShape( &font, static_cast<int>( codepoint ), &verts );
            if ( vertCount <= 0 || verts == nullptr )
            {
                if ( verts != nullptr )
                {
                    stbtt_FreeShape( &font, verts );
                }
                return false;
            }
            // Copied out of stb's buffer immediately: everything below walks a container, so the one
            // place that has to reason about a raw pointer and its length is this line.
            const std::vector<stbtt_vertex> outline( verts, std::next( verts, vertCount ) );
            stbtt_FreeShape( &font, verts );

            AppendContours( outline, scale, -static_cast<double>( boxX0 ) + kGlyphPadding,
                            -static_cast<double>( boxY0 ) + kGlyphPadding, out );

            if ( out.Contours.empty() )
            {
                return false;
            }

            double area = 0.0;
            for ( const Msdf::Contour& contour : out.Contours )
            {
                for ( const Msdf::EdgeSegment& edge : contour.Edges )
                {
                    area += PolygonArea( edge );
                }
            }

            // Positive area means the interior sits on the LEFT of the travel direction, and the field's
            // sign convention puts positive on the right — so reverse.
            if ( area > 0.0 )
            {
                ReverseShape( out );
            }
            return true;
        }

        // Printable ASCII, then whatever else was asked for. Duplicates and codepoints this font has no
        // glyph for are dropped, so a caller can request optimistically.
        std::vector<uint32_t> CodepointsToBake( const stbtt_fontinfo&        font,
                                                const std::vector<uint32_t>& extraCodepoints )
        {
            std::vector<uint32_t> codepoints;
            codepoints.reserve( kAsciiGlyphCount + extraCodepoints.size() );
            for ( uint32_t code = kFirstPrintableAscii; code <= kLastPrintableAscii; ++code )
            {
                codepoints.push_back( code );
            }
            for ( const uint32_t code : extraCodepoints )
            {
                if ( code >= kFirstPrintableAscii && code <= kLastPrintableAscii )
                {
                    continue;
                }
                if ( stbtt_FindGlyphIndex( &font, static_cast<int>( code ) ) == 0 )
                {
                    continue;
                }
                if ( std::find( codepoints.begin(), codepoints.end(), code ) == codepoints.end() )
                {
                    codepoints.push_back( code );
                }
            }
            return codepoints;
        }

        RawGlyph RasterizeGlyph( const stbtt_fontinfo& font, uint32_t codepoint, float scale )
        {
            int advance = 0;
            int bearing = 0;
            stbtt_GetCodepointHMetrics( &font, static_cast<int>( codepoint ), &advance, &bearing );

            RawGlyph raw;
            raw.Codepoint = codepoint;
            raw.Advance   = static_cast<float>( advance ) * scale;

            int boxX0 = 0;
            int boxY0 = 0;
            int boxX1 = 0;
            int boxY1 = 0;
            stbtt_GetCodepointBitmapBox( &font, static_cast<int>( codepoint ), scale, scale, &boxX0, &boxY0,
                                         &boxX1, &boxY1 );
            const int width  = ( boxX1 - boxX0 ) + ( 2 * kGlyphPadding );
            const int height = ( boxY1 - boxY0 ) + ( 2 * kGlyphPadding );

            // Space (and any empty glyph) has no box — keep it for its advance only.
            Msdf::Shape shape;
            if ( width > 2 * kGlyphPadding && height > 2 * kGlyphPadding &&
                 BuildShape( font, codepoint, scale, boxX0, boxY0, shape ) )
            {
                Msdf::ColorEdges( shape );
                Msdf::GenerateMSDF( raw.Rgb, width, height, shape, kDistanceRangeTexels );
                raw.Width   = width;
                raw.Height  = height;
                raw.OffsetX = boxX0 - kGlyphPadding;
                raw.OffsetY = boxY0 - kGlyphPadding;
            }
            return raw;
        }

        // Shelf pack: left-to-right rows of fixed atlas width, wrap to a new shelf when the row is full.
        std::vector<Placed> ShelfPack( const std::vector<RawGlyph>& raws, uint32_t& usedHeight )
        {
            uint32_t penX      = kCellGutter;
            uint32_t penY      = kCellGutter;
            uint32_t shelfHigh = 0;
            usedHeight         = 0;

            std::vector<Placed> placed;
            placed.reserve( raws.size() );
            for ( size_t idx = 0; idx < raws.size(); ++idx )
            {
                const RawGlyph& raw = raws[idx];
                if ( raw.Width <= 0 || raw.Height <= 0 )
                {
                    continue; // empty glyph (space) — no atlas cell needed
                }
                const auto cellWide = static_cast<uint32_t>( raw.Width );
                const auto cellHigh = static_cast<uint32_t>( raw.Height );
                if ( penX + cellWide + kCellGutter > kAtlasWidth )
                {
                    penX = kCellGutter;
                    penY += shelfHigh + kCellGutter;
                    shelfHigh = 0;
                }
                placed.push_back( { penX, penY, idx } );
                penX += cellWide + kCellGutter;
                shelfHigh  = std::max( shelfHigh, cellHigh );
                usedHeight = std::max( usedHeight, penY + cellHigh + kCellGutter );
            }
            return placed;
        }

        void BlitGlyph( BakedFont& font, const RawGlyph& raw, const Placed& slot )
        {
            for ( int row = 0; row < raw.Height; ++row )
            {
                for ( int col = 0; col < raw.Width; ++col )
                {
                    const size_t source = ( ( static_cast<size_t>( row ) * raw.Width ) + col ) * kFieldChannels;
                    const size_t target =
                         ( ( static_cast<size_t>( slot.Y + row ) * font.AtlasWidth ) + slot.X + col ) *
                         kChannelsPerTexel;
                    for ( size_t channel = 0; channel < kFieldChannels; ++channel )
                    {
                        const float value = std::clamp( raw.Rgb[source + channel], 0.0F, 1.0F );
                        font.AtlasRGBA[target + channel] =
                             static_cast<uint8_t>( std::lround( value * kByteScale ) );
                    }
                }
            }
        }

        Glyph MetricsFor( const RawGlyph& raw, const Placed& slot, uint32_t atlasWidth, uint32_t atlasHeight )
        {
            const auto invWidth  = 1.0F / static_cast<float>( atlasWidth );
            const auto invHeight = 1.0F / static_cast<float>( atlasHeight );

            Glyph glyph;
            glyph.U0      = static_cast<float>( slot.X ) * invWidth;
            glyph.V0      = static_cast<float>( slot.Y ) * invHeight;
            glyph.U1      = static_cast<float>( slot.X + static_cast<uint32_t>( raw.Width ) ) * invWidth;
            glyph.V1      = static_cast<float>( slot.Y + static_cast<uint32_t>( raw.Height ) ) * invHeight;
            glyph.Width   = static_cast<float>( raw.Width );
            glyph.Height  = static_cast<float>( raw.Height );
            glyph.OffsetX = static_cast<float>( raw.OffsetX );
            glyph.OffsetY = static_cast<float>( raw.OffsetY );
            glyph.Advance = raw.Advance;
            return glyph;
        }
    } // namespace

    BakedFont BakeFontMSDF( const uint8_t* ttf, size_t ttfSize, float pixelHeight,
                            const std::vector<uint32_t>& extraCodepoints )
    {
        BakedFont out;
        if ( ttf == nullptr || ttfSize == 0 || pixelHeight <= 0.0F )
        {
            return out;
        }

        stbtt_fontinfo font;
        // stbtt_GetFontOffsetForIndex returns -1 for non-font data. stbtt_InitFont does NOT re-check it and
        // stbtt__find_table would then read the table directory at (data - 1 + ...) — a heap-buffer-overflow
        // on garbage/too-short input (ASan caught this on the RejectsGarbage test). Reject a bad offset first.
        const int fontOffset = stbtt_GetFontOffsetForIndex( ttf, 0 );
        if ( fontOffset < 0 || stbtt_InitFont( &font, ttf, fontOffset ) == 0 )
        {
            return out;
        }

        const float scale = stbtt_ScaleForPixelHeight( &font, pixelHeight );

        int ascent  = 0;
        int descent = 0;
        int lineGap = 0;
        stbtt_GetFontVMetrics( &font, &ascent, &descent, &lineGap );
        out.PixelHeight         = pixelHeight;
        out.Ascent              = static_cast<float>( ascent ) * scale;
        out.Descent             = static_cast<float>( descent ) * scale;
        out.LineGap             = static_cast<float>( lineGap ) * scale;
        out.DistanceRangeTexels = kDistanceRangeTexels;

        const std::vector<uint32_t> codepoints = CodepointsToBake( font, extraCodepoints );
        std::vector<RawGlyph>       raws;
        raws.reserve( codepoints.size() );
        for ( const uint32_t codepoint : codepoints )
        {
            raws.push_back( RasterizeGlyph( font, codepoint, scale ) );
        }

        uint32_t                  usedHeight = 0;
        const std::vector<Placed> placed     = ShelfPack( raws, usedHeight );

        out.AtlasWidth  = kAtlasWidth;
        out.AtlasHeight = std::max<uint32_t>( usedHeight, 1 );
        // Cleared to "far outside" (0) in RGB and opaque in alpha: an unwritten texel must read as empty
        // space, and a zeroed alpha would make the atlas's own debug view a black rectangle.
        out.AtlasRGBA.assign( static_cast<size_t>( out.AtlasWidth ) * out.AtlasHeight * kChannelsPerTexel, 0 );
        for ( size_t texel = kFieldChannels; texel < out.AtlasRGBA.size(); texel += kChannelsPerTexel )
        {
            out.AtlasRGBA[texel] = kOpaqueAlpha;
        }

        for ( const Placed& slot : placed )
        {
            const RawGlyph& raw = raws[slot.Glyph];
            BlitGlyph( out, raw, slot );
            out.Glyphs[raw.Codepoint] = MetricsFor( raw, slot, out.AtlasWidth, out.AtlasHeight );
        }

        // Empty glyphs (space) carry only an advance so layout can step past them.
        for ( const RawGlyph& raw : raws )
        {
            if ( out.Glyphs.contains( raw.Codepoint ) )
            {
                continue;
            }
            Glyph glyph;
            glyph.Advance             = raw.Advance;
            out.Glyphs[raw.Codepoint] = glyph;
        }

        return out;
    }

    namespace
    {
        template <class T>
        void PutPod( std::vector<uint8_t>& out, const T& value )
        {
            std::array<uint8_t, sizeof( T )> bytes{};
            std::memcpy( bytes.data(), &value, sizeof( T ) );
            out.insert( out.end(), bytes.begin(), bytes.end() );
        }

        // Bounded reader over a byte span: every Get checks the remaining size, so a truncated/corrupt
        // cache file can never over-read — it just makes DeserializeBakedFont report Corrupt.
        class Reader
        {
        public:
            Reader( const uint8_t* data, size_t size )
                 : m_Bytes( data == nullptr ? std::span<const uint8_t>{} : std::span( data, size ) )
            {
            }

            template <class T>
            T Get()
            {
                T value{};
                GetBytes( &value, sizeof( T ) );
                return value;
            }

            void GetBytes( void* dst, size_t count )
            {
                if ( m_Bytes.size() < count )
                {
                    m_Ok = false;
                    return;
                }
                std::memcpy( dst, m_Bytes.data(), count );
                m_Bytes = m_Bytes.subspan( count );
            }

            [[nodiscard]] bool Ok() const
            {
                return m_Ok;
            }
            [[nodiscard]] size_t Remaining() const
            {
                return m_Bytes.size();
            }

        private:
            std::span<const uint8_t> m_Bytes;
            bool                     m_Ok = true;
        };

        constexpr std::array<char, 4> kFontMagic = { 'D', 'F', 'N', 'T' };

        // Magic + version + two atlas extents + five floats + the glyph count: enough to reserve the
        // buffer in one go rather than growing it under every field.
        constexpr size_t kHeaderBytes = 44;
    } // namespace

    std::vector<uint8_t> SerializeBakedFont( const BakedFont& font )
    {
        std::vector<uint8_t> out;
        out.reserve( font.AtlasRGBA.size() + ( font.Glyphs.size() * sizeof( Glyph ) ) + kHeaderBytes );
        for ( const char letter : kFontMagic )
        {
            out.push_back( static_cast<uint8_t>( letter ) );
        }
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
        Reader              reader( data, size );
        std::array<char, 4> magic{};
        reader.GetBytes( magic.data(), magic.size() );
        if ( !reader.Ok() || magic != kFontMagic )
        {
            return FontDecodeStatus::BadMagic;
        }

        const auto version = reader.Get<uint32_t>();
        if ( !reader.Ok() )
        {
            return FontDecodeStatus::BadMagic; // four bytes of "DFNT" and nothing behind them
        }
        if ( fileVersion != nullptr )
        {
            *fileVersion = version;
        }
        if ( version != kBakedFontCacheVersion )
        {
            return FontDecodeStatus::VersionMismatch;
        }

        BakedFont decoded;
        decoded.AtlasWidth          = reader.Get<uint32_t>();
        decoded.AtlasHeight         = reader.Get<uint32_t>();
        decoded.PixelHeight         = reader.Get<float>();
        decoded.Ascent              = reader.Get<float>();
        decoded.Descent             = reader.Get<float>();
        decoded.LineGap             = reader.Get<float>();
        decoded.DistanceRangeTexels = reader.Get<float>();

        const auto glyphCount = reader.Get<uint32_t>();
        if ( !reader.Ok() )
        {
            return FontDecodeStatus::Corrupt;
        }
        for ( uint32_t idx = 0; idx < glyphCount; ++idx )
        {
            const auto codepoint = reader.Get<uint32_t>();
            const auto glyph     = reader.Get<Glyph>();
            if ( !reader.Ok() )
            {
                return FontDecodeStatus::Corrupt;
            }
            decoded.Glyphs.emplace( codepoint, glyph );
        }

        const auto atlasSize = reader.Get<uint64_t>();
        if ( !reader.Ok() || atlasSize != reader.Remaining() )
        {
            return FontDecodeStatus::Corrupt; // trailing bytes must be exactly the atlas
        }
        if ( atlasSize != static_cast<uint64_t>( decoded.AtlasWidth ) * decoded.AtlasHeight * kChannelsPerTexel )
        {
            return FontDecodeStatus::Corrupt; // header and payload must agree on the atlas's own size
        }
        decoded.AtlasRGBA.resize( static_cast<size_t>( atlasSize ) );
        reader.GetBytes( decoded.AtlasRGBA.data(), decoded.AtlasRGBA.size() );

        if ( !reader.Ok() || !decoded.Valid() )
        {
            return FontDecodeStatus::Corrupt;
        }
        out = std::move( decoded );
        return FontDecodeStatus::Ok;
    }
} // namespace Desert::Text
