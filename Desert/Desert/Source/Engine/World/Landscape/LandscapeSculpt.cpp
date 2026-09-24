// Ported from UE 5.8 Engine/Source/Editor/LandscapeEditor/Private/LandscapeEdModePaintTools.cpp:593-808 and
// :878-1000, LandscapeEdModeTools.h:168-280 and :1134-1142, adapted: see LandscapeSculpt.hpp.

#include <Engine/World/Landscape/LandscapeSculpt.hpp>

#include <algorithm>
#include <cmath>
#include <complex>
#include <string>
#include <utility>

namespace Desert::World::Landscape
{
    namespace
    {
        std::string RectName( const LandscapeSampleBounds& r )
        {
            return "(" + std::to_string( r.X1 ) + ", " + std::to_string( r.Z1 ) + ")..(" + std::to_string( r.X2 ) +
                   ", " + std::to_string( r.Z2 ) + ")";
        }

        LandscapeSampleBounds Intersect( const LandscapeSampleBounds& a, const LandscapeSampleBounds& b )
        {
            return { std::max( a.X1, b.X1 ), std::max( a.Z1, b.Z1 ), std::min( a.X2, b.X2 ),
                     std::min( a.Z2, b.Z2 ) };
        }

        /// The brush's inclusive bounds (UE's GetInclusiveBounds).
        LandscapeSampleBounds BrushRect( const LandscapeBrushWeights& w )
        {
            return { w.X0, w.Z0, w.X0 + static_cast<int32_t>( w.Width ) - 1,
                     w.Z0 + static_cast<int32_t>( w.Depth ) - 1 };
        }

        uint16_t ClampValue( int64_t v )
        {
            return static_cast<uint16_t>( std::clamp<int64_t>( v, 0, kLandscapeMaxSample ) );
        }

        /// FMath::RoundToInt.
        int32_t RoundToInt( float v )
        {
            return static_cast<int32_t>( std::floor( v + 0.5f ) );
        }

        /// FMath::Lerp on the value type: (T)(A + Alpha * (B - A)), truncating as UE's cast does. Clamped first
        /// because a float outside uint16 converts with undefined behaviour in C++ (UE relies on it not
        /// happening).
        uint16_t LerpValue( uint16_t a, float b, float alpha )
        {
            const float v = static_cast<float>( a ) + alpha * ( b - static_cast<float>( a ) );
            return static_cast<uint16_t>( std::clamp( v, 0.0f, static_cast<float>( kLandscapeMaxSample ) ) );
        }

        using Complex = std::complex<double>;

        /// In-place DFT of @p count values spaced @p stride apart; @p sign -1 forward, +1 inverse (unnormalised,
        /// as kiss_fft). Separable rows-then-columns passes compute exactly the 2-D DFT kiss_fftnd computes.
        void Dft( std::vector<Complex>& data, size_t first, size_t stride, size_t count, double sign,
                  std::vector<Complex>& scratch )
        {
            const double kTwoPi = 6.283185307179586476925;
            scratch.assign( count, Complex( 0.0, 0.0 ) );
            for ( size_t k = 0; k < count; ++k )
                for ( size_t n = 0; n < count; ++n )
                {
                    const double angle =
                         sign * kTwoPi * static_cast<double>( ( k * n ) % count ) / static_cast<double>( count );
                    scratch[k] += data[first + n * stride] * Complex( std::cos( angle ), std::sin( angle ) );
                }
            for ( size_t k = 0; k < count; ++k )
                data[first + k * stride] = scratch[k];
        }

        void Dft2D( std::vector<Complex>& data, size_t width, size_t height, double sign )
        {
            std::vector<Complex> scratch;
            for ( size_t y = 0; y < height; ++y )
                Dft( data, y * width, 1u, width, sign, scratch );
            for ( size_t x = 0; x < width; ++x )
                Dft( data, x, width, height, sign, scratch );
        }
    } // namespace

    Common::BoolResultStr ValidateLandscapeSmooth( const LandscapeSmoothSettings& s )
    {
        if ( s.FilterKernelRadius < kLandscapeSmoothMinRadius || s.FilterKernelRadius > kLandscapeSmoothMaxRadius )
            return Common::MakeError( "landscape smooth: filter kernel radius " +
                                      std::to_string( s.FilterKernelRadius ) + " outside " +
                                      std::to_string( kLandscapeSmoothMinRadius ) + ".." +
                                      std::to_string( kLandscapeSmoothMaxRadius ) );
        if ( !( s.DetailScale >= 0.0f && s.DetailScale <= kLandscapeMaxDetailScale ) )
            return Common::MakeError( "landscape smooth: detail scale " + std::to_string( s.DetailScale ) +
                                      " outside 0.." + std::to_string( kLandscapeMaxDetailScale ) );
        return Common::MakeSuccess( true );
    }

    float LandscapeSculptStrength( const LandscapeRoot& root, const LandscapeBrushSettings& brush,
                                   const LandscapeSculptStep& step )
    {
        // StrengthMultiplier: "strength 1 = one hemisphere" — the brush radius expressed in height steps.
        const float adjusted = brush.RadiusCm * kLandscapeStepsPerLocal / root.ZScale;
        const float dt       = std::min( step.DeltaSeconds, kLandscapeStrokeMaxDeltaSeconds );
        const float strength = brush.Strength * adjusted * dt * 3.0f;
        if ( !( strength > 0.0f ) )
            return 0.0f;
        return std::max( strength, 1.0f );
    }

    LandscapeHeightStroke::LandscapeHeightStroke( const LandscapeRoot& root, LandscapeTileLookup lookup,
                                                  LandscapeSampleBounds bounds )
         : m_Root( root ), m_Bounds( bounds ), m_Cache( root, lookup ), m_Original( root, lookup )
    {
    }

    LandscapeSampleBounds LandscapeHeightStroke::StepRect( const LandscapeBrushWeights& weights ) const
    {
        // "expand the area by one vertex in each direction to ensure normals are calculated correctly"
        LandscapeSampleBounds r = BrushRect( weights );
        r.X1 -= 1;
        r.Z1 -= 1;
        r.X2 += 1;
        r.Z2 += 1;
        return Intersect( r, m_Bounds );
    }

    Common::BoolResultStr LandscapeHeightStroke::Cache( const LandscapeSampleBounds& rect )
    {
        // The original cache extends FIRST and never receives a write: a sample it already holds keeps its
        // pre-stroke value, a newly cached one has not been written by this stroke yet.
        auto original = m_Original.CacheData( rect.X1, rect.Z1, rect.X2, rect.Z2 );
        if ( !original.IsSuccess() )
            return original;
        auto current = m_Cache.CacheData( rect.X1, rect.Z1, rect.X2, rect.Z2 );
        if ( !current.IsSuccess() )
            return current;
        m_Union   = m_Touched
                         ? LandscapeSampleBounds{ std::min( m_Union.X1, rect.X1 ), std::min( m_Union.Z1, rect.Z1 ),
                                                std::max( m_Union.X2, rect.X2 ), std::max( m_Union.Z2, rect.Z2 ) }
                         : rect;
        m_Touched = true;
        return Common::MakeSuccess( true );
    }

    Common::BoolResultStr LandscapeHeightStroke::ApplySculpt( const LandscapeBrushWeights&  weights,
                                                              const LandscapeBrushSettings& brush,
                                                              const LandscapeSculptStep&    step )
    {
        if ( weights.Empty() )
            return Common::MakeSuccess( true );
        const float sculptStrength = LandscapeSculptStrength( m_Root, brush, step );
        if ( sculptStrength <= 0.0f )
            return Common::MakeSuccess( true );
        const LandscapeSampleBounds rect = StepRect( weights );
        if ( rect.Empty() )
            return Common::MakeSuccess( true );
        auto cached = Cache( rect );
        if ( !cached.IsSuccess() )
            return cached;
        auto read = m_Cache.GetCachedData( rect.X1, rect.Z1, rect.X2, rect.Z2 );
        if ( !read.IsSuccess() )
            return Common::MakeError( read.GetError() );
        std::vector<uint16_t> data = read.GetValue();

        const int32_t               width = rect.X2 - rect.X1 + 1;
        const LandscapeSampleBounds b     = Intersect( BrushRect( weights ), m_Bounds );
        for ( int32_t z = b.Z1; z <= b.Z2; ++z )
            for ( int32_t x = b.X1; x <= b.X2; ++x )
            {
                const float    sculptAmount = weights.At( x, z ) * sculptStrength;
                uint16_t&      current = data[static_cast<size_t>( ( z - rect.Z1 ) * width + ( x - rect.X1 ) )];
                const uint16_t source  = current; // non-clay: the source array IS the data array
                const int64_t  amount  = RoundToInt( sculptAmount );
                current                = step.Invert ? ClampValue( std::min<int64_t>( source - amount, current ) )
                                                     : ClampValue( std::max<int64_t>( source + amount, current ) );
            }
        return m_Cache.SetCachedData( rect.X1, rect.Z1, rect.X2, rect.Z2, data );
    }

    Common::BoolResultStr LandscapeHeightStroke::ApplySmooth( const LandscapeBrushWeights&   weights,
                                                              const LandscapeBrushSettings&  brush,
                                                              const LandscapeSmoothSettings& smooth )
    {
        auto valid = ValidateLandscapeSmooth( smooth );
        if ( !valid.IsSuccess() )
            return valid;
        if ( weights.Empty() )
            return Common::MakeSuccess( true );
        const LandscapeSampleBounds rect = StepRect( weights );
        const LandscapeSampleBounds b    = Intersect( BrushRect( weights ), m_Bounds );
        if ( rect.Empty() || b.Empty() )
            return Common::MakeSuccess( true );
        auto cached = Cache( rect );
        if ( !cached.IsSuccess() )
            return cached;
        auto read = m_Cache.GetCachedData( rect.X1, rect.Z1, rect.X2, rect.Z2 );
        if ( !read.IsSuccess() )
            return Common::MakeError( read.GetError() );
        std::vector<uint16_t>       data     = read.GetValue();
        const std::vector<uint16_t> readData = data;
        const int32_t               width    = rect.X2 - rect.X1 + 1;
        auto                        index    = [&]( int32_t x, int32_t z )
        { return static_cast<size_t>( ( z - rect.Z1 ) * width + ( x - rect.X1 ) ); };

        const float toolStrength = std::clamp( brush.Strength, 0.0f, 1.0f );

        if ( smooth.DetailSmooth )
        {
            // LowPassFilter over the brush bounds (UE's X1 + 1 .. X2 - 1 of the grown rectangle).
            const size_t fftW = static_cast<size_t>( b.X2 - b.X1 + 1 );
            const size_t fftH = static_cast<size_t>( b.Z2 - b.Z1 + 1 );
            if ( fftW <= 1u && fftH <= 1u )
                return m_Cache.SetCachedData( rect.X1, rect.Z1, rect.X2, rect.Z2, data );
            std::vector<Complex> buf( fftW * fftH );
            for ( int32_t z = b.Z1; z <= b.Z2; ++z )
                for ( int32_t x = b.X1; x <= b.X2; ++x )
                    buf[static_cast<size_t>( z - b.Z1 ) * fftW + static_cast<size_t>( x - b.X1 )] =
                         Complex( static_cast<double>( data[index( x, z )] ), 0.0 );
            Dft2D( buf, fftW, fftH, -1.0 );

            const int32_t dims0 = static_cast<int32_t>( fftH );
            const int32_t dims1 = static_cast<int32_t>( fftW );
            const float   ratio = 1.0f - smooth.DetailScale;
            const float   dist =
                 std::min( ( dims0 * ratio ) * ( dims0 * ratio ), ( dims1 * ratio ) * ( dims1 * ratio ) );
            for ( int32_t y = 0; y < dims0; ++y )
                for ( int32_t x = 0; x < dims1; ++x )
                {
                    // The four quadrants of UE's loop: distance from the zero frequency with wrap-around.
                    const int32_t fy             = y < ( dims0 >> 1 ) ? y : y - dims0;
                    const int32_t fx             = x < ( dims1 >> 1 ) ? x : x - dims1;
                    const float   distFromCenter = static_cast<float>( fx * fx + fy * fy );
                    const float   filter         = 1.0f / ( 1.0f + distFromCenter / dist );
                    buf[static_cast<size_t>( y ) * fftW + static_cast<size_t>( x )] *=
                         static_cast<double>( filter );
                }
            Dft2D( buf, fftW, fftH, 1.0 );

            const float scale = static_cast<float>( dims0 * dims1 );
            for ( int32_t z = b.Z1; z <= b.Z2; ++z )
                for ( int32_t x = b.X1; x <= b.X2; ++x )
                {
                    const float brushValue = weights.At( x, z );
                    if ( brushValue > 0.0f )
                    {
                        const float filtered = static_cast<float>(
                             buf[static_cast<size_t>( z - b.Z1 ) * fftW + static_cast<size_t>( x - b.X1 )]
                                  .real() );
                        data[index( x, z )] =
                             LerpValue( data[index( x, z )], filtered / scale, brushValue * toolStrength );
                    }
                }
            return m_Cache.SetCachedData( rect.X1, rect.Z1, rect.X2, rect.Z2, data );
        }

        const int32_t filterRadius = smooth.FilterKernelRadius;
        for ( int32_t z = b.Z1; z <= b.Z2; ++z )
            for ( int32_t x = b.X1; x <= b.X2; ++x )
            {
                const float brushValue = weights.At( x, z ) * toolStrength;
                if ( !( brushValue > 0.0f ) )
                    continue;
                int64_t       filterValue = 0;
                int32_t       samples     = 0;
                const int32_t xRadius     = std::min( { filterRadius, x - b.X1, b.X2 - x } );
                const int32_t zRadius     = std::min( { filterRadius, z - b.Z1, b.Z2 - z } );
                for ( int32_t sz = z - zRadius; sz <= z + zRadius; ++sz )
                    for ( int32_t sx = x - xRadius; sx <= x + xRadius; ++sx )
                    {
                        // "constrain sample to within the brush, symmetrically to prevent flattening bug"
                        const int32_t mx = x + ( x - sx );
                        const int32_t mz = z + ( z - sz );
                        const float   sampleBrushValue =
                             std::min( std::min( weights.At( sx, sz ), weights.At( mx, sz ) ),
                                       std::min( weights.At( sx, mz ), weights.At( mx, mz ) ) );
                        if ( sampleBrushValue > 0.0f )
                        {
                            filterValue += readData[index( sx, sz )];
                            ++samples;
                        }
                    }
                if ( samples > 0 )
                    filterValue /= samples;
                data[index( x, z )] = LerpValue(
                     data[index( x, z )], static_cast<float>( static_cast<uint16_t>( filterValue ) ), brushValue );
            }
        return m_Cache.SetCachedData( rect.X1, rect.Z1, rect.X2, rect.Z2, data );
    }

    Common::ResultStr<LandscapeStrokeRecord> LandscapeHeightStroke::Finish() const
    {
        if ( !m_Touched )
            return Common::MakeError<LandscapeStrokeRecord>(
                 "landscape stroke: nothing was cached, nothing to record" );
        LandscapeStrokeRecord record;
        record.Rect = m_Union;
        auto before = m_Original.GetCachedData( m_Union.X1, m_Union.Z1, m_Union.X2, m_Union.Z2 );
        if ( !before.IsSuccess() )
            return Common::MakeError<LandscapeStrokeRecord>( before.GetError() );
        auto after = m_Cache.GetCachedData( m_Union.X1, m_Union.Z1, m_Union.X2, m_Union.Z2 );
        if ( !after.IsSuccess() )
            return Common::MakeError<LandscapeStrokeRecord>( after.GetError() );
        record.Before = before.GetValue();
        record.After  = after.GetValue();
        return Common::MakeSuccess( std::move( record ) );
    }

    Common::BoolResultStr WriteLandscapeHeights( const LandscapeRoot& root, LandscapeTileLookup lookup,
                                                 const LandscapeSampleBounds& rect,
                                                 const std::vector<uint16_t>& values )
    {
        if ( rect.Empty() )
            return Common::MakeError( "landscape heights: empty rectangle " + RectName( rect ) );
        LandscapeHeightCache cache( root, std::move( lookup ) );
        auto                 cached = cache.CacheData( rect.X1, rect.Z1, rect.X2, rect.Z2 );
        if ( !cached.IsSuccess() )
            return cached;
        return cache.SetCachedData( rect.X1, rect.Z1, rect.X2, rect.Z2, values );
    }
} // namespace Desert::World::Landscape
