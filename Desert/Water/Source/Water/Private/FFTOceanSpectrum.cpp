#include <Water/Public/FFTOceanSpectrum.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace Desert::Water
{
    namespace
    {
        // SI gravity for the JONSWAP fit (its constants are dimensionless in SI); the result is converted to cm.
        constexpr double kGravitySI = 9.81;
        constexpr double kTwoPi     = 2.0 * std::numbers::pi;

        // SplitMix64 (Steele, Lea, Flood 2014): a counter-based stream, so a texel's numbers depend only on
        // (seed, cascade, texel) - the same h0 on every machine and in every build, independent of fill order.
        uint64_t SplitMix64( uint64_t x ) noexcept
        {
            x += 0x9E3779B97F4A7C15ull;
            x = ( x ^ ( x >> 30 ) ) * 0xBF58476D1CE4E5B9ull;
            x = ( x ^ ( x >> 27 ) ) * 0x94D049BB133111EBull;
            return x ^ ( x >> 31 );
        }

        // (0, 1], never 0: Box-Muller takes its logarithm.
        double UnitOpen( uint64_t bits ) noexcept
        {
            return ( static_cast<double>( bits >> 11 ) + 1.0 ) * ( 1.0 / 9007199254740992.0 );
        }

        // Two independent standard normals of one texel (Box-Muller).
        void GaussianPair( uint64_t key, double& a, double& b ) noexcept
        {
            const double u1 = UnitOpen( SplitMix64( key ) );
            const double u2 = UnitOpen( SplitMix64( key ^ 0xD1B54A32D192ED03ull ) );
            const double r  = std::sqrt( -2.0 * std::log( u1 ) );
            a               = r * std::cos( kTwoPi * u2 );
            b               = r * std::sin( kTwoPi * u2 );
        }

        // The Mitsuyasu cos^2s(theta / 2) normalisation over [-pi, pi]: Gamma(s+1)^2 2^(2s-1) / (pi Gamma(2s+1)).
        double SpreadNormalisation( double s ) noexcept
        {
            return std::exp( 2.0 * std::lgamma( s + 1.0 ) + ( 2.0 * s - 1.0 ) * std::log( 2.0 ) -
                             std::lgamma( 2.0 * s + 1.0 ) ) /
                   std::numbers::pi;
        }

        bool PositiveFinite( float v ) noexcept
        {
            return std::isfinite( v ) && v > 0.0f;
        }

        bool NotNegativeFinite( float v ) noexcept
        {
            return std::isfinite( v ) && v >= 0.0f;
        }
    } // namespace

    const char* FFTOceanSpectrumParamsProblem( const FFTOceanSpectrumParams& p ) noexcept
    {
        if ( !PositiveFinite( p.FetchKm ) )
            return "FFT.FetchKm must be finite and above 0";
        if ( !std::isfinite( p.PeakEnhancement ) || p.PeakEnhancement < 1.0f )
            return "FFT.PeakEnhancement must be finite and at least 1";
        if ( !NotNegativeFinite( p.DirectionalSpread ) )
            return "FFT.DirectionalSpread must be finite and not negative";
        if ( !std::isfinite( p.CounterWaves ) || p.CounterWaves < 0.0f || p.CounterWaves > 1.0f )
            return "FFT.CounterWaves must lie in [0, 1]";
        if ( !NotNegativeFinite( p.Choppiness ) )
            return "FFT.Choppiness must be finite and not negative";
        if ( !NotNegativeFinite( p.HeightScale ) )
            return "FFT.HeightScale must be finite and not negative";
        if ( !PositiveFinite( p.ShortestWavelength ) )
            return "FFT.ShortestWavelength must be finite and above 0";
        for ( uint32_t c = 0; c < kFFTOceanCascades; ++c )
        {
            if ( !PositiveFinite( p.CascadeSizes[c] ) )
                return "FFT.CascadeSizes must be finite and above 0";
            if ( c > 0 && !( p.CascadeSizes[c] < p.CascadeSizes[c - 1] ) )
                return "FFT.CascadeSizes must be strictly decreasing (largest patch first)";
        }
        // The smallest patch must resolve the shortest wave (its Nyquist wave number is pi N / L).
        if ( p.ShortestWavelength * static_cast<float>( kFFTOceanResolution ) * 0.5f <
             p.CascadeSizes[kFFTOceanCascades - 1] )
            return "FFT.ShortestWavelength is below what the smallest cascade resolves (2 * size / 256)";
        return nullptr;
    }

    float FFTOceanSpectrumDensity( const FFTOceanSpectrumParams& p, const FFTOceanWind& wind, float kx,
                                   float kz ) noexcept
    {
        const double kCm = std::sqrt( static_cast<double>( kx ) * kx + static_cast<double>( kz ) * kz );
        if ( kCm <= 0.0 || kCm > kTwoPi / p.ShortestWavelength )
            return 0.0f;
        // Still air raises no wind sea (JONSWAP's alpha and peak both go to 0 with U); a non-finite wind is no
        // wind the scene can state.
        if ( !PositiveFinite( wind.Speed ) || !std::isfinite( wind.AngleDeg ) )
            return 0.0f;

        // JONSWAP in SI (Hasselmann 1973, the fetch-limited fit of Hasselmann et al. 1976).
        const double U      = static_cast<double>( wind.Speed ) * 0.01;
        const double F      = static_cast<double>( p.FetchKm ) * 1000.0;
        const double alpha  = 0.076 * std::pow( U * U / ( F * kGravitySI ), 0.22 );
        const double omegaP = 22.0 * std::cbrt( kGravitySI * kGravitySI / ( U * F ) );
        const double kM     = kCm * 100.0;
        const double omega  = std::sqrt( kGravitySI * kM );
        const double sigma  = omega <= omegaP ? 0.07 : 0.09;
        const double peak =
             std::exp( -( omega - omegaP ) * ( omega - omegaP ) / ( 2.0 * sigma * sigma * omegaP * omegaP ) );
        const double S = alpha * kGravitySI * kGravitySI / std::pow( omega, 5.0 ) *
                         std::exp( -1.25 * std::pow( omegaP / omega, 4.0 ) ) *
                         std::pow( static_cast<double>( p.PeakEnhancement ), peak );

        // Direction: Mitsuyasu cos^2s about the wind, the waves against it kept at CounterWaves.
        const double windRad = static_cast<double>( wind.AngleDeg ) * std::numbers::pi / 180.0;
        const double theta   = std::atan2( static_cast<double>( kz ), static_cast<double>( kx ) ) - windRad;
        const double s       = static_cast<double>( p.DirectionalSpread );
        double       D       = SpreadNormalisation( s ) * std::pow( std::abs( std::cos( theta * 0.5 ) ), 2.0 * s );
        if ( std::cos( theta ) < 0.0 )
            D *= static_cast<double>( p.CounterWaves );

        // E(k) = S(omega) d(omega)/dk / k D(theta), m^4; to cm^2 per (rad/cm)^2: x 1e4 (height^2) x 1e4 (dk^2).
        const double dOmegaDk = kGravitySI / ( 2.0 * omega );
        const double E        = S * dOmegaDk / kM * D * 1.0e8;
        return static_cast<float>( E );
    }

    FFTOceanSpectrum BuildFFTOceanSpectrum( const FFTOceanSpectrumParams& p, const FFTOceanWind& wind ) noexcept
    {
        FFTOceanSpectrum out;
        out.Params = p;
        out.Wind   = wind;

        constexpr uint32_t N        = kFFTOceanResolution;
        constexpr int32_t  half     = static_cast<int32_t>( N / 2 );
        double             variance = 0.0;
        for ( uint32_t c = 0; c < kFFTOceanCascades; ++c )
        {
            FFTOceanCascade& cascade = out.Cascades[c];
            cascade.PatchSize        = p.CascadeSizes[c];
            // Each cascade owns one band of |k| (Jump Trajectory's split): from 6 fundamentals of its own patch
            // (the coarsest from its fundamental) up to 6 fundamentals of the next one; the finest up to the
            // shortest wave. So no wave is counted twice and each is drawn by the patch that resolves it best.
            cascade.BandLow  = c == 0 ? 0.0f : static_cast<float>( 6.0 * kTwoPi / p.CascadeSizes[c] );
            cascade.BandHigh = c + 1 < kFFTOceanCascades
                                    ? static_cast<float>( 6.0 * kTwoPi / p.CascadeSizes[c + 1] )
                                    : static_cast<float>( kTwoPi / p.ShortestWavelength );

            const double           dk = kTwoPi / static_cast<double>( cascade.PatchSize );
            std::vector<glm::vec2> h0( static_cast<size_t>( N ) * N, glm::vec2( 0.0f ) );
            for ( uint32_t y = 0; y < N; ++y )
            {
                for ( uint32_t x = 0; x < N; ++x )
                {
                    const double kx = static_cast<double>( static_cast<int32_t>( x ) - half ) * dk;
                    const double kz = static_cast<double>( static_cast<int32_t>( y ) - half ) * dk;
                    const double k  = std::sqrt( kx * kx + kz * kz );
                    if ( k < cascade.BandLow || k >= cascade.BandHigh || k == 0.0 )
                        continue;
                    const double E =
                         FFTOceanSpectrumDensity( p, wind, static_cast<float>( kx ), static_cast<float>( kz ) );
                    // E|h0|^2 = E dk^2 / 2 per mode: the pair (k, -k) of the real field then carries E dk^2, and
                    // the whole grid the spectrum's m0.
                    const double amplitude =
                         std::sqrt( E * dk * dk * 0.25 ) * static_cast<double>( p.HeightScale );
                    double         g0 = 0.0, g1 = 0.0;
                    const uint64_t key = ( static_cast<uint64_t>( static_cast<uint32_t>( p.Seed ) ) << 32 ) ^
                                         ( static_cast<uint64_t>( c ) << 24 ) ^
                                         ( static_cast<uint64_t>( y ) << 12 ) ^ static_cast<uint64_t>( x );
                    GaussianPair( key, g0, g1 );
                    h0[static_cast<size_t>( y ) * N + x] =
                         glm::vec2( static_cast<float>( g0 * amplitude ), static_cast<float>( g1 * amplitude ) );
                    variance += E * dk * dk * static_cast<double>( p.HeightScale ) * p.HeightScale;
                }
            }

            // Pair every texel with conj(h0(-k)): the texel mirrored through the centre (index N - i, wrapped).
            cascade.H0.resize( static_cast<size_t>( N ) * N );
            for ( uint32_t y = 0; y < N; ++y )
            {
                for ( uint32_t x = 0; x < N; ++x )
                {
                    const glm::vec2 a = h0[static_cast<size_t>( y ) * N + x];
                    const glm::vec2 m = h0[static_cast<size_t>( ( N - y ) % N ) * N + ( N - x ) % N];
                    cascade.H0[static_cast<size_t>( y ) * N + x] = glm::vec4( a.x, a.y, m.x, -m.y );
                }
            }
        }
        out.MaxWaveHeight = static_cast<float>( 3.0 * std::sqrt( variance ) );
        return out;
    }
} // namespace Desert::Water
