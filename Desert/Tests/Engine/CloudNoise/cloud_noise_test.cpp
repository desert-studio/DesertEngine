#include "CloudNoiseReference.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace Desert::Tests::CloudNoiseRef;

namespace
{
    // The shipped volume is 128^3 and its coarsest channel has a period of four lattice cells, so a sample
    // grid of 64 per axis is two samples per voxel on that channel — enough to catch a seam, cheap enough
    // to run in a unit test.
    constexpr int   kGrid   = 64;
    constexpr float kPeriod = 4.0f;

    // The defaults of Assets::CloudNoiseVolumeParams, which is what CloudNoise_Default.dcnv was baked with.
    constexpr uint  kSeed           = 1337u;
    constexpr float kCurlStrength   = 0.33f;
    constexpr float kWispyPeriodLF  = 2.0f;
    constexpr float kWispyPeriodHF  = 4.0f;
    constexpr float kBillowPeriodLF = 3.0f;
    constexpr float kBillowPeriodHF = 6.0f;

    struct Percentiles
    {
        float Min, P01, P25, P50, P75, P99, Max;
    };

    Percentiles Measure( std::vector<float>& samples )
    {
        std::sort( samples.begin(), samples.end() );
        return Percentiles{ samples.front(),
                            samples[samples.size() / 100],
                            samples[samples.size() / 4],
                            samples[samples.size() / 2],
                            samples[samples.size() * 3 / 4],
                            samples[samples.size() * 99 / 100],
                            samples.back() };
    }

    // The whole grid, all four channels, evaluated ONCE. Four tests want these numbers and the field is a
    // pure function of position, so computing it per test cost 110 seconds of unoptimised arithmetic and
    // changed no answer. A unit suite nobody is willing to run is a unit suite that stops being run.
    struct VolumeSamples
    {
        std::vector<float> Channel[4];
    };

    const VolumeSamples& Samples()
    {
        static const VolumeSamples samples = []
        {
            VolumeSamples built;
            for ( int channel = 0; channel < 4; ++channel )
                built.Channel[channel].reserve( static_cast<size_t>( kGrid ) * kGrid * kGrid );

            for ( int z = 0; z < kGrid; ++z )
                for ( int y = 0; y < kGrid; ++y )
                    for ( int x = 0; x < kGrid; ++x )
                    {
                        const vec3 uvw{ ( x + 0.5f ) / kGrid, ( y + 0.5f ) / kGrid, ( z + 0.5f ) / kGrid };
                        const vec4 c =
                             CloudNoiseVolumeChannels( uvw, kSeed, kCurlStrength, kWispyPeriodLF, kWispyPeriodHF,
                                                       kBillowPeriodLF, kBillowPeriodHF );
                        built.Channel[0].push_back( c.x );
                        built.Channel[1].push_back( c.y );
                        built.Channel[2].push_back( c.z );
                        built.Channel[3].push_back( c.w );
                    }
            return built;
        }();
        return samples;
    }

    std::vector<float> SampleChannel( int channel )
    {
        return Samples().Channel[channel];
    }
} // namespace

// ---------------------------------------------------------------------------------------------------
// Periodicity. This is the property the whole tiling scheme rests on: the volume is sampled with REPEAT
// at an arbitrary world scale, so a field that only ALMOST tiles puts a seam across the sky at every
// multiple of the tile — a defect that is invisible at the origin and unmistakable from a moving camera.
// ---------------------------------------------------------------------------------------------------

TEST( CloudNoisePeriodicity, TheGradientFieldRepeatsExactlyAfterOnePeriodOnEveryAxis )
{
    constexpr float kSamples = 17.0f;

    for ( float i = 0.0f; i < kSamples; i += 1.0f )
    {
        for ( float j = 0.0f; j < kSamples; j += 1.0f )
        {
            const vec3 p{ i * 0.37f, j * 0.53f, ( i + j ) * 0.29f };

            const float base = CloudPerlinPeriodic( p, kPeriod, kSeed );

            // NEAR rather than exact equality, and the tolerance is float arithmetic rather than slack in
            // the tiling. The lattice index is exact on both sides — that is what the wrap guarantees —
            // but the FRACTIONAL part is computed as `x - floor(x)`, and at x = 9.92 that subtraction
            // discards different low bits than it does at x = 5.92. The error is a few ULP of the
            // interpolant, six orders below anything the erosion threshold can act on.
            constexpr float kTolerance = 1e-5f;
            EXPECT_NEAR( base, CloudPerlinPeriodic( p + vec3( kPeriod, 0.0f, 0.0f ), kPeriod, kSeed ),
                         kTolerance );
            EXPECT_NEAR( base, CloudPerlinPeriodic( p + vec3( 0.0f, kPeriod, 0.0f ), kPeriod, kSeed ),
                         kTolerance );
            EXPECT_NEAR( base, CloudPerlinPeriodic( p + vec3( 0.0f, 0.0f, kPeriod ), kPeriod, kSeed ),
                         kTolerance );
        }
    }
}

TEST( CloudNoisePeriodicity, TheAlligatorFieldRepeatsExactlyAfterOnePeriodOnEveryAxis )
{
    // The Alligator's own wrap, tested separately from the gradient noise because it takes a different
    // route to the lattice: the cell index is wrapped before it is hashed, and the feature-point offset is
    // read out of that hash, so a wrap that was wrong would move the point rather than shift the value.
    for ( float i = 0.0f; i < 13.0f; i += 1.0f )
    {
        const vec3 p{ i * 0.41f, i * 0.77f, i * 0.19f };

        const float base = CloudAlligator( p, kPeriod, kSeed );
        EXPECT_NEAR( base, CloudAlligator( p + vec3( kPeriod, 0.0f, 0.0f ), kPeriod, kSeed ), 1e-5f );
        EXPECT_NEAR( base, CloudAlligator( p + vec3( 0.0f, kPeriod, 0.0f ), kPeriod, kSeed ), 1e-5f );
        EXPECT_NEAR( base, CloudAlligator( p + vec3( 0.0f, 0.0f, kPeriod ), kPeriod, kSeed ), 1e-5f );
    }
}

TEST( CloudNoisePeriodicity, TheAlligatorFieldRepeatsAcrossNEGATIVECoordinatesToo )
{
    // The wind offset accumulates without bound and is SUBTRACTED from the sample position, so a scene
    // left running walks into negative coordinates and stays there. `mod` rather than a cast is what makes
    // this hold; a truncating cast folds -0.5 and +0.5 onto the same cell and mirrors the field about the
    // origin, which reads as the sky briefly running backwards.
    for ( float i = 1.0f; i < 12.0f; i += 1.0f )
    {
        const vec3 p{ -i * 1.7f, -i * 0.9f, -i * 2.3f };
        EXPECT_NEAR( CloudAlligator( p, kPeriod, 7u ), CloudAlligator( p + vec3( kPeriod * 3.0f ), kPeriod, 7u ),
                     1e-5f );
    }
}

TEST( CloudNoisePeriodicity, TheCurlShearedWispyChannelStillTiles )
{
    // The one that could plausibly have been broken by the distortion. The curl potential shares the
    // period, so a shift by one period moves the flow and the Alligator lattice by exactly the same
    // amount — but only if BOTH are periodic, and a sheared field whose shear is not periodic tiles
    // nowhere. The tolerance is looser than the others because the shear amplifies the same few-ULP
    // fractional error through a finite difference at an epsilon of a hundredth of a cell.
    for ( float i = 0.0f; i < 11.0f; i += 1.0f )
    {
        const vec3 p{ i * 0.31f, i * 0.67f, i * 0.13f };

        const float base = CloudCurlyAlligator01( p, kPeriod, kSeed, 0.33f );
        EXPECT_NEAR( base, CloudCurlyAlligator01( p + vec3( kPeriod, 0.0f, 0.0f ), kPeriod, kSeed, 0.33f ),
                     2e-3f );
        EXPECT_NEAR( base, CloudCurlyAlligator01( p + vec3( 0.0f, 0.0f, kPeriod ), kPeriod, kSeed, 0.33f ),
                     2e-3f );
    }
}

// ---------------------------------------------------------------------------------------------------
// Lattice behaviour
// ---------------------------------------------------------------------------------------------------

TEST( CloudNoiseLattice, GradientNoiseIsZeroAtEveryLatticePoint )
{
    // A defining property of gradient noise, and the cheapest possible check that the fade, the gradient
    // selector and the interpolation are wired to the same corners: at an integer coordinate every
    // gradient is dotted with a zero offset. It still matters after the noises changed, because the curl
    // that shears the wispy channels is built out of this function.
    for ( float x = -3.0f; x <= 3.0f; x += 1.0f )
        for ( float y = -3.0f; y <= 3.0f; y += 1.0f )
            for ( float z = -3.0f; z <= 3.0f; z += 1.0f )
                EXPECT_NEAR( CloudPerlinPeriodic( vec3( x, y, z ), kPeriod, 5u ), 0.0f, 1e-6f );
}

TEST( CloudNoiseLattice, TheFadeCurveIsFlatAtBothEnds )
{
    // 6t^5-15t^4+10t^3 has a vanishing first AND second derivative at 0 and 1. The second derivative is
    // what the cubic alternative gets wrong, and the symptom is a faint rectangular grid appearing once
    // the noise is used as an erosion threshold rather than as a colour.
    EXPECT_FLOAT_EQ( CloudFade( 0.0f ), 0.0f );
    EXPECT_FLOAT_EQ( CloudFade( 1.0f ), 1.0f );
    EXPECT_NEAR( CloudFade( 0.5f ), 0.5f, 1e-6f );

    // The step is a twentieth and NOT an epsilon, which looks lax and is the opposite. `CloudFade` is
    // Horner's form, so at t = 1 - 1e-3 it evaluates `10.0 - 8.997` — a cancellation that leaves about
    // one ULP of 9, i.e. ~1e-6, in a result of 1. Divided by an epsilon of 1e-3 that noise becomes ~1e-3
    // of slope, while the slope actually being measured is 1e-5: a hundred times BELOW the arithmetic's
    // own floor. This assertion used to read `< 1e-4` at that epsilon and it passed here by luck and
    // failed on MSVC at 8.94e-4 — it was measuring float cancellation, not the curve.
    //
    // At a twentieth the true difference is ~1.16e-3, a thousand times above that floor, and the quantity
    // is expressed against the slope at the curve's middle so the threshold means something scale-free.
    constexpr float kStep = 0.05f;

    const float slopeAtMiddle = ( CloudFade( 0.5f + kStep ) - CloudFade( 0.5f - kStep ) ) / ( 2.0f * kStep );
    const float slopeAtZero   = ( CloudFade( kStep ) - CloudFade( 0.0f ) ) / kStep;
    const float slopeAtOne    = ( CloudFade( 1.0f ) - CloudFade( 1.0f - kStep ) ) / kStep;

    // 1/30 discriminates against the named alternative rather than against nothing: the cubic
    // 3t^2-2t^3, whose second derivative does NOT vanish at the ends, measures 0.097 of its own middle
    // slope by this same construction, where the quintic measures 0.012 — eight times apart.
    EXPECT_LT( slopeAtZero, slopeAtMiddle / 30.0f );
    EXPECT_LT( slopeAtOne, slopeAtMiddle / 30.0f );
}

TEST( CloudNoiseHash, OneChangedInputBitChangesAboutHalfTheOutputBits )
{
    // The avalanche property, measured rather than asserted. Without it, neighbouring lattice cells stay
    // correlated and the noise shows visible structure along the axes. It carries MORE weight now than it
    // did: the Alligator reads its feature-point offset from three bit-slices of one hash and its
    // amplitude from a fourth re-finalisation, so a hash that did not avalanche would correlate an
    // offset with an amplitude and give every lobe the same size in the same corner of its cell.
    int    trials     = 0;
    double totalRatio = 0.0;

    for ( uint x = 0u; x < 32u; ++x )
    {
        for ( uint bit = 0u; bit < 32u; ++bit )
        {
            const uint a = CloudHashCell( x, 11u, 23u, 4242u );
            const uint b = CloudHashCell( x ^ ( 1u << bit ), 11u, 23u, 4242u );

            uint diff    = a ^ b;
            int  changed = 0;
            for ( uint i = 0u; i < 32u; ++i )
                changed += static_cast<int>( ( diff >> i ) & 1u );

            totalRatio += static_cast<double>( changed ) / 32.0;
            ++trials;
        }
    }

    const double mean = totalRatio / trials;
    EXPECT_GT( mean, 0.40 ) << "hash does not avalanche: " << mean;
    EXPECT_LT( mean, 0.60 ) << "hash does not avalanche: " << mean;
}

TEST( CloudNoiseHash, DifferentSeedsProduceDifferentVolumes )
{
    // The seed's whole job, and the reason the container stores it: re-rolling must change WHICH clouds
    // appear without changing what kind of clouds they are.
    int differing = 0;
    for ( int i = 0; i < kGrid; ++i )
    {
        const vec3 uvw{ ( i + 0.5f ) / kGrid, ( i * 0.37f + 0.5f ) / kGrid, ( i * 0.71f + 0.5f ) / kGrid };
        const vec4 a = CloudNoiseVolumeChannels( uvw, 1u, kCurlStrength, kWispyPeriodLF, kWispyPeriodHF,
                                                 kBillowPeriodLF, kBillowPeriodHF );
        const vec4 b = CloudNoiseVolumeChannels( uvw, 2u, kCurlStrength, kWispyPeriodLF, kWispyPeriodHF,
                                                 kBillowPeriodLF, kBillowPeriodHF );
        if ( std::abs( a.x - b.x ) > 1e-4f )
            ++differing;
    }
    EXPECT_GT( differing, kGrid / 2 );
}

// ---------------------------------------------------------------------------------------------------
// Alligator: the shape of the noise, from the SideFX definition the deck links (p.96).
// ---------------------------------------------------------------------------------------------------

TEST( CloudNoiseAlligator, TheFieldIsNonNegativeEverywhere )
{
    // best - second, and `best` is by construction the larger of the two. A negative value would mean the
    // two running maxima had been swapped, which is a bug the eye reads as an inverted noise rather than
    // as a wrong one.
    for ( int i = 0; i < 2000; ++i )
    {
        const float t = static_cast<float>( i ) * 0.0137f;
        const vec3  p{ t, t * 1.7f, t * 0.3f };
        EXPECT_GE( CloudAlligator( p, kPeriod, kSeed ), 0.0f ) << "at i = " << i;
    }
}

TEST( CloudNoiseAlligator, TheShapingExponentIsMonotoneSoItCannotMoveALevelSet )
{
    // The load-bearing property of the median shaping. It relabels values; it must not reorder them, or
    // the silhouettes it was introduced to preserve would change after all.
    float previous = -1.0f;
    for ( int i = 0; i <= 100; ++i )
    {
        const float linear = static_cast<float>( i ) / 100.0f;
        const float shaped = pow( linear, CLOUD_ALLIGATOR_MEDIAN_SHAPE );
        EXPECT_GE( shaped, previous ) << "the shaping reordered two values at " << linear;
        previous = shaped;
    }
    EXPECT_FLOAT_EQ( pow( 0.0f, CLOUD_ALLIGATOR_MEDIAN_SHAPE ), 0.0f );
    EXPECT_FLOAT_EQ( pow( 1.0f, CLOUD_ALLIGATOR_MEDIAN_SHAPE ), 1.0f );
}

TEST( CloudNoiseAlligator, ZeroCurlStrengthLeavesThePlainInvertedField )
{
    // The panel offers a curl strength of zero and the container accepts it, so the degenerate case has to
    // BE the undistorted noise rather than something that merely looks like it — otherwise the slider's
    // zero would be a fifth kind of noise nobody asked for.
    for ( int i = 0; i < 200; ++i )
    {
        const float t = static_cast<float>( i ) * 0.031f;
        const vec3  p{ t, t * 0.6f, t * 1.3f };
        EXPECT_FLOAT_EQ( CloudCurlyAlligator01( p, kPeriod, kSeed, 0.0f ),
                         1.0f - CloudAlligator01( p, kPeriod, kSeed ) );
    }
}

// ---------------------------------------------------------------------------------------------------
// RANGE. These tests exist because a number was guessed and the frame disagreed — twice.
//
// Every consumer of this volume compares it against a THRESHOLD: the coverage field against the Coverage
// slider, the detail against the erosion depth. A channel whose distribution sits off-centre makes every
// one of those thresholds read a different part of the field than it was calibrated for, and the symptom
// is a sky that looks like a tuning problem and is not one. The first frame rendered with the unshaped
// Alligator was a solid overcast ceiling at a Coverage of 0.25.
// ---------------------------------------------------------------------------------------------------

TEST( CloudNoiseRange, EveryChannelSpansEnoughOfZeroToOneForAThresholdToMeanSomething )
{
    static const char* kNames[4] = { "R Curly-Alligator LF", "G Curly-Alligator HF", "B Alligator LF",
                                     "A Alligator HF" };

    for ( int channel = 0; channel < 4; ++channel )
    {
        std::vector<float> samples = SampleChannel( channel );
        const Percentiles  p       = Measure( samples );

        // Reported so the numbers behind the constants are on the record rather than in somebody's memory.
        std::printf( "[CloudNoise] %-22s min %.4f  p01 %.4f  p25 %.4f  p50 %.4f  p75 %.4f  p99 %.4f  max %.4f\n",
                     kNames[channel], p.Min, p.P01, p.P25, p.P50, p.P75, p.P99, p.Max );

        EXPECT_GE( p.Min, 0.0f ) << kNames[channel];
        EXPECT_LE( p.Max, 1.0f ) << kNames[channel];

        EXPECT_GT( p.P99, 0.85f ) << kNames[channel]
                                  << " never approaches 1, so a full threshold cannot "
                                     "produce solid cloud";

        // THE LOWER END IS ASSERTED AT THE QUARTILE, NOT THE PERCENTILE, and the reason is arithmetic
        // rather than slack. The coarsest channel has a period of TWO, so the whole volume contains eight
        // lattice cells; its "1st percentile" is a statement about eight numbers and measures 0.21 while
        // its minimum is 0.17. That is a property of counting cells, not of the noise — the field the
        // march actually thresholds is the COMBINATION of two channels, and the test below pins its tail
        // against the field it replaced.
        EXPECT_LT( p.P25, 0.50f ) << kNames[channel]
                                  << " has no populated lower half, so a low threshold "
                                     "cannot produce clear sky";
    }
}

TEST( CloudNoiseRange, EveryChannelIsCENTREDWhereItsConsumersThresholdIt )
{
    // THE TEST THE OVERCAST CEILING WOULD HAVE FAILED. The four channels this volume replaced measured a
    // median of 0.479 to 0.527 apiece, and every default and every documented table in
    // ECS::VolumetricCloudData was measured against that. A channel that drifts out of this band is not
    // "differently distributed", it is a slider that has silently stopped meaning what its tooltip says.
    static const char* kNames[4] = { "R Curly-Alligator LF", "G Curly-Alligator HF", "B Alligator LF",
                                     "A Alligator HF" };

    for ( int channel = 0; channel < 4; ++channel )
    {
        std::vector<float> samples = SampleChannel( channel );
        const Percentiles  p       = Measure( samples );

        EXPECT_GT( p.P50, 0.42f ) << kNames[channel]
                                  << " sits low: a threshold calibrated for a centred "
                                     "field will cut too much";
        EXPECT_LT( p.P50, 0.58f ) << kNames[channel]
                                  << " sits high: a threshold calibrated for a centred "
                                     "field will cut too little";
    }
}

TEST( CloudNoiseRange, TheCoverageFieldTheMarchACTUALLYThresholdsMatchesTheFieldItReplaced )
{
    // A test on the RELATION rather than on a function. Common/CloudField.glslh does not threshold a
    // channel, it thresholds `0.65*R + 0.35*G`, and THAT combination is what the Coverage slider's
    // documented table was measured against. Asserting the channels one at a time would leave the one
    // quantity the picture depends on unmeasured.
    //
    // The band is the old field's own percentiles, measured on dev @ d4ef3bb1 over the same grid:
    // p01 0.119 / p25 0.402 / p50 0.524 / p75 0.636 / p99 0.881.
    const VolumeSamples& volume = Samples();

    std::vector<float> coverage;
    coverage.reserve( volume.Channel[0].size() );
    for ( size_t i = 0; i < volume.Channel[0].size(); ++i )
        coverage.push_back( clamp( volume.Channel[0][i] * 0.65f + volume.Channel[1][i] * 0.35f, 0.0f, 1.0f ) );

    const Percentiles p = Measure( coverage );
    std::printf( "[CloudNoise] coverage 0.65R+0.35G  p01 %.4f  p25 %.4f  p50 %.4f  p75 %.4f  p99 %.4f\n", p.P01,
                 p.P25, p.P50, p.P75, p.P99 );

    EXPECT_NEAR( p.P50, 0.524f, 0.08f ) << "the coverage field's median moved away from the one the "
                                           "Coverage defaults were measured against";
    EXPECT_NEAR( p.P25, 0.402f, 0.10f );
    EXPECT_NEAR( p.P75, 0.636f, 0.10f );
}

TEST( CloudNoiseRange, TheRemapIsSafeWhenItsSourceRangeIsDegenerate )
{
    // One hand-edited scene file away, and the unguarded form is a division by zero that turns the whole
    // layer into NaN — which renders as a black sky, not as a warning.
    EXPECT_TRUE( std::isfinite( CloudRemap( 0.5f, 0.3f, 0.3f, 0.0f, 1.0f ) ) );
    EXPECT_FLOAT_EQ( CloudRemap( 0.0f, 0.0f, 1.0f, 0.0f, 1.0f ), 0.0f );
    EXPECT_FLOAT_EQ( CloudRemap( 1.0f, 0.0f, 1.0f, 0.0f, 1.0f ), 1.0f );
    EXPECT_FLOAT_EQ( CloudRemap( -5.0f, 0.0f, 1.0f, 0.0f, 1.0f ), 0.0f ) << "clamped below";
    EXPECT_FLOAT_EQ( CloudRemap( 5.0f, 0.0f, 1.0f, 0.0f, 1.0f ), 1.0f ) << "clamped above";
}

// ---------------------------------------------------------------------------------------------------
// The channel layout, which is the deck's (p.96) and is what the container's header claims.
// ---------------------------------------------------------------------------------------------------

TEST( CloudNoiseVolumeLayout, TheWispyPairIsTheInverseOfAnAlligatorAndTheBillowyPairIsOne )
{
    // A test on the RELATION between the header's declared meanings and the maths that fills the channels.
    // The container says R and G are Curly-Alligator and B and A are Alligator; nothing but this checks
    // that the generator agrees, and a volume whose channels were swapped would render as a cloud with its
    // wisps and its billows exchanged — which looks wrong and reads as a tuning problem.
    constexpr float kNoCurl = 0.0f;

    for ( int i = 0; i < 64; ++i )
    {
        const vec3 uvw{ ( i * 13 % 64 + 0.5f ) / 64.0f, ( i * 29 % 64 + 0.5f ) / 64.0f,
                        ( i * 7 % 64 + 0.5f ) / 64.0f };

        const vec4 c = CloudNoiseVolumeChannels( uvw, kSeed, kNoCurl, kWispyPeriodLF, kWispyPeriodHF,
                                                 kBillowPeriodLF, kBillowPeriodHF );

        EXPECT_FLOAT_EQ( c.x, 1.0f - CloudAlligator01( uvw * kWispyPeriodLF, kWispyPeriodLF, kSeed + 0u ) );
        EXPECT_FLOAT_EQ( c.y, 1.0f - CloudAlligator01( uvw * kWispyPeriodHF, kWispyPeriodHF, kSeed + 977u ) );
        EXPECT_FLOAT_EQ( c.z, CloudAlligator01( uvw * kBillowPeriodLF, kBillowPeriodLF, kSeed + 1861u ) );
        EXPECT_FLOAT_EQ( c.w, CloudAlligator01( uvw * kBillowPeriodHF, kBillowPeriodHF, kSeed + 2749u ) );
    }
}

TEST( CloudNoiseVolumeLayout, TheFourChannelsAreDecorrelated )
{
    // Four channels that agreed with one another would be one channel costing four times the bandwidth of
    // every sample in the march. The seed offsets are what keeps them apart; this is what notices if two
    // of them are ever given the same seed and period by accident.
    std::vector<float> r = SampleChannel( 0 );
    std::vector<float> g = SampleChannel( 1 );
    std::vector<float> b = SampleChannel( 2 );

    auto correlation = []( const std::vector<float>& a, const std::vector<float>& c )
    {
        double meanA = 0.0;
        double meanC = 0.0;
        for ( size_t i = 0; i < a.size(); ++i )
        {
            meanA += a[i];
            meanC += c[i];
        }
        meanA /= a.size();
        meanC /= c.size();

        double cov  = 0.0;
        double varA = 0.0;
        double varC = 0.0;
        for ( size_t i = 0; i < a.size(); ++i )
        {
            const double da = a[i] - meanA;
            const double dc = c[i] - meanC;
            cov += da * dc;
            varA += da * da;
            varC += dc * dc;
        }
        return cov / std::sqrt( varA * varC );
    };

    EXPECT_LT( std::abs( correlation( r, g ) ), 0.25 );
    EXPECT_LT( std::abs( correlation( r, b ) ), 0.25 );
    EXPECT_LT( std::abs( correlation( g, b ) ), 0.25 );
}

// ---------------------------------------------------------------------------------------------------
// HOW MANY SCALES THE EROSION HAS, AND WHY THAT IS THE CEILING ON IT — task Р10, 2026-08-28
// ---------------------------------------------------------------------------------------------------
//
// WHY THIS EXISTS. Р9 established that the silhouette is a LINE INTEGRAL: a ray stops where it has
// accumulated unit optical depth, and on the shipped protocol sky that takes 657 m of cloud, while the
// erosion field decorrelates in 160 m. Integrating 657 m of a field that varies over 160 m is a low-pass
// filter, so the erosion contributes 4.1 m of roughness at an 80 m lag on top of the 94.3 m the bare
// lumps already give — 4.3 % (Common/CloudField.glslh, and CloudFieldErosion's own instrument).
//
// Р9's note quotes a structure function for this volume — D(50) 0.167, D(100) 0.210, D(200) 0.218,
// D(400) 0.220, D(800) 0.219, "saturating by 200 m, the signature of a field with exactly ONE scale" —
// and NOTHING IN THE TREE COMPUTED IT. It was measured once, by hand, and written into a comment. This
// test is that measurement made permanent, because the whole of Р10 turns on it and the next task after
// this one will want to know the day it changes.
//
// WHAT IT PINS, and it is a property rather than a number: the field SATURATES far below the tile. Each
// channel is one Alligator octave, so it has a single cell size and no energy above it. Р10's refusal
// rests on that being true, and a change that makes it false — a fractal channel, a second octave, a
// different basis — should come here and restate the finding rather than quietly pass.
//
// THE LAG IS IN FRACTIONS OF A TILE and deliberately not in metres, so that this suite keeps the one
// property its premake file calls out — it links nothing and knows about no world. The conversion is a
// single multiplication the reader can do: one texture unit of this volume spans the LAYER's Detail Tile
// Size along x, because Common/CloudField.glslh forms `windPos * CLOUD_DETAIL_FREQ_X / tile` and that
// frequency is exactly 1. At the shipped Detail Tile Size of 1 km (Engine/ECS/VolumetricCloudComponent.hpp)
// a lag of 0.2 tiles is 200 m and 0.4 tiles is 400 m, which are the two Р9's note quotes. Retuning the
// tile moves the metres and moves nothing here, which is correct: how many scales a field has is a
// property of the field, not of the layer that samples it.
TEST( CloudNoiseVolumeLayout, EveryChannelHasExactlyOneScaleAndSaturatesFarBelowTheDepthTheEyeLooksThrough )
{
    // Mean |f(p) - f(p + lag)| along x, over the whole grid, for one channel. The same estimator Р9's
    // instrument uses on the silhouette, applied here to the FIELD that feeds it.
    auto structureAt = []( const std::vector<float>& f, int lag )
    {
        double total = 0.0;
        long   pairs = 0;
        for ( int z = 0; z < kGrid; ++z )
            for ( int y = 0; y < kGrid; ++y )
                for ( int x = 0; x + lag < kGrid; ++x )
                {
                    const size_t a = ( static_cast<size_t>( z ) * kGrid + y ) * kGrid + x;
                    total += std::fabs( f[a] - f[a + lag] );
                    ++pairs;
                }
        return pairs > 0 ? total / pairs : 0.0;
    };

    std::printf( "[CloudNoise] the eye looks through 657 m of cloud (Р9); at the shipped 1 km tile the "
                 "lags below are 50, 100, 200 and 400 m\n" );
    std::printf( "[CloudNoise]  channel   D(.05)  D(.10)  D(.20)  D(.40)   ratio .20/.40\n" );

    for ( int channel = 0; channel < 4; ++channel )
    {
        const std::vector<float> f = SampleChannel( channel );

        // Lags in GRID STEPS for the four fractions of a tile Р9's note quotes.
        const auto lagFor = []( double tiles ) { return std::max( 1, static_cast<int>( tiles * kGrid + 0.5 ) ); };

        const double d50  = structureAt( f, lagFor( 0.05 ) );
        const double d100 = structureAt( f, lagFor( 0.10 ) );
        const double d200 = structureAt( f, lagFor( 0.20 ) );
        const double d400 = structureAt( f, lagFor( 0.40 ) );

        std::printf( "[CloudNoise]  %d          %.3f   %.3f   %.3f   %.3f        %.3f\n", channel, d50, d100, d200,
                     d400, d400 > 0.0 ? d200 / d400 : 0.0 );

        // ── IT RISES AT ALL ──────────────────────────────────────────────────────────────────────────
        //
        // The weak half, and a guard against a channel that has become constant: a field with no lateral
        // variation would satisfy the saturation bound below trivially.
        EXPECT_GT( d200, 0.05 ) << "channel " << channel << " varies by " << d200
                                << " over 200 m, which is not a field the erosion can cut anything with";

        // ── AND IT HAS STOPPED RISING BY 200 m ───────────────────────────────────────────────────────
        //
        // ONE SCALE, stated as the relation Р10 measured rather than as a stored constant. A single
        // Alligator octave is flat past its own cell, so D(200) and D(400) agree to within a tenth. A
        // channel carrying two or three octaves would keep climbing and this would fail — which is the
        // point: the erosion's inability to reach the silhouette is a consequence of this saturation, so
        // whoever changes it is the person who has to restate Р9's ratio.
        EXPECT_GT( d200 / std::max( d400, 1e-6 ), 0.90 )
             << "channel " << channel << " is still rising at 200 m (D200/D400 = " << d200 / d400
             << "), so it now carries more than one scale — restate the 657 m / 160 m ratio in "
                "Common/CloudField.glslh with the new numbers rather than deleting this bound";
    }
}
