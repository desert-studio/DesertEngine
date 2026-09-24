// Ported from UE 5.8 Engine/Source/Editor/LandscapeEditor/Private/LandscapeEdModeMirrorTool.cpp:332-510
// (ApplyMirrorInternal), :519-650 (ApplyMirror) and :681-693 (CenterMirrorPoint),
// LandscapeEdModeComponentTools.cpp :963-1100 (FLandscapeToolStrokeCopy) and :1330-1530
// (FLandscapeToolStrokePaste, PasteMode), adapted: the mirror point and the copy corners come from palette
// commands instead of the transform widget and the gizmo; the copy is a rectangle held as exact sample offsets
// above its lowest sample, which is where UE's FitToSelection puts the gizmo's Z (LandscapeEdit.cpp:4813-4885,
// GetLandscapeCenterPos: MinZ - MarginZ; the margin cancels between copy and paste), instead of the gizmo's
// normalised heights (LandscapeGizmoActor.cpp:819-866: GetNormalizedHeight subtracts the gizmo's Z,
// GetLandscapeHeight adds the paste gizmo's Z back); the paste gizmo stands on the landscape sample nearest the
// paste point; weight layers, gizmo rotation/scale and the selection-ratio falloff are not ported (with a
// rectangle every ratio is 1); UE's silent returns on a bad mirror line or an empty copy are refusals; source
// samples beyond the landscape take the edge sample; every write goes through the stroke's height cache, so one
// command undoes in one step.

#include <Engine/World/Landscape/LandscapeSculpt.hpp>

#include <algorithm>
#include <cmath>
#include <string>

namespace Desert::World::Landscape
{
    namespace
    {
        LandscapeSampleBounds Clip( const LandscapeSampleBounds& a, const LandscapeSampleBounds& b )
        {
            return { std::max( a.X1, b.X1 ), std::max( a.Z1, b.Z1 ), std::min( a.X2, b.X2 ),
                     std::min( a.Z2, b.Z2 ) };
        }

        /// FMath::RoundToInt32 of a world coordinate on the lattice.
        int32_t LatticeRound( float cm, float originCm, float spacingCm )
        {
            return static_cast<int32_t>( std::floor( ( cm - originCm ) / spacingCm + 0.5f ) );
        }

        /// FMath::Lerp on uint16: (T)(A + Alpha * (B - A)), truncating.
        uint16_t LerpU16( uint16_t a, uint16_t b, float alpha )
        {
            return static_cast<uint16_t>( static_cast<float>( a ) +
                                          alpha * static_cast<float>( static_cast<int32_t>( b ) - a ) );
        }
    } // namespace

    Common::BoolResultStr LandscapeHeightStroke::ApplyMirror( std::optional<glm::vec3>       pointCm,
                                                              const LandscapeMirrorSettings& mirror )
    {
        using Op          = LandscapeMirrorOp;
        const Op   op     = mirror.Op;
        const bool alongX = op == Op::MinusXToPlusX || op == Op::PlusXToMinusX || op == Op::RotateMinusXToPlusX ||
                            op == Op::RotatePlusXToMinusX;
        const bool minusPlus = op == Op::MinusXToPlusX || op == Op::MinusZToPlusZ ||
                               op == Op::RotateMinusXToPlusX || op == Op::RotateMinusZToPlusZ;
        const bool rotate = op == Op::RotateMinusXToPlusX || op == Op::RotatePlusXToMinusX ||
                            op == Op::RotateMinusZToPlusZ || op == Op::RotatePlusZToMinusZ;
        if ( mirror.SmoothingWidth < 0 )
            return Common::MakeError( "landscape mirror: smoothing width " +
                                      std::to_string( mirror.SmoothingWidth ) + " is negative" );
        if ( m_Bounds.Empty() )
            return Common::MakeError( "landscape mirror: the landscape has no samples" );

        // "along" is the axis the mirror flips, "across" the other one.
        const int32_t lo       = alongX ? m_Bounds.X1 : m_Bounds.Z1;
        const int32_t hi       = alongX ? m_Bounds.X2 : m_Bounds.Z2;
        const int32_t acrossLo = alongX ? m_Bounds.Z1 : m_Bounds.X1;
        const int32_t across   = ( alongX ? m_Bounds.Z2 : m_Bounds.X2 ) - acrossLo + 1;
        int32_t       mirrorPos =
             pointCm ? LatticeRound( alongX ? pointCm->x : pointCm->z, alongX ? m_Root.Origin.x : m_Root.Origin.z,
                                     m_Root.SpacingCm )
                           : static_cast<int32_t>( std::floor( static_cast<float>( lo + hi ) / 2.0f + 0.5f ) );
        if ( mirrorPos <= lo || mirrorPos >= hi )
            return Common::MakeError( "landscape mirror: the mirror line at sample " +
                                      std::to_string( mirrorPos ) + " is not inside the landscape (" +
                                      std::to_string( lo ) + ".." + std::to_string( hi ) + ")" );
        const int32_t mirrorSize = std::max( hi - mirrorPos, mirrorPos - lo ); // not including the mirror line
        const int32_t blend =
             std::min( std::clamp( mirror.SmoothingWidth, 0, kLandscapeMaxMirrorSmoothing ), mirrorSize );
        // "extra column to calc normals for mirror column"
        const int32_t srcMin = minusPlus ? mirrorPos - mirrorSize : mirrorPos - blend;
        const int32_t srcMax = minusPlus ? mirrorPos + blend : mirrorPos + mirrorSize;
        const int32_t dstMin = minusPlus ? mirrorPos - blend - 1 : mirrorPos - mirrorSize;
        const int32_t dstMax = minusPlus ? mirrorPos + mirrorSize : mirrorPos + blend + 1;
        const int32_t pos    = mirrorPos - srcMin;
        const int32_t srcN   = srcMax - srcMin + 1;
        const int32_t dstN   = dstMax - dstMin + 1;

        const auto rectOf = [&]( int32_t a1, int32_t a2 ) -> LandscapeSampleBounds
        {
            return alongX ? LandscapeSampleBounds{ a1, m_Bounds.Z1, a2, m_Bounds.Z2 }
                          : LandscapeSampleBounds{ m_Bounds.X1, a1, m_Bounds.X2, a2 };
        };
        const LandscapeSampleBounds dstRect = Clip( rectOf( dstMin, dstMax ), m_Bounds );
        const LandscapeSampleBounds all =
             Clip( rectOf( std::min( srcMin, dstMin ), std::max( srcMax, dstMax ) ), m_Bounds );
        auto cached = Cache( all );
        if ( !cached.IsSuccess() )
            return cached;
        auto read = m_Cache.GetCachedData( all.X1, all.Z1, all.X2, all.Z2 );
        if ( !read.IsSuccess() )
            return Common::MakeError( read.GetError() );
        const std::vector<uint16_t>& data  = read.GetValue();
        const int32_t                width = all.X2 - all.X1 + 1;
        const auto at = [&]( int32_t along, int32_t acrossIdx ) // global along, across index; edge-clamped
        {
            const int32_t g = std::clamp( along, alongX ? all.X1 : all.Z1, alongX ? all.X2 : all.Z2 );
            const int32_t c = acrossLo + acrossIdx;
            return alongX ? data[static_cast<size_t>( ( c - all.Z1 ) * width + ( g - all.X1 ) )]
                          : data[static_cast<size_t>( ( g - all.Z1 ) * width + ( c - all.X1 ) )];
        };

        std::vector<uint16_t> dst( static_cast<size_t>( dstN ) * static_cast<size_t>( across ) );
        for ( int32_t a = 0; a < across; ++a )
        {
            const int32_t a2    = rotate ? across - a - 1 : a; // SourceLine2: the flipped line for Rotate
            const auto    src1  = [&]( int32_t s ) { return at( srcMin + s, a ); };
            const auto    src2  = [&]( int32_t s ) { return at( srcMin + s, a2 ); };
            uint16_t*     line  = dst.data() + static_cast<size_t>( a ) * static_cast<size_t>( dstN );
            const int32_t sizeN = minusPlus ? dstN : srcN;
            const int32_t start = ( sizeN - pos - 1 ) - blend; // BlendStart
            const int32_t end   = start + 2 * blend + 1;       // BlendEnd
            const int32_t off   = 2 * pos - sizeN + 1;         // Offset
            int32_t       d     = 0;
            if ( minusPlus )
            {
                for ( int32_t s = off; d < start; ++d, ++s )
                    line[d] = src1( s );
                for ( int32_t s1 = start + off, s2 = end + off - 1; d < end; ++d, ++s1, --s2 )
                {
                    const float frac = static_cast<float>( d - start + 1 ) / static_cast<float>( end - start + 1 );
                    const float alpha = std::cos( frac * 3.14159265358979323846f ) * -0.5f + 0.5f;
                    line[d]           = LerpU16( src1( s1 ), src2( s2 ), alpha );
                }
                for ( int32_t s = start + off - 1; d < dstN; ++d, --s )
                    line[d] = src2( s );
            }
            else
            {
                for ( int32_t s = srcN - 1; d < start; ++d, --s )
                    line[d] = src2( s );
                for ( int32_t s1 = start + off, s2 = end + off - 1; d < end; ++d, ++s1, --s2 )
                {
                    const float frac = static_cast<float>( d - start + 1 ) / static_cast<float>( end - start + 1 );
                    const float alpha = std::cos( frac * 3.14159265358979323846f ) * -0.5f + 0.5f;
                    line[d]           = LerpU16( src2( s2 ), src1( s1 ), alpha );
                }
                for ( int32_t s = end + off; d < dstN; ++d, ++s )
                    line[d] = src1( s );
            }
        }

        // SetHeightData over the destination, clipped to the landscape.
        std::vector<uint16_t> out;
        out.reserve( static_cast<size_t>( dstRect.X2 - dstRect.X1 + 1 ) *
                     static_cast<size_t>( dstRect.Z2 - dstRect.Z1 + 1 ) );
        for ( int32_t z = dstRect.Z1; z <= dstRect.Z2; ++z )
            for ( int32_t x = dstRect.X1; x <= dstRect.X2; ++x )
            {
                const int32_t d = ( alongX ? x : z ) - dstMin;
                const int32_t a = ( alongX ? z : x ) - acrossLo;
                out.push_back(
                     dst[static_cast<size_t>( a ) * static_cast<size_t>( dstN ) + static_cast<size_t>( d )] );
            }
        return m_Cache.SetCachedData( dstRect.X1, dstRect.Z1, dstRect.X2, dstRect.Z2, out );
    }

    Common::ResultStr<LandscapeCopyBuffer> CopyLandscapeHeights( const LandscapeRoot&         root,
                                                                 LandscapeTileLookup          lookup,
                                                                 const LandscapeSampleBounds& bounds,
                                                                 glm::vec3 cornerACm, glm::vec3 cornerBCm )
    {
        const int32_t               ax = LatticeRound( cornerACm.x, root.Origin.x, root.SpacingCm );
        const int32_t               az = LatticeRound( cornerACm.z, root.Origin.z, root.SpacingCm );
        const int32_t               bx = LatticeRound( cornerBCm.x, root.Origin.x, root.SpacingCm );
        const int32_t               bz = LatticeRound( cornerBCm.z, root.Origin.z, root.SpacingCm );
        const LandscapeSampleBounds rect =
             Clip( { std::min( ax, bx ), std::min( az, bz ), std::max( ax, bx ), std::max( az, bz ) }, bounds );
        if ( rect.Empty() )
            return Common::MakeError<LandscapeCopyBuffer>(
                 "landscape copy: the region lies outside the landscape" );
        LandscapeHeightCache cache( root, std::move( lookup ) );
        auto                 cached = cache.CacheData( rect.X1, rect.Z1, rect.X2, rect.Z2 );
        if ( !cached.IsSuccess() )
            return Common::MakeError<LandscapeCopyBuffer>( cached.GetError() );
        auto read = cache.GetCachedData( rect.X1, rect.Z1, rect.X2, rect.Z2 );
        if ( !read.IsSuccess() )
            return Common::MakeError<LandscapeCopyBuffer>( read.GetError() );
        LandscapeCopyBuffer buffer;
        buffer.SizeX                   = rect.X2 - rect.X1 + 1;
        buffer.SizeZ                   = rect.Z2 - rect.Z1 + 1;
        const std::vector<uint16_t>& v = read.GetValue();
        // The copy gizmo's Z: the region's lowest sample (UE GetLandscapeCenterPos), so every offset is >= 0 as
        // UE's clamped normalised height is.
        const int32_t base = *std::min_element( v.begin(), v.end() );
        buffer.Relative.reserve( v.size() );
        for ( const uint16_t h : v )
            buffer.Relative.push_back( static_cast<int32_t>( h ) - base );
        return Common::MakeSuccess( std::move( buffer ) );
    }

    Common::BoolResultStr LandscapeHeightStroke::ApplyPaste( const LandscapeCopyBuffer& buffer, glm::vec3 atCm,
                                                             LandscapePasteMode            mode,
                                                             const LandscapeBrushSettings& brush )
    {
        if ( buffer.Empty() ||
             buffer.Relative.size() != static_cast<size_t>( buffer.SizeX ) * static_cast<size_t>( buffer.SizeZ ) )
            return Common::MakeError( "landscape paste: nothing has been copied" );
        const int32_t cx = LatticeRound( atCm.x, m_Root.Origin.x, m_Root.SpacingCm );
        const int32_t cz = LatticeRound( atCm.z, m_Root.Origin.z, m_Root.SpacingCm );
        if ( cx < m_Bounds.X1 || cx > m_Bounds.X2 || cz < m_Bounds.Z1 || cz > m_Bounds.Z2 )
            return Common::MakeError( "landscape paste: the paste point (" + std::to_string( cx ) + ", " +
                                      std::to_string( cz ) + ") is outside the landscape" );
        const int32_t               x1 = cx - buffer.SizeX / 2;
        const int32_t               z1 = cz - buffer.SizeZ / 2;
        const LandscapeSampleBounds rect =
             Clip( { x1, z1, x1 + buffer.SizeX - 1, z1 + buffer.SizeZ - 1 }, m_Bounds );
        // UE's circle brush centred on the paste point: PaintAmount = BrushValue · ToolStrength, so the falloff
        // carries the pasted heights into the relief instead of standing them on a wall.
        const glm::vec2 centre( atCm.x, atCm.z );
        auto            weights = ComputeLandscapeBrush( m_Root, brush, std::span<const glm::vec2>( &centre, 1 ) );
        if ( !weights.IsSuccess() )
            return Common::MakeError( "landscape paste: " + weights.GetError() );
        auto cached = Cache( rect );
        if ( !cached.IsSuccess() )
            return cached;
        auto read = m_Cache.GetCachedData( rect.X1, rect.Z1, rect.X2, rect.Z2 );
        if ( !read.IsSuccess() )
            return Common::MakeError( read.GetError() );
        std::vector<uint16_t> data  = read.GetValue();
        const int32_t         width = rect.X2 - rect.X1 + 1;
        // The paste gizmo's Z (LandscapeEdModeComponentTools.cpp:1501, GetLandscapeHeight): the height under the
        // paste point, so the copy's lowest sample lands there and the rest stand above it.
        const int32_t         base  = data[static_cast<size_t>( ( cz - rect.Z1 ) * width + ( cx - rect.X1 ) )];
        for ( int32_t z = rect.Z1; z <= rect.Z2; ++z )
            for ( int32_t x = rect.X1; x <= rect.X2; ++x )
            {
                uint16_t&     value = data[static_cast<size_t>( ( z - rect.Z1 ) * width + ( x - rect.X1 ) )];
                const int32_t rel = buffer.Relative[static_cast<size_t>( ( z - z1 ) * buffer.SizeX + ( x - x1 ) )];
                const uint16_t dest = static_cast<uint16_t>( std::clamp( base + rel, 0, 65535 ) );
                // Lerp(Original, Dest, PaintAmount) truncated to uint16 as UE does; the amount is clamped to
                // [0, 1] because a strength above 1 would extrapolate past Dest and a float outside uint16 does
                // not convert.
                const float paint = std::clamp( weights.GetValue().At( x, z ), 0.0f, 1.0f );
                if ( paint > 0.0f &&
                     ( mode == LandscapePasteMode::Both || ( mode == LandscapePasteMode::Raise && value < dest ) ||
                       ( mode == LandscapePasteMode::Lower && value > dest ) ) )
                    value = static_cast<uint16_t>( static_cast<float>( value ) +
                                                   ( static_cast<float>( dest ) - static_cast<float>( value ) ) *
                                                        paint );
            }
        return m_Cache.SetCachedData( rect.X1, rect.Z1, rect.X2, rect.Z2, data );
    }
} // namespace Desert::World::Landscape
