#pragma once

// The pure half of DomeSheet: the dome's sampling plan, the label that names a sample, the grid the
// tiles are laid into, the box filter that shrinks them and the 5x7 raster that burns the label in.
//
// It is a header with no I/O in it so that the ONE relation this instrument rests on can be asserted by
// a test rather than observed in a picture.
//
// THE RELATION. A contact sheet is evidence, and evidence that mislabels itself is worse than no
// evidence — the frame still looks like a measurement. The label burnt into a tile and the `--look`
// vector that produced that tile must therefore be two views of ONE number, computed once, never
// re-derived by the shell script that drives the capture. `DomeSample` returns the vector, the label and
// the file stem together, and `ParseDomeLabel` reads the label back to the same two angles, so the
// round trip is a test and not a promise.
//
// This is the same class of defect as the four in the verify skill's table: each side is individually
// correct, and it is their AGREEMENT that fails. A script that formats "AZ 090" while passing
// `--look 1,0,0` is not caught by any test of either side.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace Desert::DomeSheet
{
    // ---- the dome ------------------------------------------------------------------------------------

    /// One look direction on the dome, with everything the capture and the sheet need to agree about it.
    struct DomeSample
    {
        int AzimuthDegrees   = 0;
        int ElevationDegrees = 0;
        /// The `--look` argument, world space, Y up. Not normalized on purpose: `--look` never is, and a
        /// normalized vector printed at six digits is a different string than the one the protocol's own
        /// six points use.
        float LookX = 0.0f;
        float LookY = 0.0f;
        float LookZ = 0.0f;
    };

    /// AZIMUTH ZERO IS -Z, and that is a decision rather than a convention borrowed from anywhere.
    ///
    /// The protocol this sheet replaces shot `--look 0,y,-1` and called it "away from the sun" and
    /// `0,y,+1` "sunward" (Docs/Clouds/DIAGNOSIS_CARTOON.md section 0). Putting azimuth 0 on -Z and
    /// increasing toward +X therefore places the old six points on this dome EXACTLY — away is column
    /// AZ 000, sunward is column AZ 180 — so a number measured under the old protocol and a number
    /// measured on a tile of this sheet are measurements of the same ray, and the two corpora can be
    /// compared instead of merely coexisting.
    inline DomeSample MakeDomeSample( int azimuthDegrees, int elevationDegrees )
    {
        constexpr double kPi = 3.14159265358979323846;

        const double az = static_cast<double>( azimuthDegrees ) * kPi / 180.0;
        const double el = static_cast<double>( elevationDegrees ) * kPi / 180.0;

        DomeSample sample;
        sample.AzimuthDegrees   = azimuthDegrees;
        sample.ElevationDegrees = elevationDegrees;
        sample.LookX            = static_cast<float>( std::sin( az ) * std::cos( el ) );
        sample.LookY            = static_cast<float>( std::sin( el ) );
        sample.LookZ            = static_cast<float>( -std::cos( az ) * std::cos( el ) );
        return sample;
    }

    /// The human-readable name of a sample. Fixed width, so a column of them lines up in a log and so
    /// the burnt-in label occupies the same pixels on every tile — a label that changes width between
    /// tiles reads as a layout bug and invites the reader to distrust the sheet.
    inline std::string DomeLabel( const DomeSample& sample )
    {
        char buffer[32];
        std::snprintf( buffer, sizeof( buffer ), "AZ %03d  EL %02d", sample.AzimuthDegrees,
                       sample.ElevationDegrees );
        return std::string( buffer );
    }

    /// The file stem a tile is written under. Lower case and no spaces, because it is also a shell word.
    inline std::string DomeStem( const DomeSample& sample )
    {
        char buffer[32];
        std::snprintf( buffer, sizeof( buffer ), "az%03d_el%02d", sample.AzimuthDegrees, sample.ElevationDegrees );
        return std::string( buffer );
    }

    /// Read a label back to the two angles it names. The other half of the round trip the test asserts;
    /// false when @p label is not one this file produced.
    inline bool ParseDomeLabel( const std::string& label, int& azimuthDegrees, int& elevationDegrees )
    {
        int az = 0;
        int el = 0;
        // The trailing %n is what makes this a parse of the WHOLE label rather than of its prefix: a
        // label with anything after the elevation is not a label this file wrote.
        int consumed = 0;
        if ( std::sscanf( label.c_str(), "AZ %d  EL %d%n", &az, &el, &consumed ) != 2 )
            return false;
        if ( consumed != static_cast<int>( label.size() ) )
            return false;

        azimuthDegrees   = az;
        elevationDegrees = el;
        return true;
    }

    /// The whole dome, row-major: one row per elevation starting at the HORIZON, azimuth increasing to
    /// the right. Horizon first because the sheet is read top to bottom and the sky is read bottom to
    /// top; a sheet whose first row is the zenith puts the two in opposite orders and is misread.
    inline std::vector<DomeSample> MakeDomePlan( const std::vector<int>& elevationsDegrees, int azimuthCount )
    {
        std::vector<DomeSample> plan;
        if ( azimuthCount < 1 )
            return plan;

        plan.reserve( elevationsDegrees.size() * static_cast<std::size_t>( azimuthCount ) );
        for ( const int elevation : elevationsDegrees )
        {
            for ( int column = 0; column < azimuthCount; ++column )
            {
                // Integer degrees, and the division is exact only when 360 divides the count. It is
                // rounded rather than truncated so that eight columns are 0/45/90/... and seven are
                // 0/51/103/... instead of 0/51/102/... — the label must name the ray that was actually
                // shot, and the ray is built from this same integer.
                const int azimuth = static_cast<int>(
                     std::lround( 360.0 * static_cast<double>( column ) / static_cast<double>( azimuthCount ) ) );
                plan.push_back( MakeDomeSample( azimuth, elevation ) );
            }
        }
        return plan;
    }

    // ---- the sheet -----------------------------------------------------------------------------------

    /// An 8-bit RGB image. Deliberately not stb's loader type: the layout code owns its own buffers so it
    /// can be exercised without a PNG on disk.
    struct Image
    {
        int                       Width  = 0;
        int                       Height = 0;
        std::vector<std::uint8_t> Pixels; // Width * Height * 3

        std::uint8_t* At( int x, int y )
        {
            return Pixels.data() + ( static_cast<std::size_t>( y ) * Width + x ) * 3;
        }
        const std::uint8_t* At( int x, int y ) const
        {
            return Pixels.data() + ( static_cast<std::size_t>( y ) * Width + x ) * 3;
        }
    };

    inline Image MakeImage( int width, int height, std::uint8_t fill )
    {
        Image image;
        image.Width  = std::max( width, 0 );
        image.Height = std::max( height, 0 );
        image.Pixels.assign( static_cast<std::size_t>( image.Width ) * image.Height * 3, fill );
        return image;
    }

    /// Shrink by an integer factor with a box filter.
    ///
    /// A box filter and not a point sample, because the thing this sheet is read for is whether a patch
    /// of sky has cloud in it at all. Point sampling a 4x reduction throws away fifteen sixteenths of the
    /// evidence and can drop a thin cirrus entirely — the sheet would then show an empty tile for a sky
    /// that is not empty, which is the exact failure the instrument exists to prevent.
    inline Image BoxDownscale( const Image& source, int factor )
    {
        if ( factor <= 1 )
            return source;

        const int width  = source.Width / factor;
        const int height = source.Height / factor;
        Image     out    = MakeImage( width, height, 0 );

        const int samples = factor * factor;
        for ( int y = 0; y < height; ++y )
        {
            for ( int x = 0; x < width; ++x )
            {
                int sum[3] = { 0, 0, 0 };
                for ( int dy = 0; dy < factor; ++dy )
                {
                    for ( int dx = 0; dx < factor; ++dx )
                    {
                        const std::uint8_t* p = source.At( x * factor + dx, y * factor + dy );
                        sum[0] += p[0];
                        sum[1] += p[1];
                        sum[2] += p[2];
                    }
                }
                std::uint8_t* q = out.At( x, y );
                // Rounded, not truncated: a truncating box filter darkens every reduction by half a
                // level, and this sheet is compared against frames that were not reduced.
                q[0] = static_cast<std::uint8_t>( ( sum[0] + samples / 2 ) / samples );
                q[1] = static_cast<std::uint8_t>( ( sum[1] + samples / 2 ) / samples );
                q[2] = static_cast<std::uint8_t>( ( sum[2] + samples / 2 ) / samples );
            }
        }
        return out;
    }

    /// Where a cell sits in the sheet. One function so the sheet's total size and the position of tile i
    /// cannot disagree — they are the two halves of the same arithmetic and were a pair of separate
    /// expressions in the first draft.
    struct SheetGeometry
    {
        int Columns       = 1;
        int Rows          = 1;
        int CellWidth     = 0;
        int CellHeight    = 0;
        int Gap           = 0;
        int CaptionHeight = 0;

        int SheetWidth() const
        {
            return Gap + Columns * ( CellWidth + Gap );
        }
        int SheetHeight() const
        {
            return CaptionHeight + Gap + Rows * ( CellHeight + Gap );
        }
        int CellX( int index ) const
        {
            return Gap + ( index % Columns ) * ( CellWidth + Gap );
        }
        int CellY( int index ) const
        {
            return CaptionHeight + Gap + ( index / Columns ) * ( CellHeight + Gap );
        }
    };

    inline SheetGeometry MakeSheetGeometry( int tileCount, int columns, int cellWidth, int cellHeight, int gap,
                                            int captionHeight )
    {
        SheetGeometry geometry;
        geometry.Columns       = std::max( columns, 1 );
        geometry.Rows          = ( std::max( tileCount, 0 ) + geometry.Columns - 1 ) / geometry.Columns;
        geometry.Rows          = std::max( geometry.Rows, 1 );
        geometry.CellWidth     = std::max( cellWidth, 0 );
        geometry.CellHeight    = std::max( cellHeight, 0 );
        geometry.Gap           = std::max( gap, 0 );
        geometry.CaptionHeight = std::max( captionHeight, 0 );
        return geometry;
    }

    inline void Blit( Image& destination, const Image& source, int x0, int y0 )
    {
        for ( int y = 0; y < source.Height; ++y )
        {
            const int dy = y0 + y;
            if ( dy < 0 || dy >= destination.Height )
                continue;
            for ( int x = 0; x < source.Width; ++x )
            {
                const int dx = x0 + x;
                if ( dx < 0 || dx >= destination.Width )
                    continue;
                const std::uint8_t* p = source.At( x, y );
                std::uint8_t*       q = destination.At( dx, dy );
                q[0]                  = p[0];
                q[1]                  = p[1];
                q[2]                  = p[2];
            }
        }
    }

    // ---- the text ------------------------------------------------------------------------------------

    // A 5x7 glyph per character, column-major, bit 0 the top row. ASCII 32 ('  ') to 96 ('`'), which is
    // every character DomeLabel and the caption can produce; lower case folds to upper and anything else
    // becomes '?'. Vendoring five bytes a glyph is what keeps this instrument dependency-free like the
    // three next to it — a label that needs a font file is a label that is missing on the machine where
    // it matters.
    inline constexpr std::uint8_t kFont5x7[65][5] = {
         { 0x00, 0x00, 0x00, 0x00, 0x00 }, // ' '
         { 0x00, 0x00, 0x5F, 0x00, 0x00 }, // !
         { 0x00, 0x07, 0x00, 0x07, 0x00 }, // "
         { 0x14, 0x7F, 0x14, 0x7F, 0x14 }, // #
         { 0x24, 0x2A, 0x7F, 0x2A, 0x12 }, // $
         { 0x23, 0x13, 0x08, 0x64, 0x62 }, // %
         { 0x36, 0x49, 0x55, 0x22, 0x50 }, // &
         { 0x00, 0x05, 0x03, 0x00, 0x00 }, // '
         { 0x00, 0x1C, 0x22, 0x41, 0x00 }, // (
         { 0x00, 0x41, 0x22, 0x1C, 0x00 }, // )
         { 0x14, 0x08, 0x3E, 0x08, 0x14 }, // *
         { 0x08, 0x08, 0x3E, 0x08, 0x08 }, // +
         { 0x00, 0x50, 0x30, 0x00, 0x00 }, // ,
         { 0x08, 0x08, 0x08, 0x08, 0x08 }, // -
         { 0x00, 0x60, 0x60, 0x00, 0x00 }, // .
         { 0x20, 0x10, 0x08, 0x04, 0x02 }, // /
         { 0x3E, 0x51, 0x49, 0x45, 0x3E }, // 0
         { 0x00, 0x42, 0x7F, 0x40, 0x00 }, // 1
         { 0x42, 0x61, 0x51, 0x49, 0x46 }, // 2
         { 0x21, 0x41, 0x45, 0x4B, 0x31 }, // 3
         { 0x18, 0x14, 0x12, 0x7F, 0x10 }, // 4
         { 0x27, 0x45, 0x45, 0x45, 0x39 }, // 5
         { 0x3C, 0x4A, 0x49, 0x49, 0x30 }, // 6
         { 0x01, 0x71, 0x09, 0x05, 0x03 }, // 7
         { 0x36, 0x49, 0x49, 0x49, 0x36 }, // 8
         { 0x06, 0x49, 0x49, 0x29, 0x1E }, // 9
         { 0x00, 0x36, 0x36, 0x00, 0x00 }, // :
         { 0x00, 0x56, 0x36, 0x00, 0x00 }, // ;
         { 0x08, 0x14, 0x22, 0x41, 0x00 }, // <
         { 0x14, 0x14, 0x14, 0x14, 0x14 }, // =
         { 0x00, 0x41, 0x22, 0x14, 0x08 }, // >
         { 0x02, 0x01, 0x51, 0x09, 0x06 }, // ?
         { 0x32, 0x49, 0x79, 0x41, 0x3E }, // @
         { 0x7E, 0x11, 0x11, 0x11, 0x7E }, // A
         { 0x7F, 0x49, 0x49, 0x49, 0x36 }, // B
         { 0x3E, 0x41, 0x41, 0x41, 0x22 }, // C
         { 0x7F, 0x41, 0x41, 0x22, 0x1C }, // D
         { 0x7F, 0x49, 0x49, 0x49, 0x41 }, // E
         { 0x7F, 0x09, 0x09, 0x09, 0x01 }, // F
         { 0x3E, 0x41, 0x49, 0x49, 0x7A }, // G
         { 0x7F, 0x08, 0x08, 0x08, 0x7F }, // H
         { 0x00, 0x41, 0x7F, 0x41, 0x00 }, // I
         { 0x20, 0x40, 0x41, 0x3F, 0x01 }, // J
         { 0x7F, 0x08, 0x14, 0x22, 0x41 }, // K
         { 0x7F, 0x40, 0x40, 0x40, 0x40 }, // L
         { 0x7F, 0x02, 0x0C, 0x02, 0x7F }, // M
         { 0x7F, 0x04, 0x08, 0x10, 0x7F }, // N
         { 0x3E, 0x41, 0x41, 0x41, 0x3E }, // O
         { 0x7F, 0x09, 0x09, 0x09, 0x06 }, // P
         { 0x3E, 0x41, 0x51, 0x21, 0x5E }, // Q
         { 0x7F, 0x09, 0x19, 0x29, 0x46 }, // R
         { 0x46, 0x49, 0x49, 0x49, 0x31 }, // S
         { 0x01, 0x01, 0x7F, 0x01, 0x01 }, // T
         { 0x3F, 0x40, 0x40, 0x40, 0x3F }, // U
         { 0x1F, 0x20, 0x40, 0x20, 0x1F }, // V
         { 0x3F, 0x40, 0x38, 0x40, 0x3F }, // W
         { 0x63, 0x14, 0x08, 0x14, 0x63 }, // X
         { 0x07, 0x08, 0x70, 0x08, 0x07 }, // Y
         { 0x61, 0x51, 0x49, 0x45, 0x43 }, // Z
         { 0x00, 0x7F, 0x41, 0x41, 0x00 }, // [
         { 0x02, 0x04, 0x08, 0x10, 0x20 }, // backslash
         { 0x00, 0x41, 0x41, 0x7F, 0x00 }, // ]
         { 0x04, 0x02, 0x01, 0x02, 0x04 }, // ^
         { 0x40, 0x40, 0x40, 0x40, 0x40 }, // _
         { 0x02, 0x05, 0x02, 0x00, 0x00 }, // ` — drawn as a DEGREE ring, which is the one non-ASCII
                                           //    glyph an angle label wants and the one a PNG can carry
                                           //    without a font file.
    };

    inline constexpr int kGlyphWidth   = 5;
    inline constexpr int kGlyphHeight  = 7;
    inline constexpr int kGlyphAdvance = 6; // one blank column between glyphs

    /// The five column bytes of @p character, folded to the table's range.
    inline const std::uint8_t* GlyphFor( char character )
    {
        unsigned char c = static_cast<unsigned char>( character );
        if ( c >= 'a' && c <= 'z' )
            c = static_cast<unsigned char>( c - 'a' + 'A' );
        if ( c < 32 || c > 96 )
            c = '?';
        return kFont5x7[c - 32];
    }

    /// Width in pixels of @p text at @p scale, trailing advance removed.
    inline int TextWidth( const std::string& text, int scale )
    {
        if ( text.empty() )
            return 0;
        return ( static_cast<int>( text.size() ) * kGlyphAdvance - 1 ) * std::max( scale, 1 );
    }

    /// Darken a rectangle toward black by @p amount in [0,1] — the backing the label is drawn on.
    ///
    /// A bar rather than an outline, and DARKENED rather than filled: a solid bar hides a strip of the
    /// tile, and on the horizon rows that strip is sky the reader needs. Darkening keeps it legible
    /// underneath while still giving the white glyphs something to sit on over a bright cloud top.
    inline void DarkenRect( Image& image, int x0, int y0, int width, int height, float amount )
    {
        const float keep = std::clamp( 1.0f - amount, 0.0f, 1.0f );
        for ( int y = y0; y < y0 + height; ++y )
        {
            if ( y < 0 || y >= image.Height )
                continue;
            for ( int x = x0; x < x0 + width; ++x )
            {
                if ( x < 0 || x >= image.Width )
                    continue;
                std::uint8_t* p = image.At( x, y );
                for ( int channel = 0; channel < 3; ++channel )
                    // std::lround, not `+ 0.5f` truncation: `keep` is a fraction so the product is never
                    // negative here, but the two forms are indistinguishable at the site and only one of
                    // them stays right if it ever is. One instruction either way (fcvtas / cvtss2si).
                    p[channel] =
                         static_cast<std::uint8_t>( std::lround( static_cast<float>( p[channel] ) * keep ) );
            }
        }
    }

    inline void DrawText( Image& image, const std::string& text, int x0, int y0, int scale, std::uint8_t r,
                          std::uint8_t g, std::uint8_t b )
    {
        const int s   = std::max( scale, 1 );
        int       pen = x0;
        for ( const char character : text )
        {
            const std::uint8_t* glyph = GlyphFor( character );
            for ( int column = 0; column < kGlyphWidth; ++column )
            {
                for ( int row = 0; row < kGlyphHeight; ++row )
                {
                    if ( ( glyph[column] & ( 1u << row ) ) == 0u )
                        continue;
                    for ( int dy = 0; dy < s; ++dy )
                    {
                        for ( int dx = 0; dx < s; ++dx )
                        {
                            const int x = pen + column * s + dx;
                            const int y = y0 + row * s + dy;
                            if ( x < 0 || x >= image.Width || y < 0 || y >= image.Height )
                                continue;
                            std::uint8_t* p = image.At( x, y );
                            p[0]            = r;
                            p[1]            = g;
                            p[2]            = b;
                        }
                    }
                }
            }
            pen += kGlyphAdvance * s;
        }
    }

    /// Burn a label into the top-left of the cell at (@p x0, @p y0) of @p width pixels.
    inline void StampLabel( Image& image, const std::string& text, int x0, int y0, int width, int scale )
    {
        const int s       = std::max( scale, 1 );
        const int pad     = 2 * s;
        const int barHigh = kGlyphHeight * s + 2 * pad;
        DarkenRect( image, x0, y0, width, barHigh, 0.62f );
        DrawText( image, text, x0 + pad, y0 + pad, s, 255, 255, 255 );
    }
} // namespace Desert::DomeSheet
