// Ported from UE 5.8 Engine/Source/Editor/LandscapeEditor/Private/LandscapeEdModePaintTools.cpp:29-46, :505-590,
// :593-808, :878-1000, :1049-1350 and :1533-1650, LandscapeEdModeTools.h:29-280, :474-516 and :1134-1142,
// LandscapeEditorObject.h:85-99, adapted: see LandscapeSculpt.hpp.

#include <Engine/World/Landscape/LandscapeSculpt.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <optional>

#include <glm/geometric.hpp>
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

        /// UE's BrushValue: the falloff weight alone. ComputeLandscapeBrush folds the tool strength into every
        /// weight, and UE multiplies (and for Smooth / Flatten / Erase clamps) that strength itself, so a tool
        /// that used the weight as UE's BrushValue applied the strength twice.
        float BrushValue( const LandscapeBrushWeights& weights, const LandscapeBrushSettings& brush, int32_t x,
                          int32_t z )
        {
            if ( !( brush.Strength > 0.0f ) )
                return 0.0f;
            return weights.At( x, z ) / brush.Strength;
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

        /// FNoiseParameter::Permutations, UE's (Ken Perlin's reference) table verbatim.
        constexpr int32_t kNoisePermutations[256] = {
             151, 160, 137, 91,  90,  15,  131, 13,  201, 95,  96,  53,  194, 233, 7,   225, 140, 36,  103, 30,
             69,  142, 8,   99,  37,  240, 21,  10,  23,  190, 6,   148, 247, 120, 234, 75,  0,   26,  197, 62,
             94,  252, 219, 203, 117, 35,  11,  32,  57,  177, 33,  88,  237, 149, 56,  87,  174, 20,  125, 136,
             171, 168, 68,  175, 74,  165, 71,  134, 139, 48,  27,  166, 77,  146, 158, 231, 83,  111, 229, 122,
             60,  211, 133, 230, 220, 105, 92,  41,  55,  46,  245, 40,  244, 102, 143, 54,  65,  25,  63,  161,
             1,   216, 80,  73,  209, 76,  132, 187, 208, 89,  18,  169, 200, 196, 135, 130, 116, 188, 159, 86,
             164, 100, 109, 198, 173, 186, 3,   64,  52,  217, 226, 250, 124, 123, 5,   202, 38,  147, 118, 126,
             255, 82,  85,  212, 207, 206, 59,  227, 47,  16,  58,  17,  182, 189, 28,  42,  223, 183, 170, 213,
             119, 248, 152, 2,   44,  154, 163, 70,  221, 153, 101, 155, 167, 43,  172, 9,   129, 22,  39,  253,
             19,  98,  108, 110, 79,  113, 224, 232, 178, 185, 112, 104, 218, 246, 97,  228, 251, 34,  242, 193,
             238, 210, 144, 12,  191, 179, 162, 241, 81,  51,  145, 235, 249, 14,  239, 107, 49,  192, 214, 31,
             181, 199, 106, 157, 184, 84,  204, 176, 115, 121, 50,  45,  127, 4,   150, 254, 138, 236, 205, 93,
             222, 114, 67,  29,  24,  72,  243, 141, 128, 195, 78,  66,  215, 61,  156, 180,
        };

        float NoiseFade( float t )
        {
            return t * t * t * ( t * ( t * 6 - 15 ) + 10 );
        }

        float NoiseGrad( int32_t hash, float x, float y )
        {
            const int32_t h = hash & 15;
            const float   u = h < 8 || h == 12 || h == 13 ? x : y;
            const float   v = h < 4 || h == 12 || h == 13 ? y : 0;
            return ( ( h & 1 ) == 0 ? u : -u ) + ( ( h & 2 ) == 0 ? v : -v );
        }

        float Lerp( float a, float b, float alpha )
        {
            return a + alpha * ( b - a );
        }

        /// FNoiseParameter::PerlinNoise2D.
        float PerlinNoise2D( float x, float y )
        {
            const int32_t truncX = static_cast<int32_t>( x );
            const int32_t truncY = static_cast<int32_t>( y );
            const int32_t intX   = truncX & 255;
            const int32_t intY   = truncY & 255;
            const float   fracX  = x - static_cast<float>( truncX );
            const float   fracY  = y - static_cast<float>( truncY );
            const float   u      = NoiseFade( fracX );
            const float   v      = NoiseFade( fracY );
            const int32_t a      = kNoisePermutations[intX] + intY;
            const int32_t aa     = kNoisePermutations[a & 255];
            const int32_t ab     = kNoisePermutations[( a + 1 ) & 255];
            const int32_t b      = kNoisePermutations[( intX + 1 ) & 255] + intY;
            const int32_t ba     = kNoisePermutations[b & 255];
            const int32_t bb     = kNoisePermutations[( b + 1 ) & 255];
            return Lerp( Lerp( NoiseGrad( kNoisePermutations[aa], fracX, fracY ),
                               NoiseGrad( kNoisePermutations[ba], fracX - 1, fracY ), u ),
                         Lerp( NoiseGrad( kNoisePermutations[ab], fracX, fracY - 1 ),
                               NoiseGrad( kNoisePermutations[bb], fracX - 1, fracY - 1 ), u ),
                         v );
        }

        /// A float height to the value type, clamped first: UE's static_cast<ValueType> of an out-of-range float
        /// is undefined behaviour in C++.
        uint16_t CastValue( float v )
        {
            if ( !( v > 0.0f ) )
                return 0u;
            return static_cast<uint16_t>( std::min( v, static_cast<float>( kLandscapeMaxSample ) ) );
        }

        /// UE's flatten / erase step: floor when above the target, ceil when at or below — never overshoots.
        uint16_t TowardsFloorCeil( uint16_t current, float target, float strength, bool above )
        {
            const float v = Lerp( static_cast<float>( current ), target, strength );
            return CastValue( above ? std::floor( v ) : std::ceil( v ) );
        }

        /// LandscapeDataAccess::GetLocalHeight / GetTexHeight.
        float LocalHeight( uint16_t v )
        {
            return ( static_cast<float>( v ) - static_cast<float>( kLandscapeMidSample ) ) *
                   kLandscapeLocalPerStep;
        }

        uint16_t TexHeight( float local )
        {
            return static_cast<uint16_t>( RoundToInt(
                 std::clamp( local * kLandscapeStepsPerLocal + static_cast<float>( kLandscapeMidSample ), 0.0f,
                             static_cast<float>( kLandscapeMaxSample ) ) ) );
        }

        /// FVector::GetSafeNormal.
        glm::dvec3 SafeNormal( const glm::dvec3& v )
        {
            const double sq = v.x * v.x + v.y * v.y + v.z * v.z;
            if ( sq < 1.0e-8 )
                return glm::dvec3( 0.0 );
            return v / std::sqrt( sq );
        }

        glm::dvec3 Cross( const glm::dvec3& a, const glm::dvec3& b )
        {
            return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
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

        /// UE's BrushValue at every sample of @p inner, row-major, X fastest.
        std::vector<float> BrushValues( const LandscapeBrushWeights& weights, const LandscapeBrushSettings& brush,
                                        const LandscapeSampleBounds& inner )
        {
            std::vector<float> values;
            values.reserve( static_cast<size_t>( inner.X2 - inner.X1 + 1 ) *
                            static_cast<size_t>( inner.Z2 - inner.Z1 + 1 ) );
            for ( int32_t z = inner.Z1; z <= inner.Z2; ++z )
                for ( int32_t x = inner.X1; x <= inner.X2; ++x )
                    values.push_back( BrushValue( weights, brush, x, z ) );
            return values;
        }

        /// LowPassFilter: a 2D DFT of @p inner (the brush bounds inside @p rect, UE's X1 + 1 .. X2 - 1), the
        /// 1 / (1 + d^2 / dist) filter, and a lerp towards the filtered height by BrushValue * @p amount.
        void LowPassFilter( std::vector<uint16_t>& data, const LandscapeSampleBounds& rect,
                            const LandscapeSampleBounds& inner, const std::vector<float>& brushValues,
                            float detailScale, float amount )
        {
            const int32_t width = rect.X2 - rect.X1 + 1;
            auto          index = [&]( int32_t x, int32_t z )
            { return static_cast<size_t>( ( z - rect.Z1 ) * width + ( x - rect.X1 ) ); };
            const size_t fftW = static_cast<size_t>( inner.X2 - inner.X1 + 1 );
            const size_t fftH = static_cast<size_t>( inner.Z2 - inner.Z1 + 1 );
            if ( fftW <= 1u && fftH <= 1u )
                return;
            auto local = [&]( int32_t x, int32_t z )
            { return static_cast<size_t>( z - inner.Z1 ) * fftW + static_cast<size_t>( x - inner.X1 ); };
            std::vector<Complex> buf( fftW * fftH );
            for ( int32_t z = inner.Z1; z <= inner.Z2; ++z )
                for ( int32_t x = inner.X1; x <= inner.X2; ++x )
                    buf[local( x, z )] = Complex( static_cast<double>( data[index( x, z )] ), 0.0 );
            Dft2D( buf, fftW, fftH, -1.0 );

            const int32_t dims0 = static_cast<int32_t>( fftH );
            const int32_t dims1 = static_cast<int32_t>( fftW );
            const float   ratio = 1.0f - detailScale;
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
            for ( int32_t z = inner.Z1; z <= inner.Z2; ++z )
                for ( int32_t x = inner.X1; x <= inner.X2; ++x )
                {
                    const float brushValue = brushValues[local( x, z )];
                    if ( brushValue > 0.0f )
                    {
                        const float filtered = static_cast<float>( buf[local( x, z )].real() );
                        data[index( x, z )] =
                             LerpValue( data[index( x, z )], filtered / scale, brushValue * amount );
                    }
                }
        }

        /// NoiseModeConversion: Both keeps the noise centred on 0, Add / Sub shift it by its amplitude.
        float NoiseModeConversion( LandscapeNoiseMode mode, float amount, float sample )
        {
            if ( mode == LandscapeNoiseMode::Add )
                return sample + amount;
            if ( mode == LandscapeNoiseMode::Sub )
                return sample - amount;
            return sample;
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

    Common::BoolResultStr ValidateLandscapeFlatten( const LandscapeFlattenSettings& s )
    {
        if ( !( s.TerraceIntervalCm >= kLandscapeMinTerraceIntervalCm &&
                s.TerraceIntervalCm <= kLandscapeMaxTerraceIntervalCm ) )
            return Common::MakeError( "landscape flatten: terrace interval " +
                                      std::to_string( s.TerraceIntervalCm ) + " cm outside " +
                                      std::to_string( kLandscapeMinTerraceIntervalCm ) + ".." +
                                      std::to_string( kLandscapeMaxTerraceIntervalCm ) );
        if ( !( s.TerraceSmooth >= kLandscapeMinTerraceSmooth && s.TerraceSmooth <= kLandscapeMaxTerraceSmooth ) )
            return Common::MakeError( "landscape flatten: terrace smooth " + std::to_string( s.TerraceSmooth ) +
                                      " outside " + std::to_string( kLandscapeMinTerraceSmooth ) + ".." +
                                      std::to_string( kLandscapeMaxTerraceSmooth ) );
        return Common::MakeSuccess( true );
    }

    Common::BoolResultStr ValidateLandscapeNoise( const LandscapeNoiseSettings& s )
    {
        if ( !( s.NoiseScale >= kLandscapeMinNoiseScale && s.NoiseScale <= kLandscapeMaxNoiseScale ) )
            return Common::MakeError( "landscape noise: scale " + std::to_string( s.NoiseScale ) + " outside " +
                                      std::to_string( kLandscapeMinNoiseScale ) + ".." +
                                      std::to_string( kLandscapeMaxNoiseScale ) );
        return Common::MakeSuccess( true );
    }

    float LandscapeNoiseSample( int32_t x, int32_t z, float noiseScale )
    {
        // FNoiseParameter(Base 0, NoiseScale, NoiseAmount 1)::Sample.
        constexpr float kDelta = 0.00001f; // UE's DELTA
        float           noise  = 0.0f;
        const float     ax     = static_cast<float>( std::abs( x ) );
        const float     az     = static_cast<float>( std::abs( z ) );
        if ( noiseScale > kDelta )
            for ( uint32_t octave = 0; octave < 4u; ++octave )
            {
                const float octaveShift = static_cast<float>( 1u << octave );
                const float octaveScale = octaveShift / noiseScale;
                noise += PerlinNoise2D( ax * octaveScale, az * octaveScale ) / octaveShift;
            }
        return noise;
    }

    namespace
    {
        /// UE's FVector2D interpolant of the ramp raster: X = side falloff alpha, Y = height in samples.
        using RampInterpolant = glm::dvec2;

        /// UE's FMath::IsNearlyZero(float) tolerance, UE_SMALL_NUMBER.
        constexpr float kRasterSmallNumber = 1.e-8f;

        float NonZeroDiff( float d )
        {
            if ( std::abs( d ) <= kRasterSmallNumber )
                return d >= 0.0f ? kRasterSmallNumber : -kRasterSmallNumber;
            return d;
        }

        /// FLandscapeRampToolHeightRasterPolicy over an inclusive rectangle of samples, row-major, X fastest.
        struct RampRasterPolicy
        {
            std::vector<uint16_t>& Data;
            int32_t                MinX;
            int32_t                MinY;
            int32_t                MaxX;
            int32_t                MaxY;
            bool                   RaiseTerrain;
            bool                   LowerTerrain;

            void ProcessPixel( int32_t x, int32_t y, const RampInterpolant& interpolant ) const
            {
                const float cosInterpX = static_cast<float>(
                     interpolant.x >= 1 ? 1 : 0.5 - 0.5 * std::cos( interpolant.x * 3.14159265358979323846 ) );
                const float alpha = cosInterpX;
                uint16_t&   dest  = Data[static_cast<size_t>( ( y - MinY ) * ( 1 + MaxX - MinX ) + x - MinX )];
                const float value = Lerp( static_cast<float>( dest ), static_cast<float>( interpolant.y ), alpha );
                const auto  dValue =
                     static_cast<uint16_t>( std::clamp( value, 0.0f, static_cast<float>( kLandscapeMaxSample ) ) );
                if ( ( RaiseTerrain && dValue > dest ) || ( LowerTerrain && dValue < dest ) )
                    dest = dValue;
            }
        };

        /// UE's FTriangleRasterizer (Raster.h): scanline fill with pixel centres at integer coordinates, the
        /// interpolant linear across the triangle; the ramp's edges are shared, so no sample is drawn twice.
        class RampRasterizer
        {
        public:
            explicit RampRasterizer( const RampRasterPolicy& policy ) : m_Policy( policy )
            {
            }

            void DrawTriangle( const RampInterpolant& i0, const RampInterpolant& i1, const RampInterpolant& i2,
                               const glm::dvec2& p0, const glm::dvec2& p1, const glm::dvec2& p2 )
            {
                std::array<RampInterpolant, 3> in{ i0, i1, i2 };
                std::array<glm::vec2, 3>       pt{ glm::vec2( p0 ), glm::vec2( p1 ), glm::vec2( p2 ) };
                if ( pt[1].y < pt[0].y && pt[1].y <= pt[2].y )
                {
                    std::swap( pt[0], pt[1] );
                    std::swap( in[0], in[1] );
                }
                else if ( pt[2].y < pt[0].y && pt[2].y <= pt[1].y )
                {
                    std::swap( pt[0], pt[2] );
                    std::swap( in[0], in[2] );
                }
                if ( pt[1].y > pt[2].y )
                {
                    std::swap( pt[2], pt[1] );
                    std::swap( in[2], in[1] );
                }
                const float d10 = NonZeroDiff( pt[1].y - pt[0].y );
                const float d20 = NonZeroDiff( pt[2].y - pt[0].y );
                const float d21 = NonZeroDiff( pt[2].y - pt[1].y );

                const float           topMinDiffX         = ( pt[1].x - pt[0].x ) / d10;
                const float           topMaxDiffX         = ( pt[2].x - pt[0].x ) / d20;
                const RampInterpolant topMinDiffInterp    = ( in[1] - in[0] ) / static_cast<double>( d10 );
                const RampInterpolant topMaxDiffInterp    = ( in[2] - in[0] ) / static_cast<double>( d20 );
                const float           bottomMinDiffX      = ( pt[2].x - pt[1].x ) / d21;
                const float           bottomMaxDiffX      = ( pt[2].x - pt[0].x ) / d20;
                const RampInterpolant bottomMinDiffInterp = ( in[2] - in[1] ) / static_cast<double>( d21 );
                const RampInterpolant bottomMaxDiffInterp = ( in[2] - in[0] ) / static_cast<double>( d20 );

                Trapezoid( in[0], topMinDiffInterp, in[0], topMaxDiffInterp, pt[0].x, topMinDiffX, pt[0].x,
                           topMaxDiffX, pt[0].y, pt[1].y );
                Trapezoid( in[1], bottomMinDiffInterp, in[0] + topMaxDiffInterp * static_cast<double>( d10 ),
                           bottomMaxDiffInterp, pt[1].x, bottomMinDiffX, pt[0].x + topMaxDiffX * d10,
                           bottomMaxDiffX, pt[1].y, pt[2].y );
            }

        private:
            void Trapezoid( const RampInterpolant& topMinInterp, const RampInterpolant& deltaMinInterp,
                            const RampInterpolant& topMaxInterp, const RampInterpolant& deltaMaxInterp,
                            float topMinX, float deltaMinX, float topMaxX, float deltaMaxX, float inMinY,
                            float inMaxY )
            {
                const int32_t intMinY =
                     std::clamp( static_cast<int32_t>( std::ceil( inMinY ) ), m_Policy.MinY, m_Policy.MaxY + 1 );
                const int32_t intMaxY =
                     std::clamp( static_cast<int32_t>( std::ceil( inMaxY ) ), m_Policy.MinY, m_Policy.MaxY + 1 );
                for ( int32_t intY = intMinY; intY < intMaxY; ++intY )
                {
                    const float     y         = static_cast<float>( intY ) - inMinY;
                    float           localMinX = topMinX + deltaMinX * y;
                    float           localMaxX = topMaxX + deltaMaxX * y;
                    RampInterpolant minInterp = topMinInterp + deltaMinInterp * static_cast<double>( y );
                    RampInterpolant maxInterp = topMaxInterp + deltaMaxInterp * static_cast<double>( y );
                    if ( localMinX > localMaxX )
                    {
                        std::swap( localMinX, localMaxX );
                        std::swap( minInterp, maxInterp );
                    }
                    if ( !( localMaxX > localMinX ) )
                        continue;
                    const int32_t         intMinX = std::clamp( static_cast<int32_t>( std::ceil( localMinX ) ),
                                                                m_Policy.MinX, m_Policy.MaxX + 1 );
                    const int32_t         intMaxX = std::clamp( static_cast<int32_t>( std::ceil( localMaxX ) ),
                                                                m_Policy.MinX, m_Policy.MaxX + 1 );
                    const RampInterpolant deltaInterp =
                         ( maxInterp - minInterp ) / static_cast<double>( localMaxX - localMinX );
                    for ( int32_t x = intMinX; x < intMaxX; ++x )
                        m_Policy.ProcessPixel(
                             x, intY,
                             minInterp +
                                  deltaInterp * static_cast<double>( static_cast<float>( x ) - localMinX ) );
                }
            }

            RampRasterPolicy m_Policy;
        };
    } // namespace

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
                const float    sculptAmount = BrushValue( weights, brush, x, z ) * sculptStrength;
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
            LowPassFilter( data, rect, b, BrushValues( weights, brush, b ), smooth.DetailScale, toolStrength );
            return m_Cache.SetCachedData( rect.X1, rect.Z1, rect.X2, rect.Z2, data );
        }

        const int32_t filterRadius = smooth.FilterKernelRadius;
        for ( int32_t z = b.Z1; z <= b.Z2; ++z )
            for ( int32_t x = b.X1; x <= b.X2; ++x )
            {
                const float brushValue = BrushValue( weights, brush, x, z ) * toolStrength;
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

    float LandscapeHeightStroke::PickValue( float gx, float gz )
    {
        const int32_t x      = static_cast<int32_t>( std::floor( gx ) );
        const int32_t z      = static_cast<int32_t>( std::floor( gz ) );
        auto          corner = [&]( int32_t cx, int32_t cz ) -> std::optional<float>
        {
            if ( cx < m_Bounds.X1 || cx > m_Bounds.X2 || cz < m_Bounds.Z1 || cz > m_Bounds.Z2 )
                return std::nullopt;
            auto v = m_Cache.GetCachedData( cx, cz, cx, cz );
            if ( !v.IsSuccess() )
                return std::nullopt;
            return static_cast<float>( v.GetValue()[0] );
        };
        const auto p00 = corner( x, z );
        const auto p10 = corner( x + 1, z );
        const auto p01 = corner( x, z + 1 );
        const auto p11 = corner( x + 1, z + 1 );
        // "Search for nearest value if missing data"
        const float v00 = p00 ? *p00 : ( p10 ? *p10 : ( p01 ? *p01 : ( p11 ? *p11 : 0.0f ) ) );
        const float v10 = p10 ? *p10 : ( p00 ? *p00 : ( p11 ? *p11 : ( p01 ? *p01 : 0.0f ) ) );
        const float v01 = p01 ? *p01 : ( p00 ? *p00 : ( p11 ? *p11 : ( p10 ? *p10 : 0.0f ) ) );
        const float v11 = p11 ? *p11 : ( p10 ? *p10 : ( p01 ? *p01 : ( p00 ? *p00 : 0.0f ) ) );
        return Lerp( Lerp( v00, v10, gx - static_cast<float>( x ) ),
                     Lerp( v01, v11, gx - static_cast<float>( x ) ), gz - static_cast<float>( z ) );
    }

    glm::dvec3 LandscapeHeightStroke::PickNormal( int32_t x, int32_t z )
    {
        const double     v00 = PickValue( static_cast<float>( x ), static_cast<float>( z ) );
        const double     v10 = PickValue( static_cast<float>( x + 1 ), static_cast<float>( z ) );
        const double     v01 = PickValue( static_cast<float>( x ), static_cast<float>( z + 1 ) );
        const double     v11 = PickValue( static_cast<float>( x + 1 ), static_cast<float>( z + 1 ) );
        const glm::dvec3 vert00( 0.0, 0.0, v00 );
        const glm::dvec3 vert01( 0.0, 1.0, v01 );
        const glm::dvec3 vert10( 1.0, 0.0, v10 );
        const glm::dvec3 vert11( 1.0, 1.0, v11 );
        const glm::dvec3 face1 = SafeNormal( Cross( vert00 - vert10, vert10 - vert11 ) );
        const glm::dvec3 face2 = SafeNormal( Cross( vert11 - vert01, vert01 - vert00 ) );
        return SafeNormal( face1 + face2 );
    }

    Common::BoolResultStr LandscapeHeightStroke::ApplyFlatten( const LandscapeBrushWeights&    weights,
                                                               const LandscapeBrushSettings&   brush,
                                                               const LandscapeFlattenSettings& flatten,
                                                               glm::vec2                       pickCm )
    {
        auto valid = ValidateLandscapeFlatten( flatten );
        if ( !valid.IsSuccess() )
            return valid;
        if ( weights.Empty() )
            return Common::MakeSuccess( true );
        const LandscapeSampleBounds rect = StepRect( weights );
        const LandscapeSampleBounds b    = Intersect( BrushRect( weights ), m_Bounds );
        if ( rect.Empty() || b.Empty() )
            return Common::MakeSuccess( true );

        if ( !m_Flatten || flatten.PickValuePerApply )
        {
            // The pick point in lattice units (UE: InteractorPositions[0].Position, landscape space).
            const float flattenX =
                 static_cast<float>( ( static_cast<double>( pickCm.x ) - m_Root.Origin.x ) / m_Root.SpacingCm );
            const float flattenZ =
                 static_cast<float>( ( static_cast<double>( pickCm.y ) - m_Root.Origin.z ) / m_Root.SpacingCm );
            const int32_t               hx   = static_cast<int32_t>( std::floor( flattenX ) );
            const int32_t               hz   = static_cast<int32_t>( std::floor( flattenZ ) );
            const LandscapeSampleBounds pick = Intersect( { hx, hz, hx + 1, hz + 1 }, m_Bounds );
            if ( !pick.Empty() )
            {
                auto cached = Cache( pick );
                if ( !cached.IsSuccess() )
                    return cached;
            }
            FlattenValues values;
            const float   interpolated = PickValue( flattenX, flattenZ );
            values.Value               = CastValue( interpolated );
            if ( flatten.UseSlopeFlatten )
            {
                values.Normal    = PickNormal( hx, hz );
                values.PlaneDist = static_cast<float>(
                     -glm::dot( values.Normal, glm::dvec3( flattenX, flattenZ, interpolated ) ) );
            }
            m_Flatten = values;
        }

        auto cached = Cache( rect );
        if ( !cached.IsSuccess() )
            return cached;
        auto read = m_Cache.GetCachedData( rect.X1, rect.Z1, rect.X2, rect.Z2 );
        if ( !read.IsSuccess() )
            return Common::MakeError( read.GetError() );
        std::vector<uint16_t> data = read.GetValue();

        const FlattenValues& fv     = *m_Flatten;
        const float          target = static_cast<float>( fv.Value );
        const float          scaleZ = m_Root.ZScale;
        const float          transZ = m_Root.Origin.y;
        const int32_t        width  = rect.X2 - rect.X1 + 1;
        for ( int32_t z = b.Z1; z <= b.Z2; ++z )
            for ( int32_t x = b.X1; x <= b.X2; ++x )
            {
                const float brushValue = BrushValue( weights, brush, x, z );
                if ( !( brushValue > 0.0f ) )
                    continue;
                const float strength = std::clamp( brushValue * brush.Strength, 0.0f, 1.0f );
                uint16_t&   current  = data[static_cast<size_t>( ( z - rect.Z1 ) * width + ( x - rect.X1 ) )];
                if ( !flatten.UseSlopeFlatten )
                {
                    const int32_t delta = static_cast<int32_t>( current ) - static_cast<int32_t>( fv.Value );
                    switch ( flatten.Mode )
                    {
                        case LandscapeFlattenMode::Terrace:
                        {
                            const float interval    = std::clamp( flatten.TerraceIntervalCm, 1.0f, 32768.0f );
                            float       worldHeight = LocalHeight( current ) * scaleZ + transZ;
                            const float currentH    = worldHeight;
                            float       level       = worldHeight / interval;
                            const float smoothness  = 1.0f / std::max( flatten.TerraceSmooth, 0.0001f );
                            const float phase       = level - std::floor( level );
                            const float halfmask    = std::clamp( std::ceil( phase - 0.5f ), 0.0f, 1.0f );
                            level                   = std::floor( worldHeight / interval );
                            float sCurve            = Lerp( phase, 1.0f - phase, halfmask ) * 2.0f;
                            sCurve                  = std::pow( sCurve, smoothness ) * 0.5f;
                            sCurve                  = Lerp( sCurve, 1.0f - sCurve, halfmask ) * interval;
                            worldHeight             = level * interval + sCurve;
                            const float finalH = ( Lerp( currentH, worldHeight, strength ) - transZ ) / scaleZ;
                            current            = TexHeight( finalH );
                            break;
                        }
                        case LandscapeFlattenMode::Interval:
                        {
                            const float interval = flatten.TerraceIntervalCm;
                            float       targetH  = LocalHeight( fv.Value ) * scaleZ + transZ;
                            const float currentH = LocalHeight( current ) * scaleZ + transZ;
                            targetH              = std::floor( targetH / interval + 0.5f ) * interval;
                            // UE lerps by the brush value here, not by the strength.
                            targetH = Lerp( currentH, targetH, brushValue );
                            current = TexHeight( ( targetH - transZ ) / scaleZ );
                            break;
                        }
                        case LandscapeFlattenMode::Raise:
                            if ( delta < 0 )
                                current = TowardsFloorCeil( current, target, strength, false );
                            break;
                        case LandscapeFlattenMode::Lower:
                            if ( delta > 0 )
                                current = TowardsFloorCeil( current, target, strength, true );
                            break;
                        case LandscapeFlattenMode::Both:
                            current = TowardsFloorCeil( current, target, strength, delta > 0 );
                            break;
                    }
                }
                else
                {
                    const float planeValue = static_cast<float>(
                         -( fv.Normal.x * x + fv.Normal.y * z + static_cast<double>( fv.PlaneDist ) ) /
                         fv.Normal.z );
                    uint16_t    dest      = CastValue( planeValue );
                    const float planeDist = static_cast<float>( current ) - static_cast<float>( dest );
                    dest                  = CastValue( static_cast<float>( current ) - planeDist * strength );
                    switch ( flatten.Mode )
                    {
                        case LandscapeFlattenMode::Raise:
                            if ( planeDist < 0 )
                                current = TowardsFloorCeil( current, dest, strength, false );
                            break;
                        case LandscapeFlattenMode::Lower:
                            if ( planeDist > 0 )
                                current = TowardsFloorCeil( current, dest, strength, true );
                            break;
                        default: // UE: Interval and Terrace fall to Both under slope flatten
                            current = TowardsFloorCeil( current, dest, strength, planeDist > 0 );
                            break;
                    }
                }
            }
        return m_Cache.SetCachedData( rect.X1, rect.Z1, rect.X2, rect.Z2, data );
    }

    Common::BoolResultStr LandscapeHeightStroke::ApplyNoise( const LandscapeBrushWeights&  weights,
                                                             const LandscapeBrushSettings& brush,
                                                             const LandscapeNoiseSettings& noise )
    {
        auto valid = ValidateLandscapeNoise( noise );
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
        std::vector<uint16_t> data = read.GetValue();

        const float brushSizeAdjust = brush.RadiusCm < kLandscapeNoiseMaximumValueRadiusCm
                                           ? brush.RadiusCm / kLandscapeNoiseMaximumValueRadiusCm
                                           : 1.0f;
        // ToolTarget::StrengthMultiplier for the heightmap: the brush radius in height steps.
        const float   multiplier = brush.RadiusCm * kLandscapeStepsPerLocal / m_Root.ZScale;
        const int32_t width      = rect.X2 - rect.X1 + 1;
        for ( int32_t z = b.Z1; z <= b.Z2; ++z )
            for ( int32_t x = b.X1; x <= b.X2; ++x )
            {
                const float brushValue = BrushValue( weights, brush, x, z );
                if ( !( brushValue > 0.0f ) )
                    continue;
                uint16_t&   current       = data[static_cast<size_t>( ( z - rect.Z1 ) * width + ( x - rect.X1 ) )];
                const float totalStrength = brushValue * brush.Strength * multiplier;
                const float amount        = totalStrength * brushSizeAdjust;
                const float paint         = NoiseModeConversion( noise.Mode, amount,
                                                                 LandscapeNoiseSample( x, z, noise.NoiseScale ) * amount );
                current = ClampValue( static_cast<int64_t>( static_cast<float>( current ) + paint ) );
            }
        return m_Cache.SetCachedData( rect.X1, rect.Z1, rect.X2, rect.Z2, data );
    }

    Common::BoolResultStr LandscapeHeightStroke::ApplyErase( const LandscapeBrushWeights&  weights,
                                                             const LandscapeBrushSettings& brush )
    {
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
        std::vector<uint16_t> data = read.GetValue();

        const float   flattenHeight = static_cast<float>( TexHeight( 0.0f ) );
        const int32_t width         = rect.X2 - rect.X1 + 1;
        for ( int32_t z = b.Z1; z <= b.Z2; ++z )
            for ( int32_t x = b.X1; x <= b.X2; ++x )
            {
                const float brushValue = BrushValue( weights, brush, x, z );
                if ( !( brushValue > 0.0f ) )
                    continue;
                const float strength = std::clamp( brushValue * brush.Strength, 0.0f, 1.0f );
                uint16_t&   current  = data[static_cast<size_t>( ( z - rect.Z1 ) * width + ( x - rect.X1 ) )];
                current = TowardsFloorCeil( current, flattenHeight, strength, current > kLandscapeMidSample );
            }
        return m_Cache.SetCachedData( rect.X1, rect.Z1, rect.X2, rect.Z2, data );
    }

    Common::BoolResultStr ValidateLandscapeRamp( const LandscapeRampSettings& s )
    {
        if ( !( s.WidthCm >= kLandscapeMinRampWidthCm ) )
            return Common::MakeError( "landscape ramp: width " + std::to_string( s.WidthCm ) + " cm below " +
                                      std::to_string( kLandscapeMinRampWidthCm ) );
        if ( !( s.SideFalloff >= 0.0f && s.SideFalloff <= 1.0f ) )
            return Common::MakeError( "landscape ramp: side falloff " + std::to_string( s.SideFalloff ) +
                                      " outside 0..1" );
        return Common::MakeSuccess( true );
    }

    Common::BoolResultStr LandscapeHeightStroke::ApplyRamp( glm::vec3 startCm, glm::vec3 endCm,
                                                            const LandscapeRampSettings& ramp )
    {
        auto valid = ValidateLandscapeRamp( ramp );
        if ( !valid.IsSuccess() )
            return valid;
        // UE's Points are landscape-local: X/Y in samples, Z in world units above the actor. Our lattice is X/Z
        // with Y up, so UE's (X, Y) is our (X, Z).
        const auto toLattice = [&]( const glm::vec3& p )
        {
            return glm::dvec2( ( static_cast<double>( p.x ) - m_Root.Origin.x ) / m_Root.SpacingCm,
                               ( static_cast<double>( p.z ) - m_Root.Origin.z ) / m_Root.SpacingCm );
        };
        const std::array<glm::dvec2, 2> points{ toLattice( startCm ), toLattice( endCm ) };
        // Cross(P1 - P0, Up) in UE's frame, then GetSafeNormal.
        const glm::dvec2 dir = points[1] - points[0];
        const glm::dvec2 cross( dir.y, -dir.x );
        const double     len = glm::length( cross );
        if ( !( len > 1.e-8 ) )
            return Common::MakeError( "landscape ramp: start and end coincide at lattice (" +
                                      std::to_string( points[0].x ) + ", " + std::to_string( points[0].y ) + ")" );
        const glm::dvec2 side      = cross / len;
        const glm::dvec2 innerSide = side *
                                     static_cast<double>( ramp.WidthCm * 0.5f * ( 1.0f - ramp.SideFalloff ) ) /
                                     static_cast<double>( m_Root.SpacingCm );
        const glm::dvec2 outerSide =
             side * static_cast<double>( ramp.WidthCm * 0.5f ) / static_cast<double>( m_Root.SpacingCm );

        std::array<std::array<glm::dvec2, 2>, 2> inner{};
        std::array<std::array<glm::dvec2, 2>, 2> outer{};
        for ( size_t i = 0; i < 2; ++i )
        {
            inner[i] = { points[i] - innerSide, points[i] + innerSide };
            outer[i] = { points[i] - outerSide, points[i] + outerSide };
        }
        // Points.Z * LANDSCAPE_INV_ZSCALE + MidValue, unclamped: the raster policy clamps each sample.
        const auto toSamples = [&]( const glm::vec3& p )
        {
            return ( static_cast<double>( p.y ) - m_Root.Origin.y ) / m_Root.ZScale * kLandscapeStepsPerLocal +
                   kLandscapeMidSample;
        };
        const std::array<double, 2> heights{ toSamples( startCm ), toSamples( endCm ) };

        double lox = outer[0][0].x, loy = outer[0][0].y, hix = lox, hiy = loy;
        for ( const auto& end : outer )
            for ( const glm::dvec2& v : end )
            {
                lox = std::min( lox, v.x );
                loy = std::min( loy, v.y );
                hix = std::max( hix, v.x );
                hiy = std::max( hiy, v.y );
            }
        // "+/- 1 to make sure we have enough data for calculating correct normals"
        const LandscapeSampleBounds rect = Intersect(
             { static_cast<int32_t>( std::ceil( lox ) ) - 1, static_cast<int32_t>( std::ceil( loy ) ) - 1,
               static_cast<int32_t>( std::floor( hix ) ) + 1, static_cast<int32_t>( std::floor( hiy ) ) + 1 },
             m_Bounds );
        // "The bounds don't intersect any data, so we skip applying the ramp entirely"
        if ( rect.Empty() )
            return Common::MakeSuccess( true );
        auto cached = Cache( rect );
        if ( !cached.IsSuccess() )
            return cached;
        auto read = m_Cache.GetCachedData( rect.X1, rect.Z1, rect.X2, rect.Z2 );
        if ( !read.IsSuccess() )
            return Common::MakeError( read.GetError() );
        std::vector<uint16_t> data = read.GetValue();

        RampRasterizer raster( { data, rect.X1, rect.Z1, rect.X2, rect.Z2, ramp.Mode != LandscapeRampMode::Lower,
                                 ramp.Mode != LandscapeRampMode::Raise } );
        const RampInterpolant h00( 0, heights[0] ), h10( 1, heights[0] ), h01( 0, heights[1] ),
             h11( 1, heights[1] );
        // Left
        raster.DrawTriangle( h00, h10, h01, outer[0][0], inner[0][0], outer[1][0] );
        raster.DrawTriangle( h10, h01, h11, inner[0][0], outer[1][0], inner[1][0] );
        // Center
        raster.DrawTriangle( h10, h10, h11, inner[0][0], inner[0][1], inner[1][0] );
        raster.DrawTriangle( h10, h11, h11, inner[0][1], inner[1][0], inner[1][1] );
        // Right
        raster.DrawTriangle( h10, h00, h11, inner[0][1], outer[0][1], inner[1][1] );
        raster.DrawTriangle( h00, h11, h01, outer[0][1], inner[1][1], outer[1][1] );
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

    Common::BoolResultStr ValidateLandscapeErosion( const LandscapeErosionSettings& s )
    {
        if ( s.Threshold < 0 || s.Threshold > kLandscapeMaxErosionThreshold )
            return Common::MakeError( "landscape erosion: threshold " + std::to_string( s.Threshold ) +
                                      " outside 0.." + std::to_string( kLandscapeMaxErosionThreshold ) );
        if ( s.Iterations < 1 || s.Iterations > kLandscapeMaxErosionIterations )
            return Common::MakeError( "landscape erosion: iterations " + std::to_string( s.Iterations ) +
                                      " outside 1.." + std::to_string( kLandscapeMaxErosionIterations ) );
        if ( !( s.NoiseScale >= kLandscapeMinNoiseScale && s.NoiseScale <= kLandscapeMaxNoiseScale ) )
            return Common::MakeError( "landscape erosion: noise scale " + std::to_string( s.NoiseScale ) +
                                      " outside " + std::to_string( kLandscapeMinNoiseScale ) + ".." +
                                      std::to_string( kLandscapeMaxNoiseScale ) );
        return Common::MakeSuccess( true );
    }

    Common::BoolResultStr ValidateLandscapeHydroErosion( const LandscapeHydroErosionSettings& s )
    {
        if ( s.RainAmount < 1 || s.RainAmount > kLandscapeMaxRainAmount )
            return Common::MakeError( "landscape hydro erosion: rain amount " + std::to_string( s.RainAmount ) +
                                      " outside 1.." + std::to_string( kLandscapeMaxRainAmount ) );
        if ( !( s.SedimentCapacity >= kLandscapeMinSedimentCapacity && s.SedimentCapacity <= 1.0f ) )
            return Common::MakeError( "landscape hydro erosion: sediment capacity " +
                                      std::to_string( s.SedimentCapacity ) + " outside " +
                                      std::to_string( kLandscapeMinSedimentCapacity ) + "..1" );
        if ( s.Iterations < 1 || s.Iterations > kLandscapeMaxErosionIterations )
            return Common::MakeError( "landscape hydro erosion: iterations " + std::to_string( s.Iterations ) +
                                      " outside 1.." + std::to_string( kLandscapeMaxErosionIterations ) );
        if ( !( s.RainScale >= kLandscapeMinNoiseScale && s.RainScale <= kLandscapeMaxNoiseScale ) )
            return Common::MakeError( "landscape hydro erosion: rain scale " + std::to_string( s.RainScale ) +
                                      " outside " + std::to_string( kLandscapeMinNoiseScale ) + ".." +
                                      std::to_string( kLandscapeMaxNoiseScale ) );
        if ( !( s.DetailScale >= 0.0f && s.DetailScale <= kLandscapeMaxHydroDetailScale ) )
            return Common::MakeError( "landscape hydro erosion: detail scale " + std::to_string( s.DetailScale ) +
                                      " outside 0.." + std::to_string( kLandscapeMaxHydroDetailScale ) );
        return Common::MakeSuccess( true );
    }

    namespace
    {
        /// Index helpers over an erosion field: @p Rect for heights, @p Inner for brush values.
        struct FieldIndex
        {
            const LandscapeErosionField& F;

            size_t At( int32_t x, int32_t z ) const
            {
                return static_cast<size_t>( ( z - F.Rect.Z1 ) * ( F.Rect.X2 - F.Rect.X1 + 1 ) +
                                            ( x - F.Rect.X1 ) );
            }

            float Brush( int32_t x, int32_t z ) const
            {
                return F.Brush[static_cast<size_t>( ( z - F.Inner.Z1 ) * ( F.Inner.X2 - F.Inner.X1 + 1 ) +
                                                    ( x - F.Inner.X1 ) )];
            }
        };

        /// A float amount as UE's (uint16) cast takes it, clamped first: a float outside uint16 converts with
        /// undefined behaviour in C++ (UE relies on it not happening).
        uint16_t ToSample( float v )
        {
            return static_cast<uint16_t>( std::clamp( v, 0.0f, static_cast<float>( kLandscapeMaxSample ) ) );
        }

        /// UE casts ELandscapeToolErosionMode to ELandscapeToolNoiseMode: Both / Raise / Lower = Both / Add / Sub.
        LandscapeNoiseMode ErosionNoiseMode( LandscapeErosionNoiseMode mode )
        {
            switch ( mode )
            {
                case LandscapeErosionNoiseMode::Raise:
                    return LandscapeNoiseMode::Add;
                case LandscapeErosionNoiseMode::Lower:
                    return LandscapeNoiseMode::Sub;
                case LandscapeErosionNoiseMode::Both:
                    break;
            }
            return LandscapeNoiseMode::Both;
        }
    } // namespace

    int32_t LandscapeThermalErosion( LandscapeErosionField& field, const LandscapeErosionSettings& s,
                                     float strength )
    {
        // UE's slider stops at 1 (ToolStrength UIMax). Above it every shed hands a neighbour more than the slope
        // it came from, the pair swaps with a larger step each iteration and the stroke grows spikes up to the
        // height limit, so the strength that reaches the shed is capped at 1, as Smooth caps its own.
        strength = std::clamp( strength, 0.0f, 1.0f );
        const FieldIndex       ix{ field };
        std::vector<uint16_t>& h      = field.Heights;
        const uint16_t         thresh = static_cast<uint16_t>( s.Threshold );
        const auto             weight = [&]( int32_t x, int32_t z ) -> float
        {
            const bool inside =
                 x >= field.Inner.X1 && x <= field.Inner.X2 && z >= field.Inner.Z1 && z <= field.Inner.Z2;
            return inside ? ix.Brush( x, z ) : 0.0f;
        };
        // Deviation from UE (LandscapeEdModeErosionTools.cpp:141-257). UE weighs the gate (Slope * BrushValue >
        // Thresh) and the shed by the brush and lets every neighbour in the rectangle receive, so the stroke
        // conserves height inside the brush and a sample of weight w holds any slope up to Thresh / w. For a hill
        // smaller than the full-weight disc that is the angle of repose (the hill slumps into a cone, mass kept
        // to UE's truncation), but a hill as wide as the brush cannot fit its repose cone inside the circle: a
        // 25000-step hill of the default brush holds 9.3e6 step-samples, ~7000 steps over the brush's area, so
        // any loop that keeps the mass inside the brush ends in a plateau with a wall at the rim (UE: steepest
        // step 1914 -> 8769). Weighing only the shed (not the gate) does not help: the rim's weight throttles
        // the outflow and the wall stays (5710 in the same scenario).
        // Here the loop runs at full weight over every sample the brush touches, with UE's unweighted gate
        // (its second loop's Slope > Thresh), and height shed onto a sample the brush does not touch leaves the
        // field: the ground outside is not part of the stroke. A hill smaller than the brush slumps to the
        // threshold slope and keeps its mass, as in UE; only height that reaches the rim is lost. The result is
        // NOT blended back by the brush weight: that blend kept part of the original height wherever the
        // slumping cone landed in the falloff, lost 13% of a half-brush hill's mass and left a ring at its foot.
        int32_t ran = 0;
        for ( int32_t i = 0; i < s.Iterations; ++i )
        {
            ++ran;
            bool changed = false;
            for ( int32_t z = field.Inner.Z1; z <= field.Inner.Z2; ++z )
                for ( int32_t x = field.Inner.X1; x <= field.Inner.X2; ++x )
                {
                    if ( !( ix.Brush( x, z ) > 0.0f ) )
                        continue;
                    const size_t                center    = ix.At( x, z );
                    const std::array<size_t, 4> neighbour = { ix.At( x - 1, z ), ix.At( x + 1, z ),
                                                              ix.At( x, z - 1 ), ix.At( x, z + 1 ) };
                    const std::array<bool, 4>   receives  = { weight( x - 1, z ) > 0.0f, weight( x + 1, z ) > 0.0f,
                                                              weight( x, z - 1 ) > 0.0f, weight( x, z + 1 ) > 0.0f };
                    uint32_t                    slopeTotal = 0;
                    uint16_t                    slopeMax   = thresh;
                    for ( const size_t n : neighbour )
                        if ( h[center] > h[n] )
                        {
                            const uint16_t slope = static_cast<uint16_t>( h[center] - h[n] );
                            if ( slope > thresh )
                            {
                                slopeTotal += slope;
                                slopeMax = std::max( slopeMax, slope );
                            }
                        }
                    if ( slopeTotal == 0 )
                        continue;
                    // UE's Softness stays 1: layer hardness needs paint layers the heightmap target lacks.
                    float totalHeightDiff = 0.0f;
                    for ( size_t k = 0; k < neighbour.size(); ++k )
                    {
                        const size_t n = neighbour[k];
                        if ( h[center] <= h[n] )
                            continue;
                        const uint16_t slope = static_cast<uint16_t>( h[center] - h[n] );
                        if ( slope <= thresh )
                            continue;
                        const float heightDiff =
                             static_cast<float>( slopeMax - thresh ) * strength *
                             ( static_cast<float>( slope ) / static_cast<float>( slopeTotal ) );
                        if ( receives[k] )
                            h[n] = ClampValue( static_cast<int64_t>( h[n] ) + ToSample( heightDiff ) );
                        totalHeightDiff += heightDiff;
                    }
                    h[center] = ClampValue( static_cast<int64_t>( h[center] ) - ToSample( totalHeightDiff ) );
                    changed   = true;
                }
            if ( !changed )
                break;
        }
        return ran;
    }

    void LandscapeErosionNoise( LandscapeErosionField& field, const LandscapeErosionSettings& s, float strength,
                                float radiusCm )
    {
        const FieldIndex         ix{ field };
        const LandscapeNoiseMode mode            = ErosionNoiseMode( s.NoiseMode );
        const float              brushSizeAdjust = radiusCm < kLandscapeNoiseMaximumValueRadiusCm
                                                        ? radiusCm / kLandscapeNoiseMaximumValueRadiusCm
                                                        : 1.0f;
        for ( int32_t z = field.Inner.Z1; z <= field.Inner.Z2; ++z )
            for ( int32_t x = field.Inner.X1; x <= field.Inner.X2; ++x )
            {
                const float brushValue = ix.Brush( x, z );
                if ( !( brushValue > 0.0f ) )
                    continue;
                const float amount = brushValue * static_cast<float>( s.Threshold ) * strength * brushSizeAdjust;
                const float paint =
                     NoiseModeConversion( mode, amount, LandscapeNoiseSample( x, z, s.NoiseScale ) * amount );
                uint16_t& current = field.Heights[ix.At( x, z )];
                current           = ClampValue( static_cast<int64_t>( static_cast<float>( current ) + paint ) );
            }
    }

    int32_t LandscapeHydraulicErosion( LandscapeErosionField& field, const LandscapeHydroErosionSettings& s,
                                       float strength )
    {
        const FieldIndex       ix{ field };
        std::vector<uint16_t>& height = field.Heights;
        std::vector<uint16_t>  water( height.size(), 0 );
        std::vector<uint16_t>  sediment( height.size(), 0 );
        const float            dissolvingRatio  = 0.07f * strength;
        const float            evaporateRatio   = 0.5f;
        const float            sedimentCapacity = 0.10f * s.SedimentCapacity;
        const float            rainAmount       = static_cast<float>( s.RainAmount );
        // UE casts RainDistMode to the noise mode: Both = Both, Positive = Add.
        const LandscapeNoiseMode rainMode =
             s.RainMode == LandscapeRainMode::Positive ? LandscapeNoiseMode::Add : LandscapeNoiseMode::Both;

        for ( int32_t z = field.Inner.Z1; z <= field.Inner.Z2; ++z )
            for ( int32_t x = field.Inner.X1; x <= field.Inner.X2; ++x )
                if ( ix.Brush( x, z ) >= 1.0f )
                {
                    const float paint = NoiseModeConversion(
                         rainMode, rainAmount, LandscapeNoiseSample( x, z, s.RainScale ) * rainAmount );
                    if ( paint > 0.0f ) // "Raining only for positive region"
                        water[ix.At( x, z )] = static_cast<uint16_t>( water[ix.At( x, z )] + ToSample( paint ) );
                }

        int32_t ran = 0;
        for ( int32_t i = 0; i < s.Iterations; ++i )
        {
            ++ran;
            bool waterExists = false;
            for ( int32_t z = field.Inner.Z1; z <= field.Inner.Z2; ++z )
                for ( int32_t x = field.Inner.X1; x <= field.Inner.X2; ++x )
                {
                    const float brushValue = ix.Brush( x, z );
                    if ( !( brushValue > 0.0f ) )
                        continue;
                    const size_t                center    = ix.At( x, z );
                    const std::array<size_t, 8> neighbour = { ix.At( x - 1, z ),     ix.At( x + 1, z ),
                                                              ix.At( x, z - 1 ),     ix.At( x, z + 1 ),
                                                              ix.At( x - 1, z - 1 ), ix.At( x + 1, z + 1 ),
                                                              ix.At( x + 1, z - 1 ), ix.At( x - 1, z + 1 ) };

                    const float dissolved = dissolvingRatio * static_cast<float>( water[center] ) * brushValue;
                    if ( dissolved > 0.0f && static_cast<float>( height[center] ) >= dissolved )
                    {
                        height[center]   = static_cast<uint16_t>( height[center] - ToSample( dissolved ) );
                        sediment[center] = static_cast<uint16_t>( sediment[center] + ToSample( dissolved ) );
                    }

                    uint32_t                totalHeightDiff   = 0;
                    uint32_t                totalAltitudeDiff = 0;
                    std::array<uint32_t, 8> altitudeDiff{};
                    uint32_t                totalWaterDiff    = 0;
                    uint32_t                totalSedimentDiff = 0;
                    const uint32_t          altitude          = height[center] + water[center];
                    float                   averageAltitude   = 0.0f;
                    uint32_t                lowerNeighbours   = 0;
                    for ( size_t k = 0; k < neighbour.size(); ++k )
                    {
                        const uint32_t neighbourAltitude = height[neighbour[k]] + water[neighbour[k]];
                        if ( altitude > neighbourAltitude )
                        {
                            altitudeDiff[k] = altitude - neighbourAltitude;
                            totalAltitudeDiff += altitudeDiff[k];
                            ++lowerNeighbours;
                            averageAltitude += static_cast<float>( neighbourAltitude );
                            if ( height[center] > height[neighbour[k]] )
                                totalHeightDiff += static_cast<uint32_t>( height[center] - height[neighbour[k]] );
                        }
                    }
                    if ( lowerNeighbours > 0 )
                    {
                        averageAltitude /= static_cast<float>( lowerNeighbours );
                        if ( totalHeightDiff != 0 )
                            averageAltitude *= std::max(
                                 0.0f, static_cast<float>( 1.0f - 0.1 * static_cast<double>( strength ) ) );
                        const uint32_t waterTransfer = static_cast<uint32_t>(
                             static_cast<float>( std::min<uint32_t>(
                                  water[center], altitude - static_cast<uint32_t>( averageAltitude ) ) ) *
                             brushValue );
                        for ( size_t k = 0; k < neighbour.size(); ++k )
                        {
                            if ( altitudeDiff[k] == 0 )
                                continue;
                            const uint32_t waterDiff = static_cast<uint32_t>(
                                 static_cast<float>( waterTransfer ) * static_cast<float>( altitudeDiff[k] ) /
                                 static_cast<float>( totalAltitudeDiff ) );
                            water[neighbour[k]] = static_cast<uint16_t>( water[neighbour[k]] + waterDiff );
                            totalWaterDiff += waterDiff;
                            const uint32_t sedimentDiff =
                                 water[center] > 0
                                      ? static_cast<uint32_t>( static_cast<float>( sediment[center] ) *
                                                               static_cast<float>( waterDiff ) /
                                                               static_cast<float>( water[center] ) )
                                      : 0u;
                            sediment[neighbour[k]] =
                                 static_cast<uint16_t>( sediment[neighbour[k]] + sedimentDiff );
                            totalSedimentDiff += sedimentDiff;
                        }
                        water[center]    = static_cast<uint16_t>( water[center] - totalWaterDiff );
                        sediment[center] = static_cast<uint16_t>( sediment[center] - totalSedimentDiff );
                    }

                    if ( water[center] > 0 )
                    {
                        waterExists = true;
                        water[center] =
                             ToSample( static_cast<float>( water[center] ) * ( 1.0f - evaporateRatio ) );
                        const float capacity     = sedimentCapacity * static_cast<float>( water[center] );
                        const float sedimentDiff = static_cast<float>( sediment[center] ) - capacity;
                        if ( sedimentDiff > 0.0f )
                        {
                            sediment[center] =
                                 static_cast<uint16_t>( sediment[center] - ToSample( sedimentDiff ) );
                            height[center] = ToSample( static_cast<float>( height[center] ) + sedimentDiff );
                        }
                    }
                }
            if ( !waterExists )
                break;
        }

        if ( s.DetailSmooth )
            LowPassFilter( height, field.Rect, field.Inner, field.Brush, s.DetailScale, 1.0f );
        return ran;
    }

    Common::ResultStr<LandscapeErosionField>
    LandscapeHeightStroke::ErosionField( const LandscapeBrushWeights&  weights,
                                         const LandscapeBrushSettings& brush )
    {
        LandscapeErosionField field;
        if ( weights.Empty() )
            return Common::MakeSuccess( std::move( field ) );
        field.Rect = StepRect( weights );
        field.Inner =
             Intersect( BrushRect( weights ), LandscapeSampleBounds{ field.Rect.X1 + 1, field.Rect.Z1 + 1,
                                                                     field.Rect.X2 - 1, field.Rect.Z2 - 1 } );
        if ( field.Rect.Empty() || field.Inner.Empty() )
            return Common::MakeSuccess( std::move( field ) );
        auto cached = Cache( field.Rect );
        if ( !cached.IsSuccess() )
            return Common::MakeError<LandscapeErosionField>( cached.GetError() );
        auto read = m_Cache.GetCachedData( field.Rect.X1, field.Rect.Z1, field.Rect.X2, field.Rect.Z2 );
        if ( !read.IsSuccess() )
            return Common::MakeError<LandscapeErosionField>( read.GetError() );
        field.Heights = read.GetValue();
        field.Brush   = BrushValues( weights, brush, field.Inner );
        return Common::MakeSuccess( std::move( field ) );
    }

    Common::BoolResultStr LandscapeHeightStroke::ApplyErosion( const LandscapeBrushWeights&    weights,
                                                               const LandscapeBrushSettings&   brush,
                                                               const LandscapeErosionSettings& erosion )
    {
        auto valid = ValidateLandscapeErosion( erosion );
        if ( !valid.IsSuccess() )
            return valid;
        auto read = ErosionField( weights, brush );
        if ( !read.IsSuccess() )
            return Common::MakeError( read.GetError() );
        LandscapeErosionField field = read.GetValue();
        if ( field.Brush.empty() )
            return Common::MakeSuccess( true );
        LandscapeThermalErosion( field, erosion, brush.Strength );
        LandscapeErosionNoise( field, erosion, brush.Strength, brush.RadiusCm );
        return m_Cache.SetCachedData( field.Rect.X1, field.Rect.Z1, field.Rect.X2, field.Rect.Z2, field.Heights );
    }

    Common::BoolResultStr LandscapeHeightStroke::ApplyHydroErosion( const LandscapeBrushWeights&         weights,
                                                                    const LandscapeBrushSettings&        brush,
                                                                    const LandscapeHydroErosionSettings& hydro )
    {
        auto valid = ValidateLandscapeHydroErosion( hydro );
        if ( !valid.IsSuccess() )
            return valid;
        auto read = ErosionField( weights, brush );
        if ( !read.IsSuccess() )
            return Common::MakeError( read.GetError() );
        LandscapeErosionField field = read.GetValue();
        if ( field.Brush.empty() )
            return Common::MakeSuccess( true );
        LandscapeHydraulicErosion( field, hydro, brush.Strength );
        return m_Cache.SetCachedData( field.Rect.X1, field.Rect.Z1, field.Rect.X2, field.Rect.Z2, field.Heights );
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
