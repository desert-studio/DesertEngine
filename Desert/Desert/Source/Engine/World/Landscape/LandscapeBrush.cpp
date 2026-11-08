// Ported from UE 5.8 Engine/Source/Editor/LandscapeEditor/Private/LandscapeEdModeBrushes.cpp:355-455,1009-1135,
// adapted: see LandscapeBrush.hpp.

#include <Engine/World/Landscape/LandscapeBrush.hpp>

#include <glm/geometric.hpp>
#include <glm/trigonometric.hpp>

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

        /// UE's FVector2D::GetRotated: @p v turned by @p degrees.
        glm::vec2 Rotated( glm::vec2 v, float degrees )
        {
            const float theta = glm::radians( degrees );
            const float c     = std::cos( theta );
            const float s     = std::sin( theta );
            return { c * v.x - s * v.y, s * v.x + c * v.y };
        }

        /// UE's FLandscapeBrushAlpha / FLandscapeBrushAlphaPattern::ApplyBrush around one centre @p c (lattice
        /// units). The rectangle is UE's ExclusiveBounds: floor(c - R) .. ceil(c + R) + 1.
        LandscapeBrushWeights AlphaBrushWeights( const LandscapeRoot& root, const LandscapeBrushSettings& s,
                                                 glm::vec2 c, float totalRadius, float radius, float falloff )
        {
            const LandscapeBrushAlphaTexture& tex   = *s.AlphaTexture;
            const float                       sizeX = static_cast<float>( tex.Width );
            const float                       sizeY = static_cast<float>( tex.Height );

            LandscapeBrushWeights out;
            out.X0             = static_cast<int32_t>( std::floor( c.x - totalRadius ) );
            out.Z0             = static_cast<int32_t>( std::floor( c.y - totalRadius ) );
            const int32_t endX = static_cast<int32_t>( std::ceil( c.x + totalRadius ) ) + 1;
            const int32_t endZ = static_cast<int32_t>( std::ceil( c.y + totalRadius ) ) + 1;
            out.Width          = static_cast<uint32_t>( endX - out.X0 );
            out.Depth          = static_cast<uint32_t>( endZ - out.Z0 );
            out.Values.assign( static_cast<size_t>( out.Width ) * out.Depth, 0.0f );

            if ( s.Type == LandscapeBrushType::Alpha )
            {
                // ComputeAlphaBrushScaleAndRadius: the longer side fits the square inscribed in the circle.
                const float maxSize = 2.0f * std::sqrt( totalRadius * totalRadius / 2.0f );
                const float scale   = maxSize / std::max( sizeX, sizeY );
                const float angle   = glm::radians( s.AlphaRotationDeg );
                const float cosA    = std::cos( angle );
                const float sinA    = std::sin( angle );
                for ( int32_t z = out.Z0; z < endZ; ++z )
                {
                    float* row = out.Values.data() + static_cast<size_t>( z - out.Z0 ) * out.Width;
                    for ( int32_t x = out.X0; x < endX; ++x )
                    {
                        const float sx = ( static_cast<float>( x ) - c.x ) / scale;
                        const float sy = ( static_cast<float>( z ) - c.y ) / scale;
                        const float tx = sx * cosA - sy * sinA + sizeX * 0.5f;
                        const float ty = sy * cosA + sx * sinA + sizeY * 0.5f;
                        if ( tx >= 0.0f && tx <= sizeX - 1.0f && ty >= 0.0f && ty <= sizeY - 1.0f )
                            row[x - out.X0] = LandscapeBrushAlphaSample( tex, tx, ty );
                    }
                }
                return out;
            }

            // Pattern: the sample position is a function of the lattice position alone, so it is hoisted.
            glm::vec2 scale;
            glm::vec2 bias;
            float     angleDeg = 0.0f;
            if ( s.PatternWorldSpace )
            {
                const float spacing     = static_cast<float>( root.SpacingCm );
                glm::vec2   localOrigin = -glm::vec2(
                     static_cast<float>( ( static_cast<double>( s.PatternOriginCm.x ) - root.Origin.x ) /
                                         root.SpacingCm ),
                     static_cast<float>( ( static_cast<double>( s.PatternOriginCm.y ) - root.Origin.z ) /
                                         root.SpacingCm ) );
                const glm::vec2 localScale( spacing / ( s.PatternRepeatCm * ( sizeX / sizeY ) ),
                                            spacing / s.PatternRepeatCm );
                localOrigin *= localScale;
                angleDeg = -s.PatternWorldRotationDeg;
                if ( s.PatternCenterOnOrigin )
                    localOrigin += Rotated( glm::vec2( 0.5f, 0.5f ), -angleDeg );
                scale = glm::vec2( sizeX, sizeY ) * localScale;
                bias  = glm::vec2( sizeX, sizeY ) * localOrigin;
            }
            else
            {
                scale    = glm::vec2( 1.0f / s.PatternScale );
                bias     = glm::vec2( sizeX * s.PatternPanU, sizeY * s.PatternPanV );
                angleDeg = s.AlphaRotationDeg;
            }
            for ( int32_t z = out.Z0; z < endZ; ++z )
            {
                float* row = out.Values.data() + static_cast<size_t>( z - out.Z0 ) * out.Width;
                for ( int32_t x = out.X0; x < endX; ++x )
                {
                    const glm::vec2 p =
                         Rotated( glm::vec2( static_cast<float>( x ), static_cast<float>( z ) ) * scale + bias,
                                  angleDeg );
                    float mx = std::fmod( p.x, sizeX );
                    float my = std::fmod( p.y, sizeY );
                    if ( mx < 0.0f )
                        mx += sizeX;
                    if ( my < 0.0f )
                        my += sizeY;
                    const float alpha = LandscapeBrushAlphaSample( tex, mx, my );
                    const float dx    = c.x - static_cast<float>( x );
                    const float dz    = c.y - static_cast<float>( z );
                    // The pattern brush derives from FLandscapeBrushCircle_Smooth: Smooth, whatever Shape says.
                    row[x - out.X0] = LandscapeBrushFalloffWeight( LandscapeBrushFalloff::Smooth,
                                                                   std::sqrt( dx * dx + dz * dz ), radius, falloff ) *
                                      alpha;
                }
            }
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
        if ( settings.Type != LandscapeBrushType::Circle )
        {
            // UE's CanPaint: an alpha brush with no valid alpha data does not paint. Here it says why.
            const LandscapeBrushAlphaTexture* tex = settings.AlphaTexture.get();
            if ( tex == nullptr )
                return Common::MakeError<bool>( std::string(
                     "Landscape alpha / pattern brush has no alpha texture; choose one with 'Landscape: Alpha "
                     "texture: <name>' or switch to 'Landscape: Brush type: Circle'" ) );
            if ( tex->Width == 0u || tex->Height == 0u ||
                 tex->Values.size() != static_cast<size_t>( tex->Width ) * tex->Height )
                return Common::MakeFormattedError<bool>(
                     "Landscape alpha texture '{}' holds {} values for {} x {} texels", tex->Source,
                     tex->Values.size(), tex->Width, tex->Height );
            if ( !std::isfinite( settings.AlphaRotationDeg ) )
                return Common::MakeFormattedError<bool>( "Landscape brush texture rotation must be finite, got {}",
                                                         settings.AlphaRotationDeg );
            if ( settings.Type == LandscapeBrushType::Pattern )
            {
                if ( !( settings.PatternScale >= 0.005f && settings.PatternScale <= 5.0f ) )
                    return Common::MakeFormattedError<bool>(
                         "Landscape pattern texture scale must be within [0.005, 5], got {}", settings.PatternScale );
                if ( !( settings.PatternPanU >= 0.0f && settings.PatternPanU <= 1.0f && settings.PatternPanV >= 0.0f &&
                        settings.PatternPanV <= 1.0f ) )
                    return Common::MakeFormattedError<bool>(
                         "Landscape pattern texture pan must be within [0, 1], got ({}, {})", settings.PatternPanU,
                         settings.PatternPanV );
                if ( settings.PatternWorldSpace &&
                     ( !std::isfinite( settings.PatternRepeatCm ) || settings.PatternRepeatCm <= 0.0f ||
                       !std::isfinite( settings.PatternOriginCm.x ) || !std::isfinite( settings.PatternOriginCm.y ) ||
                       !std::isfinite( settings.PatternWorldRotationDeg ) ) )
                    return Common::MakeFormattedError<bool>(
                         "Landscape world-space pattern needs a positive repeat size and a finite origin and rotation, "
                         "got repeat {} cm, origin ({}, {}) cm, rotation {}",
                         settings.PatternRepeatCm, settings.PatternOriginCm.x, settings.PatternOriginCm.y,
                         settings.PatternWorldRotationDeg );
            }
        }
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

        // UE's alpha brushes apply at LastMousePosition alone: the step's last position.
        if ( settings.Type != LandscapeBrushType::Circle )
        {
            if ( centres.empty() )
                return Common::MakeSuccess( LandscapeBrushWeights{} );
            LandscapeBrushWeights alpha =
                 AlphaBrushWeights( root, settings, centres.back(), totalRadius, radius, falloff );
            if ( settings.Strength != 1.0f )
                for ( float& v : alpha.Values )
                    v *= settings.Strength;
            return Common::MakeSuccess( std::move( alpha ) );
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

    float LandscapeBrushAlphaSample( const LandscapeBrushAlphaTexture& texture, float x, float y )
    {
        const int32_t w    = static_cast<int32_t>( texture.Width );
        const int32_t h    = static_cast<int32_t>( texture.Height );
        auto          wrap = []( int32_t v, int32_t n )
        {
            const int32_t m = v % n;
            return m < 0 ? m + n : m;
        };
        const float   fx  = std::floor( x );
        const float   fy  = std::floor( y );
        const int32_t x0  = wrap( static_cast<int32_t>( fx ), w );
        const int32_t y0  = wrap( static_cast<int32_t>( fy ), h );
        const int32_t x1  = wrap( x0 + 1, w );
        const int32_t y1  = wrap( y0 + 1, h );
        auto          at  = [&]( int32_t tx, int32_t ty )
        { return static_cast<float>( texture.Values[static_cast<size_t>( ty ) * texture.Width + static_cast<size_t>( tx )] ) / 255.0f; };
        const float   ax  = x - fx;
        const float   ay  = y - fy;
        const float   top = at( x0, y0 ) + ax * ( at( x1, y0 ) - at( x0, y0 ) );
        const float   bot = at( x0, y1 ) + ax * ( at( x1, y1 ) - at( x0, y1 ) );
        return top + ay * ( bot - top );
    }

    void LandscapeAlphaBrushRotation::Move( const LandscapeRoot& root, glm::vec2 positionCm, float deltaSeconds )
    {
        const glm::vec2 p(
             static_cast<float>( ( static_cast<double>( positionCm.x ) - root.Origin.x ) / root.SpacingCm ),
             static_cast<float>( ( static_cast<double>( positionCm.y ) - root.Origin.z ) / root.SpacingCm ) );
        SecondsSinceTurn += std::max( deltaSeconds, 0.0f );
        if ( !OldPosition )
        {
            OldPosition      = p;
            SecondsSinceTurn = 0.0f;
            return;
        }
        const glm::vec2 delta = p - *OldPosition;
        if ( glm::dot( delta, delta ) < 0.25f )
            return;
        const glm::vec2 direction = glm::normalize( delta );
        const float     target    = std::atan2( -direction.y, direction.x );
        AngleRadians += std::min( 10.0f * SecondsSinceTurn, 1.0f ) * ( target - AngleRadians );
        SecondsSinceTurn = 0.0f;
        OldPosition      = p;
    }
} // namespace Desert::World::Landscape
