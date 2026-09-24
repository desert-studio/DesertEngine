// Ported from UE 5.8 Engine/Source/Editor/LandscapeEditor/Private/LandscapeEdModeBrushes.cpp:355-455,1009-1135,
// adapted: see LandscapeBrush.hpp.

#include <Engine/World/Landscape/LandscapeBrush.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace Desert::World::Landscape
{
    namespace
    {
        /// Floor division for signed global indices: tile -1 holds global samples -Q .. 0.
        int32_t FloorDiv( int32_t value, int32_t divisor )
        {
            const int32_t q = value / divisor;
            return ( value % divisor != 0 && ( value < 0 ) != ( divisor < 0 ) ) ? q - 1 : q;
        }

        /// UE: FLandscapeBrushCircle::CapInteractorPositions — scale so the first and last are kept.
        std::vector<glm::vec2> CapPositions( std::span<const glm::vec2> positions )
        {
            if ( positions.size() <= kLandscapeBrushMaxPositions )
                return { positions.begin(), positions.end() };
            std::vector<glm::vec2> out;
            out.reserve( kLandscapeBrushMaxPositions );
            for ( size_t i = 0; i < kLandscapeBrushMaxPositions; ++i )
                out.push_back(
                     positions[( i * ( positions.size() - 1u ) ) / ( kLandscapeBrushMaxPositions - 1u )] );
            return out;
        }
    } // namespace

    float LandscapeBrushFalloffWeight( LandscapeBrushFalloff shape, float distance, float radius, float falloff )
    {
        switch ( shape )
        {
            case LandscapeBrushFalloff::Linear:
                return distance < radius ? 1.0f
                       : falloff > 0.0f  ? std::max( 0.0f, 1.0f - ( distance - radius ) / falloff )
                                         : 0.0f;
            case LandscapeBrushFalloff::Smooth:
            {
                const float y =
                     LandscapeBrushFalloffWeight( LandscapeBrushFalloff::Linear, distance, radius, falloff );
                return y * y * ( 3.0f - 2.0f * y );
            }
            case LandscapeBrushFalloff::Spherical:
            {
                if ( distance <= radius )
                    return 1.0f;
                if ( distance > radius + falloff )
                    return 0.0f;
                const float t = ( distance - radius ) / falloff;
                return std::sqrt( 1.0f - t * t );
            }
            case LandscapeBrushFalloff::Tip:
            {
                if ( distance <= radius )
                    return 1.0f;
                if ( distance > radius + falloff )
                    return 0.0f;
                const float t = ( falloff + radius - distance ) / falloff;
                return 1.0f - std::sqrt( 1.0f - t * t );
            }
        }
        return 0.0f;
    }

    Common::BoolResultStr ValidateLandscapeBrush( const LandscapeBrushSettings& settings )
    {
        if ( !std::isfinite( settings.RadiusCm ) || settings.RadiusCm <= 0.0f )
            return Common::MakeFormattedError<bool>(
                 "Landscape brush radius must be positive and finite, got {} cm", settings.RadiusCm );
        if ( !( settings.FalloffFraction >= 0.0f && settings.FalloffFraction <= 1.0f ) )
            return Common::MakeFormattedError<bool>( "Landscape brush falloff must be within [0, 1], got {}",
                                                     settings.FalloffFraction );
        if ( !std::isfinite( settings.Strength ) || settings.Strength < 0.0f )
            return Common::MakeFormattedError<bool>(
                 "Landscape brush strength must be non-negative and finite, got {}", settings.Strength );
        return Common::MakeSuccess( true );
    }

    float LandscapeBrushWeights::At( int32_t gx, int32_t gz ) const
    {
        const int64_t lx = static_cast<int64_t>( gx ) - X0;
        const int64_t lz = static_cast<int64_t>( gz ) - Z0;
        if ( lx < 0 || lz < 0 || lx >= Width || lz >= Depth )
            return 0.0f;
        return Values[static_cast<size_t>( lz ) * Width + static_cast<size_t>( lx )];
    }

    Common::ResultStr<LandscapeBrushWeights> ComputeLandscapeBrush( const LandscapeRoot&          root,
                                                                    const LandscapeBrushSettings& settings,
                                                                    std::span<const glm::vec2>    positionsCm )
    {
        if ( auto valid = ValidateLandscapeRoot( root ); !valid )
            return Common::MakeError<LandscapeBrushWeights>( valid.GetError() );
        if ( auto valid = ValidateLandscapeBrush( settings ); !valid )
            return Common::MakeError<LandscapeBrushWeights>( valid.GetError() );

        // UE: TotalRadius = BrushRadius / DrawScale.X; the inner circle and the falloff split it by BrushFalloff.
        const float totalRadius = settings.RadiusCm / root.SpacingCm;
        const float radius      = ( 1.0f - settings.FalloffFraction ) * totalRadius;
        const float falloff     = settings.FalloffFraction * totalRadius;

        // Centres in lattice units. In double first: a centre 1e8 cm out loses whole samples in a float subtract.
        std::vector<glm::vec2> centres = CapPositions( positionsCm );
        for ( glm::vec2& c : centres )
        {
            if ( !std::isfinite( c.x ) || !std::isfinite( c.y ) )
                return Common::MakeFormattedError<LandscapeBrushWeights>(
                     "Landscape brush position must be finite, got ({}, {})", c.x, c.y );
            c.x = static_cast<float>( ( static_cast<double>( c.x ) - root.Origin.x ) / root.SpacingCm );
            c.y = static_cast<float>( ( static_cast<double>( c.y ) - root.Origin.z ) / root.SpacingCm );
        }

        // UE's SpotExclusiveBounds, one per centre; the result covers their union.
        struct Spot
        {
            int32_t MinX, MinZ, EndX, EndZ;
        };
        std::vector<Spot> spots;
        spots.reserve( centres.size() );
        LandscapeBrushWeights out;
        int32_t               endX = std::numeric_limits<int32_t>::min();
        int32_t               endZ = std::numeric_limits<int32_t>::min();
        out.X0                     = std::numeric_limits<int32_t>::max();
        out.Z0                     = std::numeric_limits<int32_t>::max();
        for ( const glm::vec2& c : centres )
        {
            const Spot s{ static_cast<int32_t>( std::floor( c.x - totalRadius ) ),
                          static_cast<int32_t>( std::floor( c.y - totalRadius ) ),
                          static_cast<int32_t>( std::ceil( c.x + totalRadius ) ) + 1,
                          static_cast<int32_t>( std::ceil( c.y + totalRadius ) ) + 1 };
            spots.push_back( s );
            out.X0 = std::min( out.X0, s.MinX );
            out.Z0 = std::min( out.Z0, s.MinZ );
            endX   = std::max( endX, s.EndX );
            endZ   = std::max( endZ, s.EndZ );
        }
        if ( centres.empty() )
            return Common::MakeSuccess( LandscapeBrushWeights{} );

        out.Width = static_cast<uint32_t>( endX - out.X0 );
        out.Depth = static_cast<uint32_t>( endZ - out.Z0 );
        out.Values.assign( static_cast<size_t>( out.Width ) * out.Depth, 0.0f );

        for ( size_t i = 0; i < centres.size(); ++i )
        {
            const glm::vec2& c = centres[i];
            const Spot&      s = spots[i];
            for ( int32_t z = s.MinZ; z < s.EndZ; ++z )
            {
                float* row = out.Values.data() + static_cast<size_t>( z - out.Z0 ) * out.Width;
                for ( int32_t x = s.MinX; x < s.EndX; ++x )
                {
                    float& prev = row[x - out.X0];
                    if ( prev >= 1.0f )
                        continue;
                    const float dx     = c.x - static_cast<float>( x );
                    const float dz     = c.y - static_cast<float>( z );
                    const float amount = LandscapeBrushFalloffWeight(
                         settings.Shape, std::sqrt( dx * dx + dz * dz ), radius, falloff );
                    // Larger wins across centres, as UE's Scanline[X] = max(PrevAmount, PaintAmount).
                    if ( amount > prev )
                        prev = amount;
                }
            }
        }

        // Strength after the union: a constant factor commutes with the max.
        if ( settings.Strength != 1.0f )
            for ( float& v : out.Values )
                v *= settings.Strength;
        return Common::MakeSuccess( std::move( out ) );
    }

    std::vector<LandscapeBrushTile> LandscapeBrushTiles( const LandscapeRoot&         root,
                                                         const LandscapeBrushWeights& weights )
    {
        std::vector<LandscapeBrushTile> tiles;
        if ( weights.Empty() )
            return tiles;
        const int32_t q    = static_cast<int32_t>( root.QuadsPerTile );
        const int32_t endX = weights.X0 + static_cast<int32_t>( weights.Width );
        const int32_t endZ = weights.Z0 + static_cast<int32_t>( weights.Depth );
        // Tile t holds global samples t·Q .. t·Q + Q inclusive, so sample g lies in tiles ceil(g/Q) - 1 ..
        // floor(g/Q).
        const int32_t firstX = FloorDiv( weights.X0 - 1, q );
        const int32_t firstZ = FloorDiv( weights.Z0 - 1, q );
        const int32_t lastX  = FloorDiv( endX - 1, q );
        const int32_t lastZ  = FloorDiv( endZ - 1, q );
        for ( int32_t tz = firstZ; tz <= lastZ; ++tz )
            for ( int32_t tx = firstX; tx <= lastX; ++tx )
            {
                const int32_t baseX = tx * q;
                const int32_t baseZ = tz * q;
                LandscapeRect rect;
                rect.X0 = static_cast<uint32_t>( std::max( weights.X0, baseX ) - baseX );
                rect.Z0 = static_cast<uint32_t>( std::max( weights.Z0, baseZ ) - baseZ );
                rect.X1 = static_cast<uint32_t>( std::min( endX, baseX + q + 1 ) - baseX );
                rect.Z1 = static_cast<uint32_t>( std::min( endZ, baseZ + q + 1 ) - baseZ );
                if ( !rect.Empty() )
                    tiles.push_back( { tx, tz, rect } );
            }
        return tiles;
    }

    float LandscapeBrushTileWeight( const LandscapeRoot& root, const LandscapeBrushWeights& weights, int32_t tileX,
                                    int32_t tileZ, uint32_t x, uint32_t z )
    {
        const int32_t q = static_cast<int32_t>( root.QuadsPerTile );
        return weights.At( tileX * q + static_cast<int32_t>( x ), tileZ * q + static_cast<int32_t>( z ) );
    }
} // namespace Desert::World::Landscape
