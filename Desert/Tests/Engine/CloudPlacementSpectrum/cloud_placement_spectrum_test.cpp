// CloudPlacementSpectrum — is the sky on a grid, and does the instrument that answers that work?
//
// WHAT IS UNDER TEST, AND IT IS TWO THINGS BECAUSE THE SECOND IS WORTHLESS WITHOUT THE FIRST.
//
//   1. Tools/LatticePeak/Source/LatticePeakMath.hpp — the autocorrelation, the topographic prominence, the
//      lattice score and the jackknife noise. Every number this phase reports was produced by that header,
//      so a fault in it is a fault in every one of them. It is fed fields whose period this file CHOSE.
//
//   2. Engine/Assets/CloudProceduralVolume.cpp's placement, measured with it.
//
// WHY THE PLACEMENT IS MEASURED ON A RASTERISED PROXY AND NOT ONLY ON THE BAKE. A bake is three seconds in
// a Debug build and a lattice measurement needs SEVERAL independent regions, because an autocorrelation
// estimated from sixteen cells across wobbles by about a quarter of its own scale — that is the first thing
// the instrument taught its author and it is written on --repeats. Sixteen bakes is a minute of suite for
// one assertion. So the placement's own output — the lumps, which is where a lattice can be — is rasterised
// into the same top-down quantity the bake produces, and the last test in this file ties the proxy to the
// real thing by measuring one baked volume and demanding the same verdict.
//
// WHAT THE PROXY IS: for each lump, its vertical DIAMETER added over the disc its horizontal radii cover.
// That is the column integral of the lump's bounding cylinder, which is the same quantity — how much cloud
// is above this patch of ground — that Tools/LatticePeak's `--project sum` reads off the baked volume.

#include <LatticePeakMath.hpp>

#include <Engine/Assets/CloudProceduralVolume.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

using namespace Desert::Assets;

namespace
{
    constexpr int kMapSide = 256;

    /// The shipped congestus, written out here rather than read from the asset layer, because this suite
    /// links no asset layer. Only the numbers the PLACEMENT reads matter to it, and
    /// Desert/Tests/Engine/CloudType is what pins the library against meteorology.
    Desert::Graphic::CloudTypeShape Congestus()
    {
        Desert::Graphic::CloudTypeShape shape{};
        shape.BaseAltitudeKm      = 2.20f;
        shape.TopAltitudeKm       = 5.80f;
        shape.EdgeTopFraction     = 0.15f;
        shape.BaseRampFraction    = 0.04f;
        shape.Profile             = Desert::Graphic::CloudProfileFromTaper( 0.50f );
        shape.DetailCharacter     = 1.00f;
        shape.DetailFactor        = 1.00f;
        shape.DensityFactor       = 1.15f;
        shape.ExtinctionFactor    = 1.00f;
        shape.PlacementScale      = 1.00f;
        shape.PlacementAnisotropy = 1.00f;
        return shape;
    }

    /// The shipped layer: a 48 km region, a 3 km cell, the wind along +X so the lattice's axes are the
    /// volume's axes and a per-axis prediction is meaningful.
    CloudProceduralFieldParams ShippedParams()
    {
        CloudProceduralFieldParams params;
        params.RegionSizeKm      = 48.0f;
        params.LayerBottomKm     = 2.20f;
        params.LayerThicknessKm  = 3.60f;
        params.BlendRadiusKm     = 0.06f;
        params.ProfileDepthKm    = 0.36f;
        params.Coverage          = 0.60f;
        params.CoverageContrast  = 1.0f;
        params.Seed              = 1u;
        params.WindAxis          = glm::vec2( 1.0f, 0.0f );
        params.ResolvableChordKm = 0.125f;

        CloudProceduralSpecies species;
        species.Shape      = Congestus();
        species.CellKm     = 3.0f;
        species.Anisotropy = 1.0f;
        params.Species.push_back( species );

        return params;
    }

    /// A painting whose answer this file chose: the left half of the table white in every channel, the
    /// right half black, so its mean is exactly 0.5 per channel and its shape is one nobody can mistake for
    /// noise. A stripe rather than a gradient because the tests below have to be able to say WHERE the
    /// painting says cloud is, and a stripe has a side.
    ///
    /// Built through the canvas the panel and the tool both fill, so that what the tests measure is the
    /// layout a FILE would produce — means and content hash included. A fixture assembled around the
    /// encoder is a fixture that can disagree with every layout an artist ever saves.
    std::shared_ptr<const CloudLayoutData> StripeLayout( uint32_t resolution = 64u, bool withMask = false )
    {
        std::vector<unsigned char> pixels( static_cast<size_t>( resolution ) * resolution * 4u, 0u );

        for ( uint32_t y = 0; y < resolution; ++y )
            for ( uint32_t x = 0; x < resolution; ++x )
            {
                const bool   lit = x < resolution / 2u;
                const size_t at  = ( static_cast<size_t>( y ) * resolution + x ) * 4u;
                pixels[at + 0]   = lit ? 255u : 0u;
                pixels[at + 1]   = lit ? 255u : 0u;
                pixels[at + 2]   = lit ? 255u : 0u;
                pixels[at + 3]   = lit ? 255u : 0u;
            }

        const uint32_t channels[kCloudLayoutChannels] = { 0u, 1u, 2u, 3u };

        CloudLayoutCanvas canvas;
        if ( !SetCloudLayoutCanvasPatternFromImage( canvas, pixels, resolution, resolution, channels ) )
            return nullptr;

        // THE MASK IS ITS OWN PICTURE SINCE O-4, and only exists when asked for. The stripe's own bytes
        // are used for it so the figure and the mask agree about WHERE, which is what the mask tests
        // below rely on; a layout with no mask carries no mask table at all rather than a neutral one.
        if ( withMask && !SetCloudLayoutCanvasMaskFromImage( canvas, pixels, resolution, resolution, 0u ) )
            return nullptr;

        auto made = MakeCloudLayoutFromCanvas( canvas );
        if ( !made )
            return nullptr;

        return std::make_shared<const CloudLayoutData>( made.ExtractValue() );
    }

    /// What fraction of the region's columns carry any cloud at all, measured on the REAL bake. The same
    /// quantity TheCoverageSliderStillMeansTheSkyAtTheShippedPlacement measures, lifted out of it so the
    /// zero-mean relation can measure the same thing rather than something like it.
    double SkyCover( const CloudProceduralFieldParams& params )
    {
        const glm::vec2 origin = CloudProceduralRegionOriginKm( params, 0.0f, 0.0f );
        const auto      baked  = BakeCloudProceduralVolume( params, origin );
        if ( !baked )
            return -1.0;

        size_t columns = 0;
        for ( uint32_t z = 0; z < kCloudProceduralVolumeSide; ++z )
            for ( uint32_t x = 0; x < kCloudProceduralVolumeSide; ++x )
                for ( uint32_t y = 0; y < kCloudProceduralVolumeHeight; ++y )
                {
                    const size_t at = ( ( static_cast<size_t>( z ) * kCloudProceduralVolumeHeight + y ) *
                                             kCloudProceduralVolumeSide +
                                        x ) *
                                      kCloudProceduralBytesPerVoxel;
                    if ( baked.GetValue()[at] != 0u )
                    {
                        ++columns;
                        break;
                    }
                }

        return static_cast<double>( columns ) /
               static_cast<double>( kCloudProceduralVolumeSide * kCloudProceduralVolumeSide );
    }

    /// The lumps of one region, rasterised into the top-down column integral described in the file note.
    /// Periodic, because the region is: a lump against a face contributes to the opposite one, which is
    /// exactly what the bake's wrap does and what makes a circular autocorrelation the right estimator.
    std::vector<float> RasteriseColumns( const std::vector<CloudModellingBlob>& blobs, const glm::vec2& originKm,
                                         float regionKm )
    {
        std::vector<float> map( static_cast<size_t>( kMapSide ) * kMapSide, 0.0f );

        const float perVoxelKm = regionKm / static_cast<float>( kMapSide );

        for ( const CloudModellingBlob& blob : blobs )
        {
            const float weight = 2.0f * blob.RadiiKm.y;

            const float centreX = ( blob.CentreKm.x - originKm.x ) / perVoxelKm;
            const float centreZ = ( blob.CentreKm.z - originKm.y ) / perVoxelKm;

            // AN ELLIPSE IN THE LUMP'S OWN FRAME, AND A TEST FOUND OUT WHY IT HAS TO BE. This painted a
            // DISC of the lump's longer horizontal radius, which is exact only while the two are equal —
            // and §SIL made them unequal, because a lump in an anisotropic lattice is drawn out along the
            // wind. At an anisotropy of 8 the disc was eight times too wide across the wind and the proxy
            // reported the sky 0.98 covered where the bake reports 0.52. A proxy that reads the placement
            // wrong is not a weaker measurement, it is a different one.
            const float along   = blob.RadiiKm.x / perVoxelKm;
            const float acrossR = blob.RadiiKm.z / perVoxelKm;

            const float yaw     = blob.RotationDeg.y * 0.017453292519943295f;
            const float cosYaw  = std::cos( yaw );
            const float sinYaw  = std::sin( yaw );
            const float boundKm = std::max( blob.RadiiKm.x, blob.RadiiKm.z );
            const float bound   = boundKm / perVoxelKm;

            const int first = static_cast<int>( std::floor( -bound ) );
            const int last  = static_cast<int>( std::ceil( bound ) );

            for ( int dz = first; dz <= last; ++dz )
                for ( int dx = first; dx <= last; ++dx )
                {
                    // World -> local is the transpose of local -> world, and `glm::quat( radians( 0, yaw,
                    // 0 ) )` carries local +X to `( cos yaw, 0, -sin yaw )`.
                    const float localX = static_cast<float>( dx ) * cosYaw - static_cast<float>( dz ) * sinYaw;
                    const float localZ = static_cast<float>( dx ) * sinYaw + static_cast<float>( dz ) * cosYaw;

                    const float inside = ( localX * localX ) / std::max( along * along, 1e-9f ) +
                                         ( localZ * localZ ) / std::max( acrossR * acrossR, 1e-9f );
                    if ( inside > 1.0f )
                        continue;

                    int x = static_cast<int>( std::floor( centreX ) ) + dx;
                    int z = static_cast<int>( std::floor( centreZ ) ) + dz;

                    x = ( ( x % kMapSide ) + kMapSide ) % kMapSide;
                    z = ( ( z % kMapSide ) + kMapSide ) % kMapSide;

                    map[static_cast<size_t>( z ) * kMapSide + x] += weight;
                }
        }

        return map;
    }

    /// The lattice score of a placement, averaged over @p regions independent regions, with the noise the
    /// estimator makes on its own. Both axes are measured and the LARGER is returned, because a lattice
    /// that shows on one axis only is still a lattice — that is what "the clouds go in a row" describes.
    struct LatticeVerdict
    {
        double Score = 0.0;
        double Noise = 0.0;
        double LagKm = 0.0;
    };

    LatticeVerdict MeasurePlacement( const CloudProceduralFieldParams& params, int regions )
    {
        const float perVoxelKm = params.RegionSizeKm / static_cast<float>( kMapSide );

        const glm::vec2 extent  = CloudProceduralCellExtentKm( params, params.Species[0] );
        const int       periodX = static_cast<int>( extent.x / perVoxelKm + 0.5f );
        const int       periodZ = static_cast<int>( extent.y / perVoxelKm + 0.5f );

        const int maxLag = kMapSide / 2;

        std::vector<std::vector<double>> curvesX;
        std::vector<std::vector<double>> curvesZ;

        for ( int region = 0; region < regions; ++region )
        {
            // Disjoint cell sets: each region is four whole periods further along, so no cell is measured
            // twice and the curves being averaged are independent.
            const float     cameraKm = static_cast<float>( region ) * params.RegionSizeKm * 4.0f;
            const glm::vec2 origin   = CloudProceduralRegionOriginKm( params, cameraKm, cameraKm );

            const std::vector<float> map = RasteriseColumns( GenerateCloudProceduralBlobs( params, 0u, origin ),
                                                             origin, params.RegionSizeKm );

            curvesX.push_back( LatticePeak::CircularAutocorrelation( map, kMapSide, kMapSide, true, maxLag ) );
            curvesZ.push_back( LatticePeak::CircularAutocorrelation( map, kMapSide, kMapSide, false, maxLag ) );
        }

        const std::vector<double> rx = LatticePeak::Average( curvesX );
        const std::vector<double> rz = LatticePeak::Average( curvesZ );

        const LatticePeak::Peak peakX = LatticePeak::LatticeScore( rx, periodX, 4 );
        const LatticePeak::Peak peakZ = LatticePeak::LatticeScore( rz, periodZ, 4 );

        const double noiseX = LatticePeak::JackknifeNoise( curvesX, LatticePeak::FirstLagAfterCentralLobe( rx ) );
        const double noiseZ = LatticePeak::JackknifeNoise( curvesZ, LatticePeak::FirstLagAfterCentralLobe( rz ) );

        LatticeVerdict verdict;
        if ( peakX.Prominence >= peakZ.Prominence )
        {
            verdict.Score = peakX.Prominence;
            verdict.LagKm = peakX.Lag * perVoxelKm;
        }
        else
        {
            verdict.Score = peakZ.Prominence;
            verdict.LagKm = peakZ.Lag * perVoxelKm;
        }
        verdict.Noise = std::max( noiseX, noiseZ );
        return verdict;
    }

    /// What fraction of the sky has cloud in the column, on the rasterised proxy.
    double CoverOf( const std::vector<float>& map )
    {
        size_t touched = 0;
        for ( float value : map )
            touched += ( value > 0.0f ) ? 1u : 0u;
        return static_cast<double>( touched ) / static_cast<double>( map.size() );
    }

    double CoverOfPlacement( const CloudProceduralFieldParams& params )
    {
        const glm::vec2 origin = CloudProceduralRegionOriginKm( params, 0.0f, 0.0f );
        return CoverOf(
             RasteriseColumns( GenerateCloudProceduralBlobs( params, 0u, origin ), origin, params.RegionSizeKm ) );
    }
} // namespace

// ===================================================================================================
// PART ONE — THE INSTRUMENT, ON FIELDS WHOSE ANSWER THIS FILE ALREADY KNOWS
// ===================================================================================================

// A curve that only ever falls has no local maximum, so it has no prominence anywhere. This is the whole
// reason prominence and not correlation is the reported quantity: a broad body correlates strongly with
// itself at small lags whether or not there is a lattice, and reporting THAT would call every sky a grid.
TEST( CloudPlacementSpectrum, ADecayingCurveHasNoPeakAtAll )
{
    std::vector<double> decaying( 128 );
    for ( size_t k = 0; k < decaying.size(); ++k )
        decaying[k] = std::exp( -static_cast<double>( k ) / 12.0 );

    const LatticePeak::Peak peak = LatticePeak::StrongestPeak( decaying );

    EXPECT_FALSE( peak.Found ) << "a curve that only falls reported a bump at lag " << peak.Lag
                               << " of prominence " << peak.Prominence
                               << ", so the instrument would find a lattice in a sky that has none";
}

// And the mirror of it: a curve that comes back must be found, at the lag it comes back at, with a
// prominence equal to its height above the trough it climbed out of.
TEST( CloudPlacementSpectrum, ABumpIsFoundWhereItWasPutAndMeasuredAgainstItsOwnTrough )
{
    std::vector<double> curve( 128 );
    for ( size_t k = 0; k < curve.size(); ++k )
        curve[k] = std::exp( -static_cast<double>( k ) / 12.0 );

    // A bump of 0.20 riding on the decay at lag 40, three samples wide.
    curve[39] += 0.10;
    curve[40] += 0.20;
    curve[41] += 0.10;

    const LatticePeak::Peak peak = LatticePeak::StrongestPeak( curve );

    ASSERT_TRUE( peak.Found );
    EXPECT_EQ( peak.Lag, 40 ) << "the bump was put at 40 and found at " << peak.Lag;

    // The trough it climbs out of is the decay's own value just before the bump, which at lag 38 is
    // exp(-38/12) = 0.0421; the bump's top is exp(-40/12) + 0.20 = 0.2358.
    EXPECT_NEAR( peak.Prominence, 0.2358 - 0.0421, 0.005 )
         << "the prominence is not the height above the bracketing trough";
}

// A LATTICE OF POINTS PUTS A BUMP ON EVERY MULTIPLE OF ITS PERIOD, and this is the property the whole
// phase is measured by. The field here is built by this file, so the answer is not in question — what is
// being checked is that LatticeScore finds it and reports which multiple it stands on.
TEST( CloudPlacementSpectrum, APerfectLatticeIsFoundAtItsOwnPeriodAndAtItsHarmonics )
{
    constexpr int period = 16;

    std::vector<float> map( static_cast<size_t>( kMapSide ) * kMapSide, 0.0f );
    for ( int z = 0; z < kMapSide; z += period )
        for ( int x = 0; x < kMapSide; x += period )
            for ( int dz = -2; dz <= 2; ++dz )
                for ( int dx = -2; dx <= 2; ++dx )
                {
                    const int px                                   = ( x + dx + kMapSide ) % kMapSide;
                    const int pz                                   = ( z + dz + kMapSide ) % kMapSide;
                    map[static_cast<size_t>( pz ) * kMapSide + px] = 1.0f;
                }

    const std::vector<double> r =
         LatticePeak::CircularAutocorrelation( map, kMapSide, kMapSide, true, kMapSide / 2 );

    const LatticePeak::Peak score = LatticePeak::LatticeScore( r, period, 4 );

    ASSERT_TRUE( score.Found ) << "a perfect lattice of period " << period
                               << " produced no bump on any multiple of it";
    EXPECT_EQ( score.Lag % period, 0 ) << "the bump found is at lag " << score.Lag
                                       << ", which is not a multiple of the period it was built with";
    EXPECT_GT( score.Prominence, 0.5 ) << "a perfect lattice's bump is faint, so the estimator is wrong";

    std::printf( "[CloudPlacementSpectrum] a perfect lattice of period %d: bump at %d, prominence %.4f\n", period,
                 score.Lag, score.Prominence );
}

// AND A FIELD PLACED WITHOUT A LATTICE MUST NOT PRODUCE ONE. Same number of bodies, same size, placed by a
// hash over the whole map instead of on a grid — if this reported a lattice, every "the lattice is gone"
// in this phase would be unfalsifiable.
TEST( CloudPlacementSpectrum, IndependentlyPlacedBodiesProduceNoLatticeAtAnyPeriod )
{
    constexpr int period = 16;

    std::vector<float> map( static_cast<size_t>( kMapSide ) * kMapSide, 0.0f );

    uint32_t   state = 12345u;
    const auto next  = [&state]()
    {
        state = state * 1664525u + 1013904223u;
        return static_cast<int>( ( state >> 8 ) % kMapSide );
    };

    for ( int body = 0; body < ( kMapSide / period ) * ( kMapSide / period ); ++body )
    {
        const int x = next();
        const int z = next();
        for ( int dz = -2; dz <= 2; ++dz )
            for ( int dx = -2; dx <= 2; ++dx )
            {
                const int px                                   = ( x + dx + kMapSide ) % kMapSide;
                const int pz                                   = ( z + dz + kMapSide ) % kMapSide;
                map[static_cast<size_t>( pz ) * kMapSide + px] = 1.0f;
            }
    }

    const std::vector<double> r =
         LatticePeak::CircularAutocorrelation( map, kMapSide, kMapSide, true, kMapSide / 2 );

    const LatticePeak::Peak score = LatticePeak::LatticeScore( r, period, 4 );

    std::printf( "[CloudPlacementSpectrum] the same bodies placed independently: prominence %.4f\n",
                 score.Prominence );

    EXPECT_LT( score.Prominence, 0.05 )
         << "bodies placed by a hash over the whole map reported a lattice of prominence " << score.Prominence
         << " at lag " << score.Lag;
}

// The jackknife noise is what separates a real bump from the estimator's own wobble, so it has to be zero
// when the realisations agree and positive when they do not. Without that property a prominence would be
// reported against a background of nothing.
TEST( CloudPlacementSpectrum, TheNoiseEstimateIsZeroWhenTheRealisationsAgreeAndPositiveWhenTheyDoNot )
{
    std::vector<double> curve( 64 );
    for ( size_t k = 0; k < curve.size(); ++k )
        curve[k] = std::exp( -static_cast<double>( k ) / 8.0 );

    const std::vector<std::vector<double>> identical{ curve, curve, curve, curve };
    EXPECT_NEAR( LatticePeak::JackknifeNoise( identical, 1 ), 0.0, 1e-12 )
         << "four identical realisations disagree about something";

    std::vector<std::vector<double>> disagreeing = identical;
    for ( size_t k = 1; k < curve.size(); ++k )
    {
        disagreeing[0][k] += 0.02;
        disagreeing[2][k] += 0.02;
        disagreeing[1][k] -= 0.02;
        disagreeing[3][k] -= 0.02;
    }

    // The even half is high by 0.02 and the odd half low by 0.02, so half their difference is 0.02.
    EXPECT_NEAR( LatticePeak::JackknifeNoise( disagreeing, 1 ), 0.02, 1e-6 );
}

// Otsu's threshold is what makes the frame mode's cloud/sky split a property of the picture rather than of
// whoever ran the tool. On two well separated populations it has to land between them.
TEST( CloudPlacementSpectrum, OtsuSplitsTwoSeparatedPopulationsBetweenThem )
{
    // 0.10 AND 0.90 RATHER THAN 0.20 AND 0.80, and the reason is a real edge of the routine rather than a
    // taste. The threshold is a histogram BIN, so it is a multiple of 1/255; a population sitting exactly
    // on such a multiple lands on the wrong side of a `>` comparison by one part in ten million of float
    // rounding. 0.20 is exactly 51/255 and did precisely that. Values that are not bin boundaries are what
    // a frame's luminances are, so this is the representative case as well as the safe one.
    std::vector<float> values;
    values.insert( values.end(), 4000, 0.10f );
    values.insert( values.end(), 6000, 0.90f );

    const double threshold = LatticePeak::OtsuThreshold( values );

    // THE THRESHOLD IS THE TOP OF THE BACKGROUND CLASS, not a point strictly between the two — Otsu's
    // split is taken at a histogram BIN, and the bin it names is the last one that belongs below. What has
    // to be true is that comparing with `>` separates the populations, which is how both the tool and this
    // assertion use it.
    size_t below = 0;
    size_t above = 0;
    for ( float value : values )
        ( value > threshold ? above : below )++;

    EXPECT_EQ( below, 4000u ) << "the dim population did not all land below the split at " << threshold;
    EXPECT_EQ( above, 6000u ) << "the bright population did not all land above the split at " << threshold;

    EXPECT_GE( threshold, 0.10 );
    EXPECT_LT( threshold, 0.90 );
}

// ===================================================================================================
// PART TWO — THE PLACEMENT, MEASURED WITH THE INSTRUMENT ABOVE
// ===================================================================================================

// THE HEADLINE OF THE PHASE, AND IT IS A RELATION RATHER THAN A NUMBER: the sky that ships must show no
// lattice, and the sky with the scatter returned to zero must show one. Asserting only the first would go
// green on a placement that produced no cloud at all; asserting both means the measurement is alive.
//
// WHY THE SCATTER AND NOT THE DENSITY IS THE ARM THAT BRINGS IT BACK — measured, and it is the opposite of
// what the shape of the defect suggests. Raising the count of clouds per cell ALONE makes the lattice two
// and a half times stronger, because several small clouds crowded into the middle third of a cell mark
// that cell's site more sharply than one large one did. Docs/Clouds/CALIBRATION.md section RW has the row.
TEST( CloudPlacementSpectrum, TheShippedPlacementHasNoLatticeAndConfiningItAgainBringsOneBack )
{
    constexpr int kRegions = 8;

    const CloudProceduralFieldParams shipped = ShippedParams();

    CloudProceduralFieldParams confined = shipped;
    confined.PlacementScatter           = 0.0f;

    const LatticeVerdict free    = MeasurePlacement( shipped, kRegions );
    const LatticeVerdict lattice = MeasurePlacement( confined, kRegions );

    std::printf( "[CloudPlacementSpectrum] shipped scatter %.2f: lattice %.4f at %.3f km, noise %.4f\n",
                 shipped.PlacementScatter, free.Score, free.LagKm, free.Noise );
    std::printf( "[CloudPlacementSpectrum] scatter returned to 0: lattice %.4f at %.3f km, noise %.4f\n",
                 lattice.Score, lattice.LagKm, lattice.Noise );

    EXPECT_GT( lattice.Score, 0.05 )
         << "a placement confined to its own lattice site produced no measurable lattice, so this test "
            "cannot tell the two apart and its other half proves nothing";

    EXPECT_LT( free.Score, 0.02 ) << "the shipped placement still shows a lattice bump of " << free.Score << " at "
                                  << free.LagKm << " km, which is what this phase removed";

    EXPECT_LT( free.Score * 3.0, lattice.Score )
         << "freeing the placement did not reduce the lattice by even a factor of three";
}

// EVERY ONE OF THE FOUR KNOBS MOVES THE FIELD. The contract's §1.3 forbids a setting that does nothing, and
// a placement parameter is the easiest place in this component to get that wrong, because a sky that is
// already busy looks about the same whatever you do to it. So each is moved on its own and the lumps are
// compared.
TEST( CloudPlacementSpectrum, EveryPlacementKnobChangesTheLumps )
{
    const CloudProceduralFieldParams base   = ShippedParams();
    const glm::vec2                  origin = CloudProceduralRegionOriginKm( base, 0.0f, 0.0f );

    const std::vector<CloudModellingBlob> reference = GenerateCloudProceduralBlobs( base, 0u, origin );
    ASSERT_FALSE( reference.empty() );

    const auto differs = [&]( const CloudProceduralFieldParams& moved, const char* what )
    {
        const std::vector<CloudModellingBlob> other = GenerateCloudProceduralBlobs( moved, 0u, origin );

        if ( other.size() != reference.size() )
            return true;

        for ( size_t k = 0; k < other.size(); ++k )
            if ( other[k].CentreKm != reference[k].CentreKm || other[k].RadiiKm != reference[k].RadiiKm )
                return true;

        ADD_FAILURE() << what << " produced the identical field, so it is a setting that moves nothing";
        return false;
    };

    CloudProceduralFieldParams density = base;
    density.PlacementDensity           = 1.0f;
    EXPECT_TRUE( differs( density, "Cloud Density" ) );

    CloudProceduralFieldParams scatter = base;
    scatter.PlacementScatter           = 0.0f;
    EXPECT_TRUE( differs( scatter, "Cloud Scatter" ) );

    CloudProceduralFieldParams variety = base;
    variety.PlacementSizeVariety       = 0.0f;
    EXPECT_TRUE( differs( variety, "Cloud Size Variety" ) );

    CloudProceduralFieldParams strength = base;
    strength.PatchStrength              = 0.0f;
    EXPECT_TRUE( differs( strength, "Weather Patch Strength" ) );

    CloudProceduralFieldParams tile = base;
    tile.PatchTileKm                = 40.0f;
    EXPECT_TRUE( differs( tile, "Weather Patch Size" ) );
}

// THE DENSITY REDISTRIBUTES MATTER RATHER THAN ADDING IT, and that is a property by construction rather
// than a coincidence: a cell carrying `d` clusters gives each of them one over the square root of `d` of
// the width, so the ground they cover between them is the ground one covered. It is asserted because it is
// what lets the Coverage slider keep its meaning at every setting of the density — without it, decision
// D-20's mapping would have to be re-measured for each one, which is a knob that invalidates another knob.
TEST( CloudPlacementSpectrum, TheDensityDoesNotMoveTheSkysCover )
{
    CloudProceduralFieldParams params = ShippedParams();

    params.PlacementDensity = 1.0f;
    const double one        = CoverOfPlacement( params );

    params.PlacementDensity = 4.0f;
    const double four       = CoverOfPlacement( params );

    std::printf( "[CloudPlacementSpectrum] cover at density 1 / 4: %.4f / %.4f\n", one, four );

    EXPECT_NEAR( one, four, 0.05 ) << "quadrupling the number of clouds per cell moved the sky's cover by "
                                   << std::abs( one - four )
                                   << ", so the density knob and the coverage slider fight each other";
}

// AND SO DOES THE SIZE SPREAD, for the same reason stated differently: the draw is uniform in AREA and not
// in width, so its mean is one whatever the spread. A spread that was uniform in the RADIUS would have
// raised the mean area by a twelfth of the spread squared and taken the coverage mapping with it.
TEST( CloudPlacementSpectrum, TheSizeVarietyDoesNotMoveTheSkysCover )
{
    CloudProceduralFieldParams params = ShippedParams();

    params.PlacementSizeVariety = 0.0f;
    const double none           = CoverOfPlacement( params );

    params.PlacementSizeVariety = 1.0f;
    const double full           = CoverOfPlacement( params );

    std::printf( "[CloudPlacementSpectrum] cover at size variety 0 / 1: %.4f / %.4f\n", none, full );

    EXPECT_NEAR( none, full, 0.05 ) << "spreading the cloud sizes moved the sky's cover by "
                                    << std::abs( none - full );
}

// THE SLIDER STILL MEANS THE SKY AT THE PLACEMENT THAT SHIPS, and this test exists because a sabotage
// found the hole it fills. `CloudProceduralField` already measures the Coverage slider against the sky and
// allows it a TENTH — a bound set for a placement that kept every cluster near its own lattice site, on a
// fixture whose cell is finer than the shipped one. Deleting the packing compensation entirely left that
// suite GREEN: its worst deviation went from 0.033 to 0.050 and a tenth swallowed both.
//
// The compensation is what pays for the free placement's worse packing, so it has to be asserted where it
// bites — at the shipped 3 km cell, near the top of the slider, on the REAL bake. With the compensation the
// sky delivered at 0.75 is 0.741; without it, 0.703.
TEST( CloudPlacementSpectrum, TheCoverageSliderStillMeansTheSkyAtTheShippedPlacement )
{
    double worst = 0.0;

    for ( const float wanted : { 0.35f, 0.75f } )
    {
        CloudProceduralFieldParams params = ShippedParams();
        params.Coverage                   = wanted;

        const glm::vec2 origin = CloudProceduralRegionOriginKm( params, 0.0f, 0.0f );
        const auto      baked  = BakeCloudProceduralVolume( params, origin );
        ASSERT_TRUE( baked ) << ( baked ? std::string{} : baked.GetError() );

        size_t columns = 0;
        for ( uint32_t z = 0; z < kCloudProceduralVolumeSide; ++z )
            for ( uint32_t x = 0; x < kCloudProceduralVolumeSide; ++x )
                for ( uint32_t y = 0; y < kCloudProceduralVolumeHeight; ++y )
                {
                    const size_t at = ( ( static_cast<size_t>( z ) * kCloudProceduralVolumeHeight + y ) *
                                             kCloudProceduralVolumeSide +
                                        x ) *
                                      kCloudProceduralBytesPerVoxel;
                    if ( baked.GetValue()[at] != 0u )
                    {
                        ++columns;
                        break;
                    }
                }

        const double measured = static_cast<double>( columns ) /
                                static_cast<double>( kCloudProceduralVolumeSide * kCloudProceduralVolumeSide );

        std::printf( "[CloudPlacementSpectrum] coverage %.2f -> %.3f of the sky (%+.3f)\n", wanted, measured,
                     measured - wanted );

        worst = std::max( worst, std::abs( measured - static_cast<double>( wanted ) ) );
    }

    // A FORTIETH, AND THE NUMBER IS THE GAP RATHER THAN AN AMBITION. With the compensation the worst of the
    // two settings is out by 0.009; with it deleted, by 0.047. Anything between the two separates them, and
    // 0.025 sits in the middle with a factor of two of headroom on each side.
    EXPECT_LT( worst, 0.025 ) << "the Coverage slider is out by " << worst
                              << " of the sky at the shipped placement, which is where the packing the free "
                                 "placement costs has to be paid for";
}

// A PATCH FINER THAN THREE CELLS IS A CHECKERBOARD, NOT A WEATHER SYSTEM, and it is refused by name rather
// than drawn. This is the one relation of the four knobs that has a hard bound on it: the modulation exists
// to decide REGIONS of sky, and one whose period is near a cell's decides cells one at a time.
TEST( CloudPlacementSpectrum, APatchFinerThanThreeCellsIsRefusedAndACoarseOneIsAccepted )
{
    CloudProceduralFieldParams params = ShippedParams();
    params.PatchStrength              = 0.6f;

    const glm::vec2 extent = CloudProceduralCellExtentKm( params, params.Species[0] );
    const float     cellKm = std::max( extent.x, extent.y );

    params.PatchTileKm = 2.9f * cellKm;
    EXPECT_FALSE( ValidateCloudProceduralParams( params ) )
         << "a patch of " << params.PatchTileKm << " km against a cell of " << cellKm
         << " km was accepted, so the modulation may be made as fine as the lattice it modulates";

    params.PatchTileKm = 3.1f * cellKm;
    EXPECT_TRUE( ValidateCloudProceduralParams( params ) );

    // AND IT IS NOT CHECKED WHEN IT IS OFF, because a number nothing reads must not refuse a layer.
    params.PatchStrength = 0.0f;
    params.PatchTileKm   = 0.5f * cellKm;
    EXPECT_TRUE( ValidateCloudProceduralParams( params ) )
         << "a patch size was refused on a layer whose patch modulation is switched off";
}

// THE ANVIL IS PART OF ITS CLUSTER AND HAS TO SHRINK WITH IT, and this test exists because a sabotage found
// that nothing said so. The anvil is a second, wider, flatter lump above the tower, and it is the only lump
// in the generator whose width is written a second time rather than derived from the cluster's radius —
// which is exactly the two-places-that-must-agree shape §2.3.1 of the contract is about. Deleting the
// cluster's own size, density and packing factors from the anvil's line left every suite in the repository
// green: only the cumulonimbus has an anvil at all, and no suite baked one procedurally and looked at it.
//
// What is asserted is the RELATION rather than either number: whatever the density does to the tower it must
// do to the anvil, so the ratio between them cannot depend on the density.
TEST( CloudPlacementSpectrum, TheAnvilShrinksWithTheClusterItCaps )
{
    const auto anvilToTowerAt = [&]( float density )
    {
        CloudProceduralFieldParams params = ShippedParams();
        params.PlacementDensity           = density;

        // The shipped cumulonimbus, which is the only type in the library with an anvil.
        Desert::Graphic::CloudTypeShape& shape = params.Species[0].Shape;
        shape.BaseAltitudeKm                   = 0.9f;
        shape.TopAltitudeKm                    = 9.0f;
        shape.EdgeTopFraction                  = 0.12f;
        shape.Profile                          = Desert::Graphic::CloudProfileFromTaper( 0.4f );
        shape.AnvilAltitudeKm                  = 9.5f;
        shape.AnvilThicknessKm                 = 1.8f;
        shape.AnvilStrength                    = 0.85f;

        params.LayerBottomKm     = 0.9f;
        params.LayerThicknessKm  = 10.4f;
        params.Species[0].CellKm = 6.0f;

        const glm::vec2                       origin = CloudProceduralRegionOriginKm( params, 0.0f, 0.0f );
        const std::vector<CloudModellingBlob> blobs  = GenerateCloudProceduralBlobs( params, 0u, origin );

        double anvil  = 0.0;
        double anvils = 0.0;
        double tower  = 0.0;
        double towers = 0.0;

        for ( const CloudModellingBlob& blob : blobs )
        {
            // The anvil is the lump AT the anvil altitude; every other lump of a cluster sits inside the
            // band between the type's base and top, which ends half a kilometre below it.
            if ( std::abs( blob.CentreKm.y - shape.AnvilAltitudeKm ) < 1e-3f )
            {
                anvil += blob.RadiiKm.x;
                anvils += 1.0;
            }
            else
            {
                tower += blob.RadiiKm.x;
                towers += 1.0;
            }
        }

        EXPECT_GT( anvils, 0.0 ) << "no anvil was generated at all, so this test measured nothing";
        EXPECT_GT( towers, 0.0 );

        return ( anvil / std::max( anvils, 1.0 ) ) / ( tower / std::max( towers, 1.0 ) );
    };

    const double one  = anvilToTowerAt( 1.0f );
    const double four = anvilToTowerAt( 4.0f );

    std::printf( "[CloudPlacementSpectrum] anvil/tower width at density 1 / 4: %.4f / %.4f\n", one, four );

    EXPECT_NEAR( one, four, 0.05 * one )
         << "the anvil and the tower it caps scale differently with the density, so at one setting the "
            "anvil is a lid the storm has outgrown and at another it is a saucer floating over a stump";
}

// AND THE ANVIL IS DRAWN OUT WITH THE CLUSTER IT CAPS, which is the SAME hole in the same line found a
// second time from a new direction.
//
// §RW's sabotage run found that deleting the cluster's size, density and packing factors from the anvil's
// width left the whole repository green, "because only the cumulonimbus has an anvil at all and no suite
// baked one procedurally and looked at it". §RW closed it with the anvil-over-tower ratio above — which is
// blind to ANISOTROPY, because the shipped cumulonimbus' is 1 and the ratio is taken on one axis. §SIL's
// own sabotage run rediscovered the line: deleting the anvil's `stretch` was GREEN across the repository.
//
// A storm whose tower is a downwind band under a circular lid is two bodies, and it is exactly the shape
// nobody would notice in a number until they rendered one.
TEST( CloudPlacementSpectrum, TheAnvilIsDrawnOutDownwindWithTheTowerItCaps )
{
    CloudProceduralFieldParams params = ShippedParams();

    Desert::Graphic::CloudTypeShape& shape = params.Species[0].Shape;
    shape.BaseAltitudeKm                   = 0.9f;
    shape.TopAltitudeKm                    = 9.0f;
    shape.EdgeTopFraction                  = 0.12f;
    shape.Profile                          = Desert::Graphic::CloudProfileFromTaper( 0.4f );
    shape.AnvilAltitudeKm                  = 9.5f;
    shape.AnvilThicknessKm                 = 1.8f;
    shape.AnvilStrength                    = 0.85f;

    params.LayerBottomKm     = 0.9f;
    params.LayerThicknessKm  = 10.4f;
    params.Species[0].CellKm = 6.0f;

    // A STRETCHED LATTICE, which is the case the ratio test above cannot see. Anything the cell does to
    // the tower it must do to the canopy.
    params.Species[0].Anisotropy = 4.0f;

    const glm::vec2                       origin = CloudProceduralRegionOriginKm( params, 0.0f, 0.0f );
    const std::vector<CloudModellingBlob> blobs  = GenerateCloudProceduralBlobs( params, 0u, origin );

    double anvilStretch = 0.0;
    double anvils       = 0.0;
    double towerStretch = 0.0;
    double towers       = 0.0;

    for ( const CloudModellingBlob& blob : blobs )
    {
        const double stretch = static_cast<double>( blob.RadiiKm.x ) / std::max( blob.RadiiKm.z, 1e-6f );

        if ( std::abs( blob.CentreKm.y - shape.AnvilAltitudeKm ) < 1e-3f )
        {
            anvilStretch += stretch;
            anvils += 1.0;
        }
        else
        {
            towerStretch += stretch;
            towers += 1.0;
        }
    }

    ASSERT_GT( anvils, 0.0 ) << "no anvil was generated at all, so this test measured nothing";
    ASSERT_GT( towers, 0.0 );

    const double anvil = anvilStretch / anvils;
    const double tower = towerStretch / towers;

    std::printf( "[CloudPlacementSpectrum] at anisotropy 4 the tower's lumps are %.2fx long and the anvil "
                 "is %.2fx\n",
                 tower, anvil );

    // A FIFTH, AND THE GAP IT ALLOWS IS ACCOUNTED FOR RATHER THAN PADDING. The anvil's across-wind radius
    // carries a 0.9 of its own — a canopy is slightly oval in plan and always was — so its measured stretch
    // is the cell's over that 0.9: 4.44 against the tower's 4.01. The tower's lumps additionally carry an
    // independent wobble on each horizontal axis, which is why its MEAN is what is compared. A fifth
    // accommodates both and is far inside the factor of four the sabotage produced.
    EXPECT_NEAR( anvil, tower, 0.20 * tower )
         << "the anvil is not drawn out downwind by the same factor as the tower under it, so a storm in a "
            "stretched lattice is a band with a circular lid on it — two bodies rather than one";
}

// ---------------------------------------------------------------------------------------------------
// THE CANOPY AND THE COVERAGE SLIDER, which is CALIBRATION.md §CB
// ---------------------------------------------------------------------------------------------------
//
// THE DEFECT THESE THREE WERE WRITTEN AFTER. The `Coverage` slider is documented as the fraction of the
// SKY that has cloud in the column, and every genus in the library delivered it to within 0.06 except one:
// the cumulonimbus returned 0.856 for a setting of 0.5, unchanged through two phases that each named a
// cause and neither tested it. The cause is the ANVIL — a canopy 1.70 times the tower's own footprint
// covers 2.9 times the sky and nothing priced it — and it hid for exactly the reason §RW's own sabotage run
// named about a different line of the same block: only one type in the library HAS an anvil, so a relation
// asserted on the shipped congestus cannot see it.
//
// Two of these three assert the sky and one asserts the lumps, deliberately: the sky is what the slider
// promises and the lumps are what the promise is made of, and a phase that moved one without the other is
// exactly what produced the defect.

namespace
{
    /// The shipped cumulonimbus' placement numbers, on a layer whose shell is the RENDERER's — the union of
    /// the tower's band and the canopy above it, `Graphic::CloudTypeSetEnvelopeKm`, and not `Top - Base`.
    CloudProceduralFieldParams StormParams()
    {
        CloudProceduralFieldParams params = ShippedParams();

        Desert::Graphic::CloudTypeShape& shape = params.Species[0].Shape;
        shape.BaseAltitudeKm                   = 0.9f;
        shape.TopAltitudeKm                    = 9.0f;
        shape.EdgeTopFraction                  = 0.12f;
        shape.BaseRampFraction                 = 0.04f;
        shape.Profile                          = Desert::Graphic::CloudProfileFromTaper( 0.4f );
        shape.AnvilAltitudeKm                  = 9.5f;
        shape.AnvilThicknessKm                 = 1.8f;
        shape.AnvilStrength                    = 0.85f;

        params.LayerBottomKm     = 0.9f;
        params.LayerThicknessKm  = 10.4f;
        params.Species[0].CellKm = 6.0f;

        return params;
    }
} // namespace

TEST( CloudPlacementSpectrum, ACanopyCostsTheSkyNothingBecauseTheClusterPaysForIt )
{
    // THE LAYER IS THE SAME IN BOTH ARMS and it is the one the storm WITH its canopy needs, so the only
    // thing that differs between them is whether the canopy is there. Giving each arm its own shell would
    // also change how finely 32 voxels sample the band, and the difference measured would be two things.
    CloudProceduralFieldParams capped   = StormParams();
    CloudProceduralFieldParams bare     = StormParams();
    bare.Species[0].Shape.AnvilStrength = 0.0f;

    for ( const float coverage : { 0.35f, 0.50f, 0.75f } )
    {
        capped.Coverage = coverage;
        bare.Coverage   = coverage;

        const double withCanopy = SkyCover( capped );
        const double without    = SkyCover( bare );

        ASSERT_GE( withCanopy, 0.0 );
        ASSERT_GE( without, 0.0 );

        std::printf( "[CloudPlacementSpectrum] coverage %.2f: a storm covers %.4f of the sky with its "
                     "canopy and %.4f without it\n",
                     coverage, withCanopy, without );

        // A TWENTIETH, AND IT IS THE RESIDUAL OF A DERIVATION RATHER THAN A TOLERANCE PICKED TO PASS. The
        // compensation equates the two AREAS exactly; what the two arms then deliver differs by however far
        // the placement is from the independent-bodies model that turns an area into a cover — the same gap
        // §RW measured when it tried to derive the packing law and rejected it. Measured at 0.041 of the sky
        // at its worst of the three settings. Deleting the compensation puts this pair 0.32 apart.
        EXPECT_NEAR( withCanopy, without, 0.05 )
             << "a cumulonimbus' canopy is not paid for by the cluster under it, so the Coverage slider "
                "means one thing for a storm and another for every other genus in the library";
    }
}

TEST( CloudPlacementSpectrum, TheCanopysOwnFootprintDoesNotMoveWithItsStrength )
{
    // WHAT THIS PINS THAT THE SKY CANNOT. The test above says the canopy is paid for; this says the price
    // is the RIGHT SHAPE in the strength — `(1 + kAnvilSpreadPerStrength * S)` — because the compensation
    // divides by exactly what the emission multiplies by. If the two ever stop being the same expression,
    // the canopy's absolute width starts to depend on a slider that is documented as changing only how far
    // it spreads BEYOND its tower, and the sky drifts with `AnvilStrength` for no reason a reader could see.
    const auto canopyRadiusKm = [&]( float strength )
    {
        CloudProceduralFieldParams params     = StormParams();
        params.Species[0].Shape.AnvilStrength = strength;

        const glm::vec2                       origin = CloudProceduralRegionOriginKm( params, 0.0f, 0.0f );
        const std::vector<CloudModellingBlob> blobs  = GenerateCloudProceduralBlobs( params, 0u, origin );

        double total = 0.0;
        double count = 0.0;

        for ( const CloudModellingBlob& blob : blobs )
            if ( std::abs( blob.CentreKm.y - params.Species[0].Shape.AnvilAltitudeKm ) < 1e-3f )
            {
                // The geometric mean of the canopy's two horizontal radii, which is the radius of the
                // circle of the same area — the quantity the compensation is written in.
                total += std::sqrt( static_cast<double>( blob.RadiiKm.x ) * blob.RadiiKm.z );
                count += 1.0;
            }

        EXPECT_GT( count, 0.0 ) << "no canopy at strength " << strength << ", so this measured nothing";
        return total / std::max( count, 1.0 );
    };

    const double quarter = canopyRadiusKm( 0.25f );
    const double half    = canopyRadiusKm( 0.50f );
    const double shipped = canopyRadiusKm( 0.85f );
    const double full    = canopyRadiusKm( 1.00f );

    std::printf( "[CloudPlacementSpectrum] canopy footprint radius at strength 0.25 / 0.50 / 0.85 / 1.00: "
                 "%.4f / %.4f / %.4f / %.4f km\n",
                 quarter, half, shipped, full );

    // A HUNDREDTH. The three arms differ only by a factor that cancels algebraically, so the residual is
    // the per-lump floor and float rounding; without the compensation the ends of this row are 1.44 apart.
    EXPECT_NEAR( half, quarter, 0.01 * quarter )
         << "the canopy's own width follows AnvilStrength, so that slider moves the sky's cover as well as "
            "the storm's proportions";
    EXPECT_NEAR( shipped, quarter, 0.01 * quarter );
    EXPECT_NEAR( full, quarter, 0.01 * quarter );
}

TEST( CloudPlacementSpectrum, TheTowersFootprintConstantsSayWhatTheLayoutActuallyDoes )
{
    // WHY THIS EXISTS: A SABOTAGE FOUND THE HOLE. Dropping the taper term from
    // CloudClusterTowerFootprintRadii — making the tower's footprint one constant instead of a line
    // through two — left the whole repository green, because every test above holds the profile curve
    // fixed and an error common to both arms cancels. That is an untested number in the middle of a calibration,
    // which is the shape DEV_CONTRACT.md §1.3 forbids arrived at from the far side.
    //
    // WHAT IT ASSERTS. The two constants are a QUADRATURE over the lobe layout, not a fit, so they are a
    // prediction about the sky and can be checked as one. With no canopy the gain is exactly 1 and the
    // constants reach no lump at all — so the bake is a completely independent measurement of the same
    // quantity. Independently placed footprints cover `1 - exp(-n A)` of the sky, so `-ln(1 - cover)` is
    // proportional to the footprint AREA whatever the constant of proportionality is, and the ratio of two
    // tapers cancels it: `(kappa(a) / kappa(b))^2`. Nothing about the placement, the cell or the coverage
    // survives into that ratio.
    const auto skyAtTaper = [&]( float taper )
    {
        CloudProceduralFieldParams params     = StormParams();
        params.Species[0].Shape.AnvilStrength = 0.0f;
        params.Species[0].Shape.Profile       = Desert::Graphic::CloudProfileFromTaper( taper );
        params.Coverage                       = 0.5f;
        return SkyCover( params );
    };

    const double flat    = skyAtTaper( 0.4f );
    const double tapered = skyAtTaper( 1.0f );

    ASSERT_GT( flat, 0.0 );
    ASSERT_GT( tapered, 0.0 );

    const double measured = std::log( 1.0 - tapered ) / std::log( 1.0 - flat );

    const double a         = CloudClusterTowerFootprintRadii( Desert::Graphic::CloudProfileFromTaper( 1.0f ) );
    const double b         = CloudClusterTowerFootprintRadii( Desert::Graphic::CloudProfileFromTaper( 0.4f ) );
    const double predicted = ( a * a ) / ( b * b );

    std::printf( "[CloudPlacementSpectrum] tower footprint, taper 1.0 over 0.4: the constants say %.4f, "
                 "the sky says %.4f (%.4f / %.4f of the sky)\n",
                 predicted, measured, tapered, flat );

    // A TWENTY-FIFTH, AND BOTH ENDS OF THE WINDOW ARE ACCOUNTED FOR. The two agree to 0.5 per cent as
    // measured, and the slack is the independent-bodies model this ratio is read through — the same model
    // §RW rejected as a mapping and which survives here only because it is a RATIO. Dropping the taper term
    // makes the left-hand side exactly 1 against a measured 0.936, which is seven times outside this.
    EXPECT_NEAR( measured, predicted, 0.04 * predicted )
         << "the tower footprint the canopy is priced against is not the footprint the layout produces, so "
            "the compensation is exact for a tower nobody generates";
}

TEST( CloudPlacementSpectrum, ACanopyInsideItsOwnTowerIsNotPaidForTwice )
{
    Desert::Graphic::CloudTypeShape shape = StormParams().Species[0].Shape;

    // NO CANOPY AT ALL is the case the gain must leave alone: eight of the nine shipped genera are this
    // row, and a gain other than exactly one on them would move every sky in the project.
    shape.AnvilStrength = 0.0f;
    EXPECT_FLOAT_EQ( CloudClusterFootprintGain( shape ), 1.0f );

    shape.AnvilStrength    = 0.85f;
    shape.AnvilThicknessKm = 0.0f;
    EXPECT_FLOAT_EQ( CloudClusterFootprintGain( shape ), 1.0f )
         << "a canopy of no thickness is never emitted, so pricing one shrinks every storm for nothing";

    // AND A CANOPY NARROWER THAN THE TOWER IT CAPS COSTS THE SKY NOTHING, because it sits inside the
    // tower's own silhouette. The floor is reachable and this is where: a barely-present canopy over an
    // untapered tower is 0.949 cluster radii against the tower's 0.959.
    shape.AnvilThicknessKm = 1.8f;
    shape.AnvilStrength    = 0.0011f;
    shape.Profile          = Desert::Graphic::CloudProfileFromTaper( 0.0f );
    EXPECT_FLOAT_EQ( CloudClusterFootprintGain( shape ), 1.0f )
         << "a canopy hidden inside its own tower is being paid for, so the storm is shrunk for cloud that "
            "was never outside it";

    // The shipped storm, whose canopy is far outside its tower and IS paid for.
    shape.AnvilStrength = 0.85f;
    shape.Profile       = Desert::Graphic::CloudProfileFromTaper( 0.4f );
    EXPECT_GT( CloudClusterFootprintGain( shape ), 1.6f );
    EXPECT_LT( CloudClusterFootprintGain( shape ), 1.8f );
}

// THE CACHE IS THE OTHER PLACE A LIVE SETTING CAN DIE, and this test exists because a sabotage found it
// green. Every field is wired from the component to the bake, and the bake reads every one of them — and
// none of that matters if the renderer's staleness check does not NOTICE the change, because then the
// artist moves the slider and the volume it already has is handed back. That is a dead setting arrived at
// from the far side, and it looks exactly like the knob not being wired.
//
// The check used to live in the renderer's own translation unit, which nothing links, so dropping a field
// from it broke nothing that could go red. It is Assets::CloudProceduralParamsEqual now, and this walks
// every field of the struct in turn.
TEST( CloudPlacementSpectrum, EveryFieldTheBakeReadsMakesTheCachedVolumeStale )
{
    const CloudProceduralFieldParams base = ShippedParams();

    EXPECT_TRUE( CloudProceduralParamsEqual( base, base ) )
         << "a set of parameters is not equal to itself, so this test proves nothing about the rest";

    const auto notices = [&]( CloudProceduralFieldParams moved, const char* what )
    {
        EXPECT_FALSE( CloudProceduralParamsEqual( base, moved ) )
             << what
             << " was changed and the cached volume was still considered current, so moving it in "
                "the editor would do nothing at all";
    };

    CloudProceduralFieldParams moved = base;
    moved.RegionSizeKm               = 40.0f;
    notices( moved, "Region Size" );

    // THE BAKE'S OWN GRID (O8). It is not a look and no artist reaches for it to change the sky, which is
    // exactly why it needs this line: it is the field most likely to be left out of the comparison on the
    // grounds that it "isn't a parameter of the cloud". Two volumes of different resolutions over one
    // region are different bytes, so a view that changed its budget and kept the volume it had would be
    // marching a grid its own setting had already moved away from.
    moved                  = base;
    moved.VolumeSideVoxels = 128u;
    notices( moved, "the volume's own resolution" );

    moved               = base;
    moved.LayerBottomKm = 1.0f;
    notices( moved, "the layer's base altitude" );

    moved                  = base;
    moved.LayerThicknessKm = 2.0f;
    notices( moved, "the layer's thickness" );

    moved               = base;
    moved.BlendRadiusKm = 0.09f;
    notices( moved, "the blend radius" );

    moved                = base;
    moved.ProfileDepthKm = 0.50f;
    notices( moved, "the profile depth" );

    moved          = base;
    moved.Coverage = 0.30f;
    notices( moved, "Coverage" );

    moved                  = base;
    moved.CoverageContrast = 2.0f;
    notices( moved, "Coverage Contrast" );

    moved      = base;
    moved.Seed = 7u;
    notices( moved, "Seed" );

    moved          = base;
    moved.WindAxis = glm::vec2( 0.0f, 1.0f );
    notices( moved, "the wind axis" );

    moved                   = base;
    moved.ResolvableChordKm = 0.25f;
    notices( moved, "the march's resolvable chord" );

    moved                  = base;
    moved.PlacementDensity = 1.0f;
    notices( moved, "Cloud Density" );

    moved                  = base;
    moved.PlacementScatter = 0.0f;
    notices( moved, "Cloud Scatter" );

    moved                      = base;
    moved.PlacementSizeVariety = 0.0f;
    notices( moved, "Cloud Size Variety" );

    moved               = base;
    moved.PatchStrength = 0.0f;
    notices( moved, "Weather Patch Strength" );

    moved             = base;
    moved.PatchTileKm = 40.0f;
    notices( moved, "Weather Patch Size" );

    moved                   = base;
    moved.Species[0].CellKm = 4.0f;
    notices( moved, "the species' cell" );

    moved                       = base;
    moved.Species[0].Anisotropy = 2.0f;
    notices( moved, "the species' anisotropy" );

    moved                                  = base;
    moved.Species[0].Shape.EdgeTopFraction = 0.5f;
    notices( moved, "the type's edge top fraction" );

    // THE VERTICAL PROFILE, and both ends of it. The shape is compared as bytes, so the curve is covered
    // by construction — but "covered by construction" is exactly what was true of the taper this replaced,
    // and §1.3 is about the authored field being asserted live rather than inferred. A whole preset and a
    // SINGLE SAMPLE are both here: sixteen floats inside a memcmp is the shape of a field that starts
    // being compared partially the first time somebody hand-rolls the comparison.
    moved                          = base;
    moved.Species[0].Shape.Profile = Desert::Graphic::CloudProfileTower();
    notices( moved, "the type's vertical profile curve" );

    moved = base;
    moved.Species[0].Shape.Profile.HalfWidth[11] += 0.05f;
    notices( moved, "one sample of the type's vertical profile curve" );

    moved = base;
    moved.Species.push_back( moved.Species[0] );
    notices( moved, "a second species" );

    // THE PAINTED LAYOUT. Binding one, and changing which one is bound, both have to be noticed — the
    // second is the one that would not be if this compared the pointer instead of the content.
    const std::shared_ptr<const CloudLayoutData> stripe = StripeLayout();
    const std::shared_ptr<const CloudLayoutData> other  = StripeLayout( 128u );
    ASSERT_TRUE( stripe && other ) << "the fixture painting could not be built, so nothing below means "
                                      "anything";
    ASSERT_NE( stripe->ContentHash, other->ContentHash )
         << "two paintings of different resolutions hash the same, so this test cannot tell them apart "
            "and neither can the cache";

    moved               = base;
    moved.PatternSource = stripe;
    notices( moved, "binding a cloud layout to the pattern input" );

    moved            = base;
    moved.MaskSource = stripe;
    notices( moved, "binding a cloud layout to the mask input" );

    CloudProceduralFieldParams painted = base;
    painted.PatternSource              = stripe;
    painted.MaskSource                 = stripe;

    moved               = painted;
    moved.PatternSource = other;
    EXPECT_FALSE( CloudProceduralParamsEqual( painted, moved ) )
         << "a DIFFERENT painting was bound and the cached volume was still considered current — which is "
            "what comparing the pointer rather than the content would do only by luck, and what comparing "
            "nothing at all does always";

    moved            = painted;
    moved.MaskSource = other;
    EXPECT_FALSE( CloudProceduralParamsEqual( painted, moved ) )
         << "the MASK input was pointed at a different painting and the cached volume was still considered "
            "current, so one of the two slots is not in the comparison at all";

    // THE TWO SLOTS ARE COMPARED SEPARATELY, and this is the line that says so. Swapping which painting
    // feeds which input changes the sky and leaves any COMBINED checksum untouched — the same "two
    // orderings, one checksum" trap task O-3 measured on the noise sheet, where reversing the tile order
    // in both codecs at once left the round trip perfect.
    CloudProceduralFieldParams crossed = painted;
    crossed.PatternSource              = other;
    crossed.MaskSource                 = stripe;

    CloudProceduralFieldParams swapped = painted;
    swapped.PatternSource              = stripe;
    swapped.MaskSource                 = other;

    EXPECT_FALSE( CloudProceduralParamsEqual( crossed, swapped ) )
         << "exchanging the pattern's painting for the mask's read as no change at all, so the two hashes "
            "are being folded into one number and a swap is invisible to the cache";

    // AND THE FIVE PLACEMENT NUMBERS, ON A BASE THAT HAS A PAINTING. They are deliberately NOT compared
    // when the slot is empty, because none of them reaches a lump then and a rebake for a slider that
    // provably changed nothing is a stall an artist feels. That exemption is exactly the kind of thing that
    // silently grows to cover a field that DOES matter, so it is asserted from both sides: inert without a
    // painting (the test below), noticed with one (here).
    const auto noticesPainted = [&]( CloudProceduralFieldParams m, const char* what )
    {
        EXPECT_FALSE( CloudProceduralParamsEqual( painted, m ) )
             << what
             << " was changed on a layer WITH a painting bound and the cached volume was still considered "
                "current, so moving it in the editor would do nothing at all";
    };

    moved                                  = painted;
    moved.LayoutPlacement.RepeatsPerRegion = 3u;
    noticesPainted( moved, "Layout Repeats" );

    moved                              = painted;
    moved.LayoutPlacement.QuarterTurns = 1u;
    noticesPainted( moved, "Layout Rotation" );

    moved                          = painted;
    moved.LayoutPlacement.OffsetKm = glm::vec2( 5.0f, -2.0f );
    noticesPainted( moved, "Layout Offset" );

    moved                                 = painted;
    moved.LayoutPlacement.PatternStrength = 0.25f;
    noticesPainted( moved, "Layout Pattern Strength" );

    moved                              = painted;
    moved.LayoutPlacement.MaskStrength = 0.25f;
    noticesPainted( moved, "Layout Mask Strength" );
}

// THE GUARD ON THE WALK ABOVE, AND IT IS THE POINT OF THIS PAIR RATHER THAN AN EXTRA.
//
// The test above lists its fields BY HAND. That list is a second statement of the struct's contents, and a
// second statement with nothing checking it against the first is the defect class DEV_CONTRACT.md §2.3.1
// names — it is how the anvil went untested twice in a row and how two channels of the noise volume were
// read by nobody. A field added to CloudProceduralFieldParams without a line added there is a setting the
// cache cannot see, and nothing else in this suite would go red for it.
//
// ⚠️ THE FIRST VERSION OF THIS GUARD PINNED `sizeof` AND A SABOTAGE WALKED STRAIGHT THROUGH IT. Adding
// `float SabotageTwo` to the struct left the size at 136 bytes — it landed in the padding before the
// shared_ptr — and this test stayed GREEN. A size is not a field count, and the two agree only until the
// next field happens to fit in a hole. Recorded here rather than quietly replaced, because the failed
// version is the more useful half of the lesson: a guard that cannot go red for the thing it guards is
// worse than no guard, since it also stops anyone from writing a real one.
//
// WHAT IS HERE INSTEAD IS A STRUCTURED BINDING, and it is exact rather than nearly. A decomposition names
// every field of an aggregate, so a struct with one more or one fewer than the list below does not COMPILE
// — the check fires before the suite even links, it cannot be defeated by padding, and the error stands in
// this file, next to the walk that has to be extended. The runtime half only records the size for the
// benefit of whoever reads a failure on a platform that lays the struct out differently.
TEST( CloudPlacementSpectrum, AddingAFieldToTheBakesParametersForcesAVisitToTheStalenessWalk )
{
    const CloudProceduralFieldParams params;

    // TWENTY FIELDS. If this line stops compiling, a field was added to or removed from
    // CloudProceduralFieldParams. Do BOTH of these before you touch this list:
    //
    //   1. add a line for it to EveryFieldTheBakeReadsMakesTheCachedVolumeStale above, and
    //   2. add it to Assets::CloudProceduralParamsEqual,
    //
    // or the artist will move it in the editor and nothing at all will happen — the dead setting §1.3 of
    // the contract forbids, arrived at from the far side where the knob is wired and the CACHE is what eats
    // it.
    const auto& [regionSizeKm, volumeSideVoxels, layerBottomKm, layerThicknessKm, blendRadiusKm, profileDepthKm,
                 coverage, coverageContrast, seed, placementDensity, placementScatter, placementSizeVariety,
                 patchTileKm, patchStrength, windAxis, layoutPlacement, patternSource, maskSource,
                 resolvableChordKm, species] = params;

    // Named so the decomposition is not optimised away as unused, and asserted on the three that the walk
    // above cannot reach through CloudProceduralParamsEqual at all — a defaulted set must be the shipped
    // state, which is "no painting in either input".
    EXPECT_EQ( patternSource, nullptr ) << "a defaulted set of bake parameters arrives with a painting "
                                           "already bound to the pattern input, so an unpainted scene is "
                                           "not the default state";
    EXPECT_EQ( maskSource, nullptr ) << "a defaulted set of bake parameters arrives with a painting already "
                                        "bound to the mask input";
    EXPECT_EQ( layoutPlacement.RepeatsPerRegion, 1u );
    EXPECT_EQ( layoutPlacement.QuarterTurns, 0u );

    (void)regionSizeKm;
    (void)volumeSideVoxels;
    (void)layerBottomKm;
    (void)layerThicknessKm;
    (void)blendRadiusKm;
    (void)profileDepthKm;
    (void)coverage;
    (void)coverageContrast;
    (void)seed;
    (void)placementDensity;
    (void)placementScatter;
    (void)placementSizeVariety;
    (void)patchTileKm;
    (void)patchStrength;
    (void)windAxis;
    (void)resolvableChordKm;
    (void)species;

    std::printf( "[CloudPlacementSpectrum] CloudProceduralFieldParams is %zu bytes over 20 fields\n",
                 sizeof( CloudProceduralFieldParams ) );
}

// THE PAINTING REPEATS EXACTLY WITH THE SKY, AND THIS IS THE RELATION THE TWO INTEGERS EXIST TO KEEP.
//
// The modelling volume is periodic over the region and everything past the region is REPEAT sampling of it
// — that is what the far field IS (Engine/Assets/CloudProceduralVolume.hpp), and the wrap seam is measured
// at 0.950/255 against 1.239/255 between ordinary neighbours. A painting sampled on a world period that did
// not divide the region would break it, and the defect would be a hard discontinuity across every region
// face, an order of magnitude larger than the seam that exists.
//
// The argument for why it cannot happen is that `RepeatsPerRegion` is a whole number and the rotation is a
// count of QUARTER turns — a square lattice maps onto itself under a quarter turn and under nothing else.
// An argument is not a test. This asserts the consequence directly.
//
// WHAT IT CATCHES AND WHAT IT DOES NOT, both measured rather than assumed. Rotating by 45 degrees instead
// of 90 takes the departure to 0.414 of a period and this goes red instantly. Rotating by 90 degrees
// through a float `cos`/`sin` matrix instead of the exact swap takes it from 1.9e-6 to 3.8e-6 and this
// stays GREEN — which is the correct verdict and not a gap, because that change does not break anything:
// the relation is about the ANGLE being a quarter turn, not about how the quarter turn is spelt. The first
// draft of this comment claimed otherwise and the sabotage disproved it.
TEST( CloudPlacementSpectrum, ThePaintingRepeatsExactlyWithTheRegionAtEveryRotationAndOffset )
{
    constexpr float kRegionKm = 48.0f;

    // Points far from the origin ON PURPOSE. A world that measures in centimetres reaches thousands of
    // kilometres in ordinary use, and any residue in the rotation is multiplied by the distance — so a
    // fault invisible at the origin is the one this has to look for.
    const glm::vec2 probes[] = {
         { 0.0f, 0.0f }, { 1.3f, -7.7f }, { -123.5f, 88.25f }, { 4000.0f, -4000.0f }, { -9999.5f, 12345.75f } };

    double worst = 0.0;

    for ( uint32_t repeats : { 1u, 2u, 3u, 7u, 16u } )
        for ( uint32_t turns = 0u; turns < 4u; ++turns )
            for ( const glm::vec2& offset : { glm::vec2( 0.0f, 0.0f ), glm::vec2( 3.25f, -11.5f ) } )
            {
                CloudLayoutPlacement placement;
                placement.RepeatsPerRegion = repeats;
                placement.QuarterTurns     = turns;
                placement.OffsetKm         = offset;

                for ( const glm::vec2& p : probes )
                    for ( const glm::vec2& step : { glm::vec2( kRegionKm, 0.0f ), glm::vec2( 0.0f, kRegionKm ),
                                                    glm::vec2( -kRegionKm, kRegionKm ) } )
                    {
                        const glm::vec2 here  = CloudLayoutUv( placement, kRegionKm, p );
                        const glm::vec2 there = CloudLayoutUv( placement, kRegionKm, p + step );

                        // Both must land on the same texel of the painting, which is equality MODULO ONE:
                        // the table wraps, so a whole number of turns around it is no movement at all.
                        const glm::vec2 delta = there - here;

                        for ( int axis = 0; axis < 2; ++axis )
                        {
                            const double d    = static_cast<double>( delta[axis] );
                            const double away = std::abs( d - std::round( d ) );
                            worst             = std::max( worst, away );
                        }
                    }
            }

    std::printf( "[CloudPlacementSpectrum] worst departure from a whole period: %.3e texture units\n", worst );

    // A THOUSANDTH OF A TEXEL AT THE FINEST SETTING, which is what the bound means: at 16 repeats of a 1024
    // table over 48 km, one texture unit is 3 km and a thousandth of a texel is 3 metres. A cos/sin
    // rotation at 4000 km is out by far more than that; an exact quarter turn is out by zero.
    EXPECT_LT( worst, 1.0e-3 ) << "the painting does not repeat with the region — it is out by " << worst
                               << " of a period, so the modelling volume is no longer periodic and the far "
                                  "field grows a seam at every region face";
}

// THE PAINTING REDISTRIBUTES THE SKY RATHER THAN ADDING TO IT, and this is the single relation that keeps
// decision D-20 true once a layout can be bound.
//
// `Coverage` addresses a FRACTION OF SKY directly, and every shipped scene was re-authorised against that
// mapping (CALIBRATION.md §RW: out by at most 0.019 over five settings). The painted pattern is ON the
// moment a layout is dropped into the slot, so if it could move the sky's average cover then the slider
// would quietly stop meaning the sky for every painted layer — and the artist would discover it as "my
// clouds got thicker when I loaded my picture".
//
// What prevents it is one subtraction: the pattern is applied about its OWN MEAN, which the container
// computes once and carries in its header. Delete `- layout->PatternMean[...]` in
// Assets::CloudCellCoverage and this goes red.
TEST( CloudPlacementSpectrum, APaintedPatternRedistributesTheSkyRatherThanAddingToIt )
{
    CloudProceduralFieldParams plain = ShippedParams();
    plain.Coverage                   = 0.5f;

    const double unpainted = SkyCover( plain );
    ASSERT_GE( unpainted, 0.0 ) << "the unpainted bake failed, so there is nothing to compare against";

    CloudProceduralFieldParams painted = plain;
    painted.PatternSource              = StripeLayout();
    ASSERT_TRUE( painted.PatternSource ) << "the fixture painting could not be built";

    // HALF STRENGTH AND NOT FULL, and the reason is that the fixture is the harshest painting there is: a
    // hard black-and-white stripe. At full strength the lit half asks for twice the slider and the dark
    // half for none, so both ends CLAMP — and a clamped mean is no longer the mean that was subtracted.
    // At a half the modulation spans 0.25..0.75 around a slider of 0.5 and nothing clamps, which is what
    // makes this a measurement of the zero-mean rule rather than of the clamp.
    painted.LayoutPlacement.PatternStrength = 0.5f;

    const double withPainting = SkyCover( painted );
    ASSERT_GE( withPainting, 0.0 ) << "the painted bake failed";

    std::printf( "[CloudPlacementSpectrum] sky at Coverage 0.50: %.3f unpainted, %.3f painted (%+.3f)\n",
                 unpainted, withPainting, withPainting - unpainted );

    EXPECT_NEAR( withPainting, unpainted, 0.10 )
         << "binding a painting moved the sky's cover by " << std::abs( withPainting - unpainted )
         << " at the same Coverage. The pattern must be applied about its own mean, or the slider stops "
            "meaning the fraction of sky it delivers and decision D-20's re-authorisation of every shipped "
            "scene stops holding for any painted layer";

    // AND THE PAINTING IS NOT INERT, which the assertion above cannot say on its own: a pattern that did
    // nothing at all would pass it perfectly. The stripe must actually move cloud from one half of the sky
    // to the other, and the lumps are where that shows.
    const glm::vec2 origin = CloudProceduralRegionOriginKm( painted, 0.0f, 0.0f );

    const std::vector<CloudModellingBlob> without = GenerateCloudProceduralBlobs( plain, 0u, origin );
    const std::vector<CloudModellingBlob> with    = GenerateCloudProceduralBlobs( painted, 0u, origin );

    size_t moved = 0;
    for ( size_t i = 0; i < std::min( without.size(), with.size() ); ++i )
        if ( without[i].CentreKm != with[i].CentreKm )
            ++moved;

    EXPECT_TRUE( without.size() != with.size() || moved > 0 )
         << "the painting changed neither the number of clouds nor where any of them is, so it is a slot an "
            "artist can fill and never see";
}

// ONE NUMBER, ONE SOURCE — the rule that keeps the painted pattern and the procedural patch field from
// both deciding how busy a cell is.
//
// Two mechanisms setting one value is the second path §1.3 and §4.2 of the contract forbid, and this is the
// first time this subsystem has had two candidates for one number. The rule is that the painting wins when
// it is bound and turned up, and the hash wins otherwise. Asserted from BOTH sides, because only one side
// would be satisfied by a bug that ignored the painting entirely.
TEST( CloudPlacementSpectrum, OnlyOneSourceModulatesACellsCoverage )
{
    CloudProceduralFieldParams painted = ShippedParams();
    painted.PatternSource              = StripeLayout();
    ASSERT_TRUE( painted.PatternSource ) << "the fixture painting could not be built";

    const glm::vec2 origin = CloudProceduralRegionOriginKm( painted, 0.0f, 0.0f );

    const auto centres = []( const std::vector<CloudModellingBlob>& blobs )
    {
        std::vector<glm::vec3> out;
        out.reserve( blobs.size() );
        for ( const CloudModellingBlob& blob : blobs )
            out.push_back( blob.CentreKm );
        return out;
    };

    // WITH THE PAINTING RULING, the patch field must be unreachable — moving its strength from end to end
    // cannot move one lump.
    {
        CloudProceduralFieldParams quiet = painted;
        quiet.PatchStrength              = 0.0f;

        CloudProceduralFieldParams loud = painted;
        loud.PatchStrength              = 1.0f;

        EXPECT_EQ( centres( GenerateCloudProceduralBlobs( quiet, 0u, origin ) ),
                   centres( GenerateCloudProceduralBlobs( loud, 0u, origin ) ) )
             << "the procedural patch field still moved clouds while a painting was ruling the sky, so two "
                "mechanisms are deciding one number and the artist's painting is being argued with";
    }

    // WITH THE PATTERN TURNED DOWN, the patch field must be back — this end of the slider hands the
    // decision to the hash rather than to nothing, which is what makes it a live position rather than an
    // absence of weather.
    {
        CloudProceduralFieldParams quiet      = painted;
        quiet.LayoutPlacement.PatternStrength = 0.0f;
        quiet.PatchStrength                   = 0.0f;

        CloudProceduralFieldParams loud      = painted;
        loud.LayoutPlacement.PatternStrength = 0.0f;
        loud.PatchStrength                   = 1.0f;

        EXPECT_NE( centres( GenerateCloudProceduralBlobs( quiet, 0u, origin ) ),
                   centres( GenerateCloudProceduralBlobs( loud, 0u, origin ) ) )
             << "with Layout Pattern Strength at zero the procedural patch field did nothing either, so "
                "that end of the slider is a sky with no weather in it rather than the procedural sky it is "
                "documented to be";
    }
}

// AN EMPTY SLOT IS THE SKY THAT SHIPPED, AND THE FIVE LAYOUT NUMBERS CANNOT REACH IT.
//
// This is the other half of the phase's acceptance criterion, one level below the frame: the six-point
// protocol says the PICTURE does not move, and this says the PLACEMENT cannot, whatever the layout knobs
// are set to. It is the cheaper of the two and it is the one that localises a regression — a frame that
// moved says only that something did.
TEST( CloudPlacementSpectrum, WithNoPaintingBoundTheLayoutKnobsCannotReachOneCloud )
{
    const CloudProceduralFieldParams plain  = ShippedParams();
    const glm::vec2                  origin = CloudProceduralRegionOriginKm( plain, 0.0f, 0.0f );

    const std::vector<CloudModellingBlob> reference = GenerateCloudProceduralBlobs( plain, 0u, origin );
    ASSERT_FALSE( reference.empty() ) << "the unpainted layer placed no clouds, so this proves nothing";

    CloudProceduralFieldParams turned       = plain;
    turned.LayoutPlacement.RepeatsPerRegion = 9u;
    turned.LayoutPlacement.QuarterTurns     = 3u;
    turned.LayoutPlacement.OffsetKm         = glm::vec2( 17.5f, -4.25f );
    turned.LayoutPlacement.PatternStrength  = 0.0f;
    turned.LayoutPlacement.MaskStrength     = 0.0f;

    const std::vector<CloudModellingBlob> after = GenerateCloudProceduralBlobs( turned, 0u, origin );

    ASSERT_EQ( reference.size(), after.size() )
         << "the layout knobs changed how many clouds an UNPAINTED layer has, which they must not be able "
            "to do at all";

    for ( size_t i = 0; i < reference.size(); ++i )
        ASSERT_EQ( reference[i].CentreKm, after[i].CentreKm )
             << "lump " << i << " moved when a layout knob was turned on a layer with no painting bound";

    // AND THE CACHE AGREES: turning them must not call for a rebake either, or the editor stalls for
    // seconds on a slider that provably cannot change a pixel.
    EXPECT_TRUE( CloudProceduralParamsEqual( plain, turned ) )
         << "the staleness check wants a rebake of two million voxels for layout knobs on a layer with no "
            "painting, and the assertions above have just proved the result would be identical";
}

// The density's ceiling is the bake's cost and is refused rather than survived: the cost is linear in the
// lump count, and a mistyped density is a bake that never returns rather than a sky that looks wrong.
TEST( CloudPlacementSpectrum, ThePlacementNumbersAreRefusedOutsideTheirRanges )
{
    CloudProceduralFieldParams params = ShippedParams();

    params                  = ShippedParams();
    params.PlacementDensity = 250.0f;
    EXPECT_FALSE( ValidateCloudProceduralParams( params ) ) << "a density of 250 clusters per cell was "
                                                               "accepted; that is a bake of a million lumps";

    params                  = ShippedParams();
    params.PlacementScatter = -1.0f;
    EXPECT_FALSE( ValidateCloudProceduralParams( params ) );

    params                      = ShippedParams();
    params.PlacementSizeVariety = 2.0f;
    EXPECT_FALSE( ValidateCloudProceduralParams( params ) );

    params               = ShippedParams();
    params.PatchStrength = 1.5f;
    EXPECT_FALSE( ValidateCloudProceduralParams( params ) );
}

// ===================================================================================================
// PART THREE — THE PROXY AGAINST THE REAL THING
// ===================================================================================================
//
// Everything in part two measures the rasterised column integral of the LUMPS, because a bake is seconds
// and a lattice measurement wants several regions. This is the one test that pays for a bake, and its whole
// job is to stop that proxy from drifting away from what it stands for: the same verdict, measured on the
// baked volume's own top-down projection, on the configuration that ships.
TEST( CloudPlacementSpectrum, TheBakedVolumeAgreesWithTheProxyThatTheShippedSkyHasNoLattice )
{
    const CloudProceduralFieldParams params = ShippedParams();

    const glm::vec2 origin = CloudProceduralRegionOriginKm( params, 0.0f, 0.0f );
    const auto      baked  = BakeCloudProceduralVolume( params, origin );
    ASSERT_TRUE( baked ) << ( baked ? std::string{} : baked.GetError() );

    const uint32_t width  = kCloudProceduralVolumeSide;
    const uint32_t height = kCloudProceduralVolumeHeight;
    const uint32_t depth  = kCloudProceduralVolumeSide;

    std::vector<float> map( static_cast<size_t>( width ) * depth, 0.0f );
    for ( uint32_t z = 0; z < depth; ++z )
        for ( uint32_t x = 0; x < width; ++x )
        {
            float sum = 0.0f;
            for ( uint32_t y = 0; y < height; ++y )
            {
                const size_t at =
                     ( ( static_cast<size_t>( z ) * height + y ) * width + x ) * kCloudProceduralBytesPerVoxel;
                sum += static_cast<float>( baked.GetValue()[at] ) * ( 1.0f / 255.0f );
            }
            map[static_cast<size_t>( z ) * width + x] = sum / static_cast<float>( height );
        }

    const float     perVoxelKm = params.RegionSizeKm / static_cast<float>( width );
    const glm::vec2 extent     = CloudProceduralCellExtentKm( params, params.Species[0] );
    const int       period     = static_cast<int>( extent.x / perVoxelKm + 0.5f );

    const std::vector<double> r = LatticePeak::CircularAutocorrelation(
         map, static_cast<int>( width ), static_cast<int>( depth ), true, static_cast<int>( width ) / 2 );

    const LatticePeak::Peak score = LatticePeak::LatticeScore( r, period, 4 );

    std::printf( "[CloudPlacementSpectrum] the BAKED volume, one region: lattice %.4f (period %.3f km)\n",
                 score.Prominence, extent.x );

    // ONE REGION IS SIXTEEN CELLS ACROSS and an autocorrelation from sixteen things wobbles, so the bound
    // here is looser than part two's by exactly that: it is a check that the proxy has not drifted, not a
    // second measurement of the phase.
    EXPECT_LT( score.Prominence, 0.06 )
         << "the baked volume shows a lattice bump of " << score.Prominence << " at lag " << score.Lag
         << " where the proxy the rest of this suite measures says there is none — the two have drifted "
            "apart and the proxy can no longer be trusted";
}

// ---------------------------------------------------------------------------------------------------
// PART THREE — THE SHAPE OF A BODY, WHICH THE PEAK IS BLIND TO
// ---------------------------------------------------------------------------------------------------
//
// The autocorrelation answers where the bodies are. Two skies with identical placement — one of towers and
// one of plates standing on the same footprints — give the same curve and the same prominence, so "the
// lattice is gone" and "the sky looks natural" are different claims and only the first of them has been
// measurable in this programme so far. LatticePeak::AccumulateChords is the second: how far a ray running
// along an axis stays inside cloud, gathered over every scan line of the baked volume.

TEST( CloudPlacementSpectrum, AChordCensusCountsEveryUnbrokenRunAndNothingElse )
{
    LatticePeak::ChordCensus census;

    // Two runs, of one and of two.
    LatticePeak::AccumulateChords( { 0.0f, 1.0f, 1.0f, 0.0f, 1.0f, 0.0f }, false, census );

    EXPECT_EQ( census.Runs, 2u );
    EXPECT_NEAR( census.MeanVoxels(), 1.5, 1e-9 );

    // An empty line adds nothing at all rather than a run of zero, which would drag the mean down.
    LatticePeak::AccumulateChords( { 0.0f, 0.0f, 0.0f }, false, census );
    EXPECT_EQ( census.Runs, 2u );
    EXPECT_NEAR( census.MeanVoxels(), 1.5, 1e-9 );

    // An empty vector is not a line and must not fault.
    LatticePeak::AccumulateChords( {}, true, census );
    EXPECT_EQ( census.Runs, 2u );
}

// THE WRAP IS THE DIFFERENCE BETWEEN ONE BODY AND TWO, and this is the assertion that says so. The baked
// volume is exactly periodic across the region's faces, so a body straddling a face is the same body seen
// twice; counted without the wrap it arrives as two short chords and the mean falls.
TEST( CloudPlacementSpectrum, TheWrapJoinsABodyThatStraddlesTheRegionsFace )
{
    const std::vector<float> line{ 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f };

    LatticePeak::ChordCensus wrapped;
    LatticePeak::AccumulateChords( line, true, wrapped );
    EXPECT_EQ( wrapped.Runs, 1u );
    EXPECT_NEAR( wrapped.MeanVoxels(), 3.0, 1e-9 );

    LatticePeak::ChordCensus cut;
    LatticePeak::AccumulateChords( line, false, cut );
    EXPECT_EQ( cut.Runs, 2u );
    EXPECT_NEAR( cut.MeanVoxels(), 1.5, 1e-9 );

    // A line that is cloud from end to end is ONE body that circles the world, not two and not none.
    LatticePeak::ChordCensus solid;
    LatticePeak::AccumulateChords( { 1.0f, 1.0f, 1.0f, 1.0f }, true, solid );
    EXPECT_EQ( solid.Runs, 1u );
    EXPECT_NEAR( solid.MeanVoxels(), 4.0, 1e-9 );
}

// THE SPAN IS NOT THE CHORD, and a suite that measured only one of them would call a tower with air in it
// a plate. Both are needed to say what the sky is made of, so both are asserted.
TEST( CloudPlacementSpectrum, ASpanReachesOverAGapAndAChordStopsAtIt )
{
    const std::vector<float> tower{ 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f };

    EXPECT_EQ( LatticePeak::OccupiedSpan( tower ), 4u );

    LatticePeak::ChordCensus census;
    LatticePeak::AccumulateChords( tower, false, census );
    EXPECT_EQ( census.Runs, 2u );
    EXPECT_NEAR( census.MeanVoxels(), 1.0, 1e-9 );

    // A clear column has no span at all rather than a span of one, or the mean over the sky would be
    // dragged towards the empty half of it.
    EXPECT_EQ( LatticePeak::OccupiedSpan( { 0.0f, 0.0f, 0.0f } ), 0u );
    EXPECT_EQ( LatticePeak::OccupiedSpan( {} ), 0u );

    // A solid body's span IS its chord, which is what makes the ratio of the two a measure of solidity.
    EXPECT_EQ( LatticePeak::OccupiedSpan( { 0.0f, 1.0f, 1.0f, 1.0f, 0.0f } ), 3u );
}

// THE RELATION, AND IT IS THE ONE §2.3.1 ASKS FOR: the two chords are set by two DIFFERENT numbers, and a
// change to one must move one of them and not the other. The horizontal chord follows the placement cell —
// the cluster's radius is a fraction of it — and the vertical chord follows the type's own band. Sizing the
// cluster off the band, or the stack off the cell, would tie them together and this test is what would say
// so.
//
// It is also where the number the owner was shown comes from: the shipped congestus is measurably WIDER
// THAN IT IS TALL, and by how much is Docs/Clouds/CALIBRATION.md section RW2.
TEST( CloudPlacementSpectrum, TheBodysWidthFollowsTheCellAndItsHeightFollowsTheBand )
{
    const auto measure =
         []( const CloudProceduralFieldParams& params, double& horizontalKm, double& verticalKm, double& spanKm )
    {
        const glm::vec2 origin = CloudProceduralRegionOriginKm( params, 0.0f, 0.0f );
        const auto      baked  = BakeCloudProceduralVolume( params, origin );
        ASSERT_TRUE( baked ) << ( baked ? std::string{} : baked.GetError() );

        const uint32_t width  = kCloudProceduralVolumeSide;
        const uint32_t height = kCloudProceduralVolumeHeight;
        const uint32_t depth  = kCloudProceduralVolumeSide;

        LatticePeak::ChordCensus horizontal;
        LatticePeak::ChordCensus vertical;
        std::vector<float>       line;

        for ( uint32_t y = 0; y < height; ++y )
            for ( uint32_t z = 0; z < depth; ++z )
            {
                line.assign( width, 0.0f );
                for ( uint32_t x = 0; x < width; ++x )
                {
                    const size_t at =
                         ( ( static_cast<size_t>( z ) * height + y ) * width + x ) * kCloudProceduralBytesPerVoxel;
                    line[x] = static_cast<float>( baked.GetValue()[at] );
                }
                LatticePeak::AccumulateChords( line, true, horizontal );
            }

        double spanVoxels  = 0.0;
        size_t spanColumns = 0;

        for ( uint32_t z = 0; z < depth; ++z )
            for ( uint32_t x = 0; x < width; ++x )
            {
                line.assign( height, 0.0f );
                for ( uint32_t y = 0; y < height; ++y )
                {
                    const size_t at =
                         ( ( static_cast<size_t>( z ) * height + y ) * width + x ) * kCloudProceduralBytesPerVoxel;
                    line[y] = static_cast<float>( baked.GetValue()[at] );
                }

                LatticePeak::AccumulateChords( line, false, vertical );

                const size_t span = LatticePeak::OccupiedSpan( line );
                if ( span > 0 )
                {
                    spanVoxels += static_cast<double>( span );
                    ++spanColumns;
                }
            }

        const double perVoxelUpKm = static_cast<double>( params.LayerThicknessKm ) / static_cast<double>( height );

        horizontalKm = horizontal.MeanVoxels() *
                       ( static_cast<double>( params.RegionSizeKm ) / static_cast<double>( width ) );
        verticalKm = vertical.MeanVoxels() * perVoxelUpKm;
        spanKm = ( spanColumns > 0 ) ? ( spanVoxels / static_cast<double>( spanColumns ) ) * perVoxelUpKm : 0.0;
    };

    double shippedWide = 0.0;
    double shippedTall = 0.0;
    double shippedSpan = 0.0;
    measure( ShippedParams(), shippedWide, shippedTall, shippedSpan );

    std::printf( "[CloudPlacementSpectrum] the shipped congestus: chord %.3f km across, %.3f km up, "
                 "%.2fx wider than tall; column span %.3f km, solidity %.2f\n",
                 shippedWide, shippedTall, shippedWide / shippedTall, shippedSpan, shippedTall / shippedSpan );

    EXPECT_GT( shippedWide, shippedTall )
         << "the shipped type is no longer wider than it is tall — section RW2 of "
            "Docs/Clouds/CALIBRATION.md is written against a body that is, and the number it quotes has to "
            "be re-measured";

    // HALVE THE CELL AND THE BODY NARROWS; THE BAND IS UNTOUCHED AND SO IS THE HEIGHT. Anything else means
    // the two have been tied to one number.
    CloudProceduralFieldParams narrow = ShippedParams();
    narrow.Species[0].CellKm *= 0.5f;

    double narrowWide = 0.0;
    double narrowTall = 0.0;
    double narrowSpan = 0.0;
    measure( narrow, narrowWide, narrowTall, narrowSpan );

    std::printf( "[CloudPlacementSpectrum] at half the cell: chord %.3f km across, %.3f km up, span %.3f km\n",
                 narrowWide, narrowTall, narrowSpan );

    EXPECT_LT( narrowWide, shippedWide * 0.90 )
         << "halving the placement cell left the bodies as wide as before, so the cluster's width no "
            "longer follows the cell it is placed in";

    // AND THE LUMP NARROWS WITH IT ON BOTH AXES, WHICH IS §SIL'S DECISION STATED ON THE BAKE. §RW2's
    // version of this test demanded the opposite — that halving the cell leave the vertical chord alone —
    // and it passed because a lump's height came from the type's band divided by a constant in the
    // placement file while its width came from the cell. Those are the two unrelated numbers §RW2 measured
    // at 2.1 to 2.5 to one, and a lump with ONE size cannot satisfy the old assertion. What must still
    // hold, and does, is the relation one level up: the BODY's height is the type's band.
    EXPECT_LT( narrowTall, shippedTall * 0.90 )
         << "halving the placement cell left the LUMPS as tall as before, so a lump's two radii have been "
            "untied from one another again — the defect §SIL exists to remove";
    EXPECT_NEAR( narrowSpan, shippedSpan, shippedSpan * 0.25 )
         << "halving the placement cell changed how tall the BODY stands, so the stack has been tied to "
            "the cell instead of to the type's own band";
}

// A LUMP HAS ONE SIZE — the decision §SIL records, asserted on the lumps the generator emits rather than on
// the constant that produces them.
//
// WHAT IS ACTUALLY UNDER TEST. The claim is not "the ratio is 0.75"; a test that read the constant back
// would be checking its own arithmetic. The claim is that a lump's height and its width are ONE quantity —
// so the ratio between them is the same for two types whose cells differ by a factor of three and whose
// bands differ by a factor of four. If either radius picks up an input the other does not, those four
// measurements part company, which is precisely how the defect arrived in the first place.
//
// THE RATIO IS TAKEN AGAINST THE GEOMETRIC MEAN of the two horizontal radii, because the plan-view outline
// carries two independent wobble draws and the vertical radius carries their geometric mean — a lump is
// scaled by the wobble, not reshaped by it.
TEST( CloudPlacementSpectrum, ALumpsHeightAndItsWidthAreOneQuantity )
{
    const auto ratioFor = []( float cellKm, float baseKm, float topKm, float rampFraction = 0.04f )
    {
        CloudProceduralFieldParams params        = ShippedParams();
        params.Species[0].CellKm                 = cellKm;
        params.Species[0].Shape.BaseAltitudeKm   = baseKm;
        params.Species[0].Shape.TopAltitudeKm    = topKm;
        params.Species[0].Shape.BaseRampFraction = rampFraction;
        params.LayerBottomKm                     = baseKm;
        params.LayerThicknessKm                  = topKm - baseKm;

        const glm::vec2                       origin = CloudProceduralRegionOriginKm( params, 0.0f, 0.0f );
        const std::vector<CloudModellingBlob> blobs  = GenerateCloudProceduralBlobs( params, 0u, origin );

        double sum   = 0.0;
        double count = 0.0;

        for ( const CloudModellingBlob& blob : blobs )
        {
            // The LOWEST lump of every cluster is reshaped on purpose by Base Ramp Fraction — it is the
            // spreading floor a type authors — so it is the one lump this relation does not cover, and it
            // is excluded by the thing that identifies it rather than by its index: it is the only lump
            // whose height is a stated fraction of what its width would give.
            //
            // THE THRESHOLD IS THE CONSTANT ITSELF AND IT USED TO BE A LITERAL SPELLING OF IT. It stood as
            // `0.6 * 0.75`, which is 0.45 — the aspect that shipped at the time, written in a form that
            // named neither the constant nor the fact that it WAS the constant. §SIL2 moved the aspect to
            // 0.75 and this line would have gone on excluding everything below 0.45: the ramped lumps it
            // exists to drop are at `aspect * rampFactor` and rampFactor is never below 0.525, so at the
            // new aspect they land ABOVE the old threshold and would have been averaged in as if they were
            // unreshaped. The test would not have failed — it would have quietly measured something else.
            //
            // A lump the ramp and the band clamp have both left alone measures the constant EXACTLY (the
            // vertical radius takes the geometric mean of the two horizontal wobble draws, which is what
            // makes that true), so the threshold is the constant with one part in a thousand of slack for
            // the float division rather than a fraction of it.
            const double wide = std::sqrt( static_cast<double>( blob.RadiiKm.x ) * blob.RadiiKm.z );
            const double tall = static_cast<double>( blob.RadiiKm.y );
            const double at   = tall / std::max( wide, 1e-9 );

            if ( at < 0.999 * kCloudLumpVerticalOverHorizontal )
                continue;

            sum += at;
            count += 1.0;
        }

        EXPECT_GT( count, 0.0 ) << "no lump was measured at all, so this test asserted nothing";
        return sum / std::max( count, 1.0 );
    };

    // A 3.6 km band in a 3 km cell — the shipped congestus — against a 0.9 km band in a 1 km cell. Both the
    // cell and the band move, and by different factors.
    const double shipped = ratioFor( 3.0f, 2.20f, 5.80f );
    const double small   = ratioFor( 1.0f, 2.20f, 3.10f );
    const double deep    = ratioFor( 3.0f, 2.20f, 9.40f );

    // AND ONE ARM WITH A FAT BASE RAMP, WHICH IS HERE BECAUSE A SABOTAGE STAYED GREEN WITHOUT IT.
    //
    // The exclusion below drops the one lump per cluster that Base Ramp Fraction deliberately reshapes, and
    // its threshold has to be the aspect constant. It used to be the literal `0.6 * 0.75` — the constant of
    // the day, 0.45, spelled out. Putting that literal back at the new aspect of 0.75 left this test GREEN,
    // and the reason is that the fixture's ramp is the shipped congestus' 0.04: that gives a ramp factor of
    // 0.52 and a ramped lump at 0.39, which the stale threshold still happens to exclude. The hole was real
    // and invisible.
    //
    // 0.25 IS THE CIRRUS' OWN AUTHORED VALUE, not a number invented to break the test. It gives a ramp
    // factor of 0.625 and a ramped lump at 0.469 — ABOVE a stale 0.45 threshold, so a threshold that has
    // not moved with the constant averages that lump in and drags the mean to about 0.70, which the
    // assertion against the constant then catches. A test whose fixture cannot express the defect is a test
    // that passes for the wrong reason.
    const double fatRamp = ratioFor( 3.0f, 2.20f, 5.80f, 0.25f );

    std::printf( "[CloudPlacementSpectrum] lump height over width: shipped %.4f, third the cell %.4f, "
                 "twice the band %.4f, fat base ramp %.4f\n",
                 shipped, small, deep, fatRamp );

    EXPECT_NEAR( fatRamp, kCloudLumpVerticalOverHorizontal, 0.01 )
         << "with a Base Ramp Fraction of 0.25 the measured aspect is " << fatRamp << ", not the "
         << kCloudLumpVerticalOverHorizontal
         << " the constant declares — the lump the base ramp reshapes is being averaged in as though it "
            "were an unreshaped one, which is what happens when the exclusion threshold below stops being "
            "the constant and becomes a stale spelling of an older value";

    EXPECT_NEAR( shipped, small, 0.02 )
         << "a third of the cell gave lumps of a different SHAPE, so the two radii are not one quantity — "
            "one of them has picked up the cell and the other has not";
    EXPECT_NEAR( shipped, deep, 0.02 )
         << "twice the band gave lumps of a different SHAPE, so the type's altitudes are deciding a lump's "
            "height again — which is §RW2's defect, back";

    // AND THE SHAPE THE LUMPS ACTUALLY HAVE IS THE ONE THE EXPORTED CONSTANT DECLARES.
    //
    // The three assertions above are mutual — they say the ratio does not move when the cell and the band
    // move — and a generator that ignored Assets::kCloudLumpVerticalOverHorizontal entirely would satisfy
    // every one of them. That is not a hypothetical: the constant is exported precisely so that
    // Desert/Tests/Engine/CloudField can read it and assert the erosion is calibrated against it, and that
    // whole relation is worth nothing if the number the test reads and the number the generator uses are
    // allowed to be two different numbers. This is the line that makes the exported symbol the ONE
    // statement of the lump's shape rather than a copy of it that happens to agree today.
    EXPECT_NEAR( shipped, kCloudLumpVerticalOverHorizontal, 0.01 )
         << "the lumps the generator emits measure " << shipped << " tall over wide, but "
         << "Assets::kCloudLumpVerticalOverHorizontal declares " << kCloudLumpVerticalOverHorizontal
         << ". The exported constant is what Desert/Tests/Engine/CloudField reads when it checks that the "
            "erosion's strength is calibrated against the shape of the lump, so if the generator has gone "
            "back to spelling that shape out for itself, the erosion is being checked against a number "
            "nothing in the sky uses.";
}

// EVERY LUMP STANDS INSIDE THE ALTITUDES ITS OWN TYPE DECLARES, which the stack did not before.
//
// The old layout put lump centres at `base + band * fullness * t` and then gave each lump a vertical radius
// on top of that, so the body reached `band * fullness + 2 * lumpRadius` — 3.78 km out of a 3.60 km band on
// the shipped congestus. A type's Base and Top Altitude are the two numbers an artist reads off a
// meteorological table, and a body that stands half a kilometre above its own Top is those two numbers not
// meaning what they say.
//
// THE ANVIL IS EXCLUDED BY NAME AND NOT BY ACCIDENT: it authors its own altitude and thickness, and the
// cumulonimbus' anvil sits deliberately above the tower's top with a gap under it.
TEST( CloudPlacementSpectrum, EveryLumpStandsInsideItsTypesOwnBand )
{
    const auto checkBand = []( float baseKm, float topKm, float cellKm, float edgeTop )
    {
        CloudProceduralFieldParams params       = ShippedParams();
        params.Species[0].CellKm                = cellKm;
        params.Species[0].Shape.BaseAltitudeKm  = baseKm;
        params.Species[0].Shape.TopAltitudeKm   = topKm;
        params.Species[0].Shape.EdgeTopFraction = edgeTop;
        params.LayerBottomKm                    = baseKm;
        params.LayerThicknessKm                 = topKm - baseKm;

        const glm::vec2                       origin = CloudProceduralRegionOriginKm( params, 0.0f, 0.0f );
        const std::vector<CloudModellingBlob> blobs  = GenerateCloudProceduralBlobs( params, 0u, origin );

        ASSERT_FALSE( blobs.empty() );

        float lowest  = std::numeric_limits<float>::max();
        float highest = -std::numeric_limits<float>::max();

        for ( const CloudModellingBlob& blob : blobs )
        {
            lowest  = std::min( lowest, blob.CentreKm.y - blob.RadiiKm.y );
            highest = std::max( highest, blob.CentreKm.y + blob.RadiiKm.y );
        }

        std::printf( "[CloudPlacementSpectrum] band %.2f..%.2f km, cell %.2f km: lumps span %.3f..%.3f km\n",
                     baseKm, topKm, cellKm, lowest, highest );

        // A voxel of the shipped volume is the tolerance, because a body inside the band by less than the
        // volume can express is inside it as far as any picture is concerned.
        const float toleranceKm = ( topKm - baseKm ) / static_cast<float>( kCloudProceduralVolumeHeight );

        EXPECT_GE( lowest, baseKm - toleranceKm )
             << "a lump hangs below the type's own Base Altitude, so the volume clips its floor flat";
        EXPECT_LE( highest, topKm + toleranceKm )
             << "a lump stands above the type's own Top Altitude, so a type's altitudes do not bound the "
                "body they describe";
    };

    // The shipped congestus, a thin deck whose band the lump floor fights for, and a deep tower.
    checkBand( 2.20f, 5.80f, 3.0f, 0.15f );
    checkBand( 0.60f, 1.60f, 1.05f, 0.80f );
    checkBand( 0.90f, 9.00f, 6.0f, 0.12f );
}

// THE SKY'S COVER DOES NOT MOVE WITH THE ANISOTROPY — §SIL's first defect, stated as the relation it broke.
//
// `CloudProceduralCellExtentKm` holds the cell's AREA constant under anisotropy and its own comment says
// why: so that stretching the lattice draws a cluster out into a band instead of making the sky emptier.
// The cluster was sized by `min(extent)` instead, so the sky emptied as the SQUARE of the stretch and a
// cirrus layer — anisotropy 8 — delivered 0.089 of the sky where its slider asked for 0.5. Four of the nine
// shipped types are affected. Nothing in the repository asserted the relation the comment claimed.
//
// MEASURED ON THE RASTERISED PROXY and not on a bake, for the reason the file note gives: this needs four
// arms and a bake is seconds. The proxy is the lumps' own column integral, which is what the cover of a
// projected volume counts.
TEST( CloudPlacementSpectrum, TheSkysCoverDoesNotMoveWithTheAnisotropy )
{
    const auto coverAt = []( float anisotropy )
    {
        CloudProceduralFieldParams params = ShippedParams();
        params.Coverage                   = 0.50f;
        params.Species[0].Anisotropy      = anisotropy;

        const glm::vec2          origin = CloudProceduralRegionOriginKm( params, 0.0f, 0.0f );
        const std::vector<float> map =
             RasteriseColumns( GenerateCloudProceduralBlobs( params, 0u, origin ), origin, params.RegionSizeKm );

        double covered = 0.0;
        for ( float v : map )
            covered += ( v > 0.0f ) ? 1.0 : 0.0;
        return covered / static_cast<double>( map.size() );
    };

    // The four anisotropies the shipped library actually uses: the isotropic types, the stratocumulus and
    // the altocumulus, the lenticular, and the cirrus.
    const double isotropic = coverAt( 1.0f );
    const double rowed     = coverAt( 1.6f );
    const double across    = coverAt( 0.2f );
    const double fibrous   = coverAt( 8.0f );

    std::printf( "[CloudPlacementSpectrum] cover at anisotropy 1.0 / 1.6 / 0.2 / 8.0: %.4f %.4f %.4f %.4f\n",
                 isotropic, rowed, across, fibrous );

    // A TWENTIETH OF THE SKY, and the bound is what the defect cleared by an order of magnitude: the
    // measured spread on the shipped generator was 0.519 down to 0.089, which is 0.43 of the sky.
    EXPECT_NEAR( rowed, isotropic, 0.05 )
         << "a stratocumulus' rowed lattice moved the sky's cover, so its Coverage slider means something "
            "different from a cumulus'";
    EXPECT_NEAR( across, isotropic, 0.05 ) << "a lenticular's across-wind lattice moved the sky's cover";
    EXPECT_NEAR( fibrous, isotropic, 0.05 )
         << "a cirrus' anisotropy of 8 moved the sky's cover — which is the defect measured in §RW, where "
            "it delivered a fifth of what its slider asked for";
}

// AND THE STRETCH GOES ALONG THE WIND RATHER THAN ACROSS IT, asked of the DISTANCE FIELD rather than of the
// comment that derives the rotation.
//
// WHY THIS TEST EXISTS AT ALL. The lump's stretch is expressed as an anisotropic radius plus a yaw, and the
// yaw's sign depends on a convention — `glm::quat( radians( euler ) )` — that is stated in one file and
// consumed in another. A sign error there is invisible in every aggregate this suite measures: the cover is
// the same, the chords are the same on average, and the lattice peak is the same. It shows up only as a
// cirrus whose bands comb out at right angles to the wind, which is a thing only a frame or this test can
// see. So the question is put to the field: at a wind of 45 degrees, is a point one radius along the wind
// INSIDE the lump and the same distance across it OUTSIDE?
TEST( CloudPlacementSpectrum, AStretchedLumpIsLongAlongTheWindAndNotAcrossIt )
{
    CloudProceduralFieldParams params = ShippedParams();
    params.Species[0].Anisotropy      = 8.0f;

    // A wind that is on neither volume axis, so that a lump which had simply kept the world's axes would
    // fail rather than accidentally agree.
    const glm::vec2 along( 0.70710678f, 0.70710678f );
    const glm::vec2 across( -along.y, along.x );
    params.WindAxis = along;

    const glm::vec2                       origin = CloudProceduralRegionOriginKm( params, 0.0f, 0.0f );
    const std::vector<CloudModellingBlob> blobs  = GenerateCloudProceduralBlobs( params, 0u, origin );

    ASSERT_FALSE( blobs.empty() );

    size_t longAlong  = 0;
    size_t longAcross = 0;

    for ( const CloudModellingBlob& blob : blobs )
    {
        // The probe is the lump's own LONGER horizontal radius, stepped from its centre. A lump stretched
        // along the wind contains that point along the wind and not across it; one stretched the other way
        // gives the opposite pair; one that ignored the rotation gives a mixture that is neither.
        const float reachKm = 0.9f * std::max( blob.RadiiKm.x, blob.RadiiKm.z );

        const CloudModellingPreparedBlob prepared = PrepareCloudModellingBlob( blob );

        const glm::vec3 downwind = blob.CentreKm + glm::vec3( along.x * reachKm, 0.0f, along.y * reachKm );
        const glm::vec3 sideways = blob.CentreKm + glm::vec3( across.x * reachKm, 0.0f, across.y * reachKm );

        const bool insideDownwind = CloudModellingBlobDistanceKm( prepared, downwind ) < 0.0f;
        const bool insideSideways = CloudModellingBlobDistanceKm( prepared, sideways ) < 0.0f;

        if ( insideDownwind && !insideSideways )
            ++longAlong;
        if ( insideSideways && !insideDownwind )
            ++longAcross;
    }

    std::printf( "[CloudPlacementSpectrum] of %zu lumps at anisotropy 8: %zu reach downwind only, %zu "
                 "reach across only\n",
                 blobs.size(), longAlong, longAcross );

    EXPECT_EQ( longAcross, 0u ) << "a lump reaches ACROSS the wind and not along it, so the yaw that turns "
                                   "the lump into the lattice's frame has the wrong sign — a cirrus would "
                                   "comb out at right angles to the wind that combs it";
    EXPECT_GT( longAlong, blobs.size() / 2 )
         << "fewer than half the lumps reach downwind at an anisotropy of 8, so the stretch is not being "
            "applied to the lump at all and only its placement was drawn out";
}

// THE SILHOUETTE INSTRUMENT, ON SHAPES WHOSE ANSWER THIS FILE CHOSE.
//
// WHY IT IS BEING TESTED NOW. §DS chose the erosion's Detail Strength on a "silhouette raggedness" it
// reported to four decimals, and the code that produced those numbers was never committed. §SIL has to show
// that a change to the PLACEMENT did not spend the scalloped edge §DS bought, and there was nothing in the
// tree to show it with. The quantity is in `LatticePeakMath.hpp` now and this is the check on it: a square
// and a comb of the same area must not measure the same, and the number must not move when the picture is
// simply made bigger.
TEST( CloudPlacementSpectrum, RaggednessSeesTheEdgeAndNotTheArea )
{
    const int side = 64;

    const auto blank = []() { return std::vector<float>( static_cast<size_t>( side ) * side, 0.0f ); };

    // A solid 32 x 32 square in the middle: 1024 pixels of area and 128 of boundary.
    std::vector<float> square = blank();
    for ( int y = 16; y < 48; ++y )
        for ( int x = 16; x < 48; ++x )
            square[static_cast<size_t>( y ) * side + x] = 1.0f;

    // A COMB OF THE SAME AREA: sixteen 2 x 32 teeth with a gap between each pair, so the area is 1024
    // again and the boundary is far longer. If the quantity were measuring the amount of cloud these two
    // would be equal, and they are not.
    std::vector<float> comb = blank();
    for ( int y = 16; y < 48; ++y )
        for ( int t = 0; t < 16; ++t )
            for ( int x = 0; x < 2; ++x )
                comb[static_cast<size_t>( y ) * side + ( t * 4 + x )] = 1.0f;

    const double squareRagged = LatticePeak::SilhouetteRaggedness( square, side, side );
    const double combRagged   = LatticePeak::SilhouetteRaggedness( comb, side, side );

    std::printf( "[CloudPlacementSpectrum] raggedness: square %.4f, comb of the same area %.4f\n", squareRagged,
                 combRagged );

    EXPECT_GT( combRagged, squareRagged * 3.0 )
         << "a comb and a solid square of the SAME AREA measure nearly the same raggedness, so the "
            "quantity is reading how much cloud there is instead of how ragged its edge is";

    // AND THE SAME PICTURE AT TWICE THE RESOLUTION IS THE SAME NUMBER, which is what makes it comparable
    // across frames — and this assertion is the one that chose the normalisation. Written §DS's way, with
    // the perimeter and the area as fractions of the FRAME, this measures half as much at twice the
    // resolution, and the conversion between the two forms is now written on the function.
    const int          big = side * 2;
    std::vector<float> bigSquare( static_cast<size_t>( big ) * big, 0.0f );
    for ( int y = 32; y < 96; ++y )
        for ( int x = 32; x < 96; ++x )
            bigSquare[static_cast<size_t>( y ) * big + x] = 1.0f;

    EXPECT_NEAR( LatticePeak::SilhouetteRaggedness( bigSquare, big, big ), squareRagged, 0.02 * squareRagged )
         << "doubling the resolution changed the raggedness of the same shape, so the quantity carries a "
            "scale and two frames of different sizes cannot be compared with it";

    // A single square measures 4 for the same reason a disc measures 2*sqrt(pi): the ratio is a property of
    // the SHAPE. Pinned so that a change of normalisation cannot pass unnoticed.
    EXPECT_NEAR( squareRagged, 4.0, 1e-6 )
         << "a solid square no longer measures 4, so the quantity's units have moved and every number "
            "reported against it has moved with them";

    // THE FRAME'S OWN EDGE IS NOT A CLOUD EDGE. A mask that is cloud everywhere has no boundary at all, and
    // counting the crop as one would make the number depend on where the rectangle was taken.
    std::vector<float> full( static_cast<size_t>( side ) * side, 1.0f );
    EXPECT_DOUBLE_EQ( LatticePeak::SilhouetteRaggedness( full, side, side ), 0.0 )
         << "a frame that is entirely cloud reported a silhouette, so the crop's own edge is being counted";
}

// AND THE INTERIOR TEXTURE READS THE BODY AND NOT THE SKY, which is the half of §DS's pair that says
// whether the erosion is carving billows rather than only nibbling the outline.
TEST( CloudPlacementSpectrum, TheInteriorLaplacianReadsOnlyWhatTheMaskCallsCloud )
{
    const int    side   = 64;
    const size_t pixels = static_cast<size_t>( side ) * side;

    // The left half is cloud and is FLAT; the right half is not cloud and is a violent checker. A quantity
    // that respected the mask reports zero; one that did not reports the checker.
    std::vector<float> value( pixels, 0.0f );
    std::vector<float> mask( pixels, 0.0f );

    for ( int y = 0; y < side; ++y )
        for ( int x = 0; x < side; ++x )
        {
            const size_t at = static_cast<size_t>( y ) * side + x;
            if ( x < side / 2 )
            {
                mask[at]  = 1.0f;
                value[at] = 0.5f;
            }
            else
                value[at] = ( ( x + y ) % 2 == 0 ) ? 1.0f : 0.0f;
        }

    EXPECT_NEAR( LatticePeak::InteriorLaplacian( value, mask, side, side, 4 ), 0.0, 1e-9 )
         << "a flat cloud beside a violent sky measured texture, so the mask is not being respected and "
            "every number from this quantity is partly the sky's";

    // Now give the cloud its own texture and demand it be found.
    for ( int y = 0; y < side; ++y )
        for ( int x = 0; x < side / 2; ++x )
            value[static_cast<size_t>( y ) * side + x] = ( ( x / 4 + y / 4 ) % 2 == 0 ) ? 0.8f : 0.2f;

    EXPECT_GT( LatticePeak::InteriorLaplacian( value, mask, side, side, 4 ), 0.1 )
         << "a cloud with billows in it measured no texture at all";
}

// =====================================================================================================
// THE AUTHORING PANEL'S TWO QUANTITIES — CALIBRATION.md §PTP
//
// The Cloud Layout panel shows an artist a top-down MAP of the sky a painting makes and a measure of the
// painting's own STROKES. Both are pure functions of the same parameters the bake takes, and both exist to
// carry facts §PT could only measure with a renderer and a protocol. What follows pins the relations they
// are made of — not their values, which are pictures.
// =====================================================================================================

namespace
{
    /// A painting with a single horizontal BAND on chosen rows and a single vertical BAR on chosen columns,
    /// so a test can ask "where in the world did row 3 end up" and get an answer with a side to it.
    std::shared_ptr<const CloudLayoutData> BandLayout( uint32_t resolution, uint32_t firstRow, uint32_t rows )
    {
        std::vector<unsigned char> pixels( static_cast<size_t>( resolution ) * resolution * 4u, 0u );

        for ( uint32_t y = firstRow; y < firstRow + rows; ++y )
            for ( uint32_t x = 0; x < resolution; ++x )
            {
                const size_t at = ( static_cast<size_t>( y % resolution ) * resolution + x ) * 4u;
                pixels[at + 0]  = 255u;
                pixels[at + 1]  = 255u;
                pixels[at + 2]  = 255u;
                pixels[at + 3]  = 128u;
            }

        const uint32_t channels[kCloudLayoutChannels] = { 0u, 1u, 2u, 3u };

        CloudLayoutCanvas canvas;
        if ( !SetCloudLayoutCanvasPatternFromImage( canvas, pixels, resolution, resolution, channels ) )
            return nullptr;

        auto made = MakeCloudLayoutFromCanvas( canvas );
        if ( !made )
            return nullptr;

        return std::make_shared<const CloudLayoutData>( made.ExtractValue() );
    }

    /// A painting that varies SMOOTHLY and in BOTH axes, and tiles exactly: one period of a sine along u
    /// plus one along v. It exists because a stripe cannot catch a small displacement — see the comment on
    /// TheMapAgreesWithTheBakesOwnCoverageCellForCell, where a sabotage of four tenths of a cell was
    /// measured GREEN against a stripe because no cell centre ever crossed its one hard edge. A field with
    /// no flat places has no hiding places.
    std::shared_ptr<const CloudLayoutData> RippleLayout( uint32_t resolution = 64u )
    {
        std::vector<unsigned char> pixels( static_cast<size_t>( resolution ) * resolution * 4u, 0u );

        for ( uint32_t y = 0; y < resolution; ++y )
            for ( uint32_t x = 0; x < resolution; ++x )
            {
                const double u = ( static_cast<double>( x ) + 0.5 ) / resolution;
                const double v = ( static_cast<double>( y ) + 0.5 ) / resolution;
                // Spelled out rather than M_PI: that macro is a POSIX extension MSVC does not define
                // without _USE_MATH_DEFINES, so this line built here and broke both Windows jobs.
                constexpr double kTwoPi = 6.283185307179586;
                const double     f      = 0.5 + 0.25 * std::sin( kTwoPi * u ) + 0.25 * std::sin( kTwoPi * v );

                const size_t at = ( static_cast<size_t>( y ) * resolution + x ) * 4u;
                pixels[at + 0]  = static_cast<unsigned char>( std::lround( f * 255.0 ) );
                pixels[at + 1]  = pixels[at + 0];
                pixels[at + 2]  = pixels[at + 0];
                pixels[at + 3]  = 128u;
            }

        const uint32_t channels[kCloudLayoutChannels] = { 0u, 1u, 2u, 3u };

        CloudLayoutCanvas canvas;
        if ( !SetCloudLayoutCanvasPatternFromImage( canvas, pixels, resolution, resolution, channels ) )
            return nullptr;

        auto made = MakeCloudLayoutFromCanvas( canvas );
        if ( !made )
            return nullptr;

        return std::make_shared<const CloudLayoutData>( made.ExtractValue() );
    }

    /// A painting carrying one vertical bar @p width texels wide whose left edge is at @p firstColumn,
    /// wrapping past the right edge. The fixture the stroke measure is asked about, because a bar has
    /// exactly one width and it is known before the measurement runs.
    CloudLayoutData BarLayout( uint32_t resolution, uint32_t firstColumn, uint32_t width )
    {
        std::vector<unsigned char> pixels( static_cast<size_t>( resolution ) * resolution * 4u, 0u );

        for ( uint32_t y = 0; y < resolution; ++y )
            for ( uint32_t k = 0; k < width; ++k )
            {
                const uint32_t x  = ( firstColumn + k ) % resolution;
                const size_t   at = ( static_cast<size_t>( y ) * resolution + x ) * 4u;
                pixels[at + 0]    = 255u;
                pixels[at + 1]    = 255u;
                pixels[at + 2]    = 255u;
                pixels[at + 3]    = 128u;
            }

        const uint32_t channels[kCloudLayoutChannels] = { 0u, 1u, 2u, 3u };

        CloudLayoutCanvas canvas;
        if ( !SetCloudLayoutCanvasPatternFromImage( canvas, pixels, resolution, resolution, channels ) )
            return CloudLayoutData{};

        auto made = MakeCloudLayoutFromCanvas( canvas );
        return made ? made.ExtractValue() : CloudLayoutData{};
    }
} // namespace

// WHICH WAY UP A PAINTING GOES INTO THE WORLD, pinned because the panel now STATES it to the artist and a
// statement nobody checks is a caption. The layout's v axis runs NORTH and its first row sits at the world
// origin — which together are why the panel's north-up map is the artist's picture mirrored top to bottom,
// and why Layout Offset is the control that slides a figure into the middle of a region.
TEST( CloudPlacementSpectrum, ThePaintingsFirstRowSitsAtTheOriginAndItsRowsRunNorth )
{
    CloudLayoutPlacement placement;
    placement.RepeatsPerRegion = 1u;
    placement.QuarterTurns     = 0u;
    placement.OffsetKm         = glm::vec2( 0.0f );

    const float region = 48.0f;

    // The world origin is texel (0,0) exactly. Everything below is a statement about where that is.
    const glm::vec2 atOrigin = CloudLayoutUv( placement, region, glm::vec2( 0.0f, 0.0f ) );
    EXPECT_FLOAT_EQ( atOrigin.x, 0.0f );
    EXPECT_FLOAT_EQ( atOrigin.y, 0.0f );

    // A band on the first four rows. If v ran SOUTH it would be found north of the origin instead.
    auto north = BandLayout( 64u, /*firstRow=*/1u, /*rows=*/3u );
    ASSERT_TRUE( north );

    const float period    = region; // one repeat
    const float texelKm   = period / 64.0f;
    const float insideKm  = 2.0f * texelKm; // the middle of the band
    const float outsideKm = -2.0f * texelKm;

    const float toTheNorth =
         SampleCloudLayoutPattern( *north, 0u, CloudLayoutUv( placement, region, glm::vec2( 0.0f, insideKm ) ) );
    const float toTheSouth =
         SampleCloudLayoutPattern( *north, 0u, CloudLayoutUv( placement, region, glm::vec2( 0.0f, outsideKm ) ) );

    EXPECT_GT( toTheNorth, 0.9f ) << "rows 1..3 of the painting were not found just NORTH of the origin, so "
                                     "the layout's v axis does not run north and the panel's caption under "
                                     "the two panes is wrong";
    EXPECT_LT( toTheSouth, 0.1f ) << "the band was found on BOTH sides of the origin, so this measures "
                                     "nothing about direction";

    // And the same claim on the other axis, so that a swap of u and v cannot pass the test above.
    const float toTheEast =
         SampleCloudLayoutPattern( *north, 0u, CloudLayoutUv( placement, region, glm::vec2( insideKm, 0.0f ) ) );
    EXPECT_LT( toTheEast, 0.1f ) << "a band drawn across the painting's ROWS was found by walking east, so "
                                    "u and v are swapped somewhere between here and the sampler";
}

// THE MAP IS THE BAKE'S OWN ANSWER, cell for cell. This is the relation that makes the panel a preview
// rather than a second opinion: BuildCloudLayoutPreview must contain no arithmetic about coverage of its
// own, only the sampling grid.
TEST( CloudPlacementSpectrum, TheMapAgreesWithTheBakesOwnCoverageCellForCell )
{
    // BOTH ROTATIONS, AND A FIELD WITH NO FLAT PLACES IN IT. Neither is thoroughness; both are what make
    // the test able to fail. Against a STRIPE the map's u could be displaced by four tenths of a cell and
    // all 256 cells still came back identical — at a quarter turn the stripe varies only with world y, and
    // even at zero turns no cell centre ever crossed its single hard edge. That sabotage was measured
    // GREEN, which is a hole and not a pass. A ripple varies everywhere, and the two rotations put that
    // variation on both axes in turn.
    for ( const uint32_t turns : { 0u, 1u } )
    {
        CloudProceduralFieldParams params       = ShippedParams();
        params.Coverage                         = 0.50f;
        params.PatternSource                    = RippleLayout();
        params.LayoutPlacement.RepeatsPerRegion = 2u;
        params.LayoutPlacement.QuarterTurns     = turns;
        params.LayoutPlacement.OffsetKm         = glm::vec2( 3.5f, -1.25f );
        params.LayoutPlacement.PatternStrength  = 0.35f;
        params.LayoutPlacement.MaskStrength     = 0.0f;
        ASSERT_TRUE( params.PatternSource );

        const float spanKm = params.RegionSizeKm;

        auto mapped = BuildCloudLayoutPreview( params, 0u, spanKm, 256u );
        ASSERT_TRUE( mapped ) << mapped.GetError();

        const CloudLayoutPreview& preview = mapped.GetValue();
        ASSERT_GT( preview.Side, 1u );

        double sum = 0.0;
        for ( uint32_t iv = 0; iv < preview.Side; ++iv )
            for ( uint32_t iu = 0; iu < preview.Side; ++iu )
            {
                const glm::vec2 centre(
                     ( ( static_cast<float>( iu ) + 0.5f ) / static_cast<float>( preview.Side ) - 0.5f ) * spanKm,
                     ( ( static_cast<float>( iv ) + 0.5f ) / static_cast<float>( preview.Side ) - 0.5f ) *
                          spanKm );

                const float fromTheBake = CloudProceduralCellCoverage( params, 0u, centre );
                const float fromTheMap  = preview.Coverage[static_cast<size_t>( iv ) * preview.Side + iu];

                ASSERT_FLOAT_EQ( fromTheMap, fromTheBake )
                     << "at " << turns << " quarter turns the map and the function the bake calls disagree "
                     << "at cell (" << iu << ", " << iv
                     << "), so the picture an artist judges a painting by is not the sky they will get";
                sum += static_cast<double>( fromTheBake );
            }

        EXPECT_NEAR( preview.MeanCoverage,
                     static_cast<float>( sum / ( static_cast<double>( preview.Side ) * preview.Side ) ), 1e-5f );
    }
}

// EVERY SPECIES ON ITS OWN LATTICE. The first draft of the panel drew every channel's map on the FINEST
// cell in the layer, which shows detail a coarse species cannot place and quotes the most permissive
// legibility bound in the layer for a channel that has to clear its own.
TEST( CloudPlacementSpectrum, TheMapIsSampledOnTheMappedSpeciesOwnCellAndNotTheLayersFinest )
{
    CloudProceduralFieldParams params = ShippedParams();
    params.PatternSource              = StripeLayout();
    ASSERT_TRUE( params.PatternSource );

    // A second species FOUR TIMES coarser. Both ends of the same knob: mapping the fine one and mapping
    // the coarse one must not produce the same lattice.
    CloudProceduralSpecies coarse;
    coarse.Shape      = Congestus();
    coarse.CellKm     = 12.0f;
    coarse.Anisotropy = 1.0f;
    params.Species.push_back( coarse );

    // The patch tile has to clear three of the COARSEST cell or the parameters are illegal; the panel
    // floors it the same way for the same reason.
    params.PatchTileKm = 48.0f;

    const float spanKm = params.RegionSizeKm;

    auto fine = BuildCloudLayoutPreview( params, 0u, spanKm, 256u );
    ASSERT_TRUE( fine ) << fine.GetError();
    auto broad = BuildCloudLayoutPreview( params, 1u, spanKm, 256u );
    ASSERT_TRUE( broad ) << broad.GetError();

    EXPECT_FLOAT_EQ( fine.GetValue().CellKm, 3.0f );
    EXPECT_FLOAT_EQ( broad.GetValue().CellKm, 12.0f )
         << "the coarse species' map quoted a cell that is not its own, so an artist is told a stroke "
            "clears a lattice it does not clear";

    EXPECT_EQ( fine.GetValue().Side, 16u );
    EXPECT_EQ( broad.GetValue().Side, 4u )
         << "the coarse species was mapped at the fine one's resolution, so the picture shows structure the "
            "sky cannot place";

    EXPECT_FLOAT_EQ( fine.GetValue().SamplePitchKm, 3.0f );
    EXPECT_FLOAT_EQ( broad.GetValue().SamplePitchKm, 12.0f );
}

// §PT'S BYTE-IDENTICAL PAIR, AS A RELATION. Two frames across the whole pattern slider came back identical
// because the mask alone had driven the figure past both ends of the clamp. The panel says so out loud, and
// what it says has to be true in both directions or it is a superstition with a percentage attached.
TEST( CloudPlacementSpectrum, ThePatternSliderIsDeadExactlyWhenTheMaskHasSaturatedTheClamp )
{
    CloudProceduralFieldParams params      = ShippedParams();
    params.Coverage                        = 0.50f;
    // BOTH INPUTS FROM ONE PAINTING, which is how a `.dclayout` carrying both tables is normally bound
    // and the only arrangement in which the pattern and the mask describe the same figure.
    const std::shared_ptr<const CloudLayoutData> figure = StripeLayout( 64u, /*withMask=*/true );
    ASSERT_TRUE( figure );
    params.PatternSource                   = figure;
    params.MaskSource                      = figure;
    params.LayoutPlacement.PatternStrength = 0.30f;

    // MASK OFF: the pattern is the only source, and it has to move the sky.
    params.LayoutPlacement.MaskStrength = 0.0f;

    auto loose = BuildCloudLayoutPreview( params, 0u, params.RegionSizeKm, 256u );
    ASSERT_TRUE( loose ) << loose.GetError();

    EXPECT_EQ( loose.GetValue().CellsClamped, 0u )
         << "a gentle pattern on a half-covered sky pinned cells at the clamp, so the fixture cannot "
            "distinguish the two ends of this test";
    EXPECT_GT( loose.GetValue().CellsPatternMoves, 0u )
         << "with no mask at all the pattern slider moved nothing, which would make the panel's warning "
            "fire on a sky where the pattern is perfectly alive";

    // MASK AT FULL STRENGTH: the mask adds a whole unit of coverage on one side and takes one away on the
    // other, so every cell is pinned and the pattern has nowhere left to speak.
    params.LayoutPlacement.MaskStrength = 1.0f;

    auto saturated = BuildCloudLayoutPreview( params, 0u, params.RegionSizeKm, 256u );
    ASSERT_TRUE( saturated ) << saturated.GetError();

    EXPECT_EQ( saturated.GetValue().CellsClamped, saturated.GetValue().Cells )
         << "a mask at full strength on a signed stripe failed to pin the sky at both ends of the clamp, so "
            "the explanation the panel offers for a dead slider is not the mechanism";
    EXPECT_EQ( saturated.GetValue().CellsPatternMoves, 0u )
         << "the pattern slider still moved cells under a saturating mask, so §PT's byte-identical pair of "
            "frames had some other cause and the panel names the wrong one";
}

// THE STROKE MEASURE IS A RULER, and a ruler is tested against something whose width is known before it is
// measured. A bar is that thing.
TEST( CloudPlacementSpectrum, TheStrokeMeasureReportsTheWidthOfABarAtBothEnds )
{
    for ( const uint32_t width : { 4u, 16u } )
    {
        const CloudLayoutData bar = BarLayout( 64u, /*firstColumn=*/8u, width );
        ASSERT_TRUE( bar.HasPattern() );

        const CloudLayoutStrokeStats stats = MeasureCloudLayoutStrokes( bar, 0u, /*limitTexels=*/0.0f );

        EXPECT_EQ( stats.PaintedTexels, static_cast<uint64_t>( width ) * 64u )
             << "the painted area of a " << width << "-texel bar was not counted";
        EXPECT_FLOAT_EQ( stats.MedianTexels, static_cast<float>( width ) )
             << "a bar " << width << " texels wide measured " << stats.MedianTexels;
        EXPECT_FLOAT_EQ( stats.ThinnestTenthTexels, static_cast<float>( width ) );
    }

    // A CHANNEL NOBODY DREW ON HAS NO STROKES, which must not be read as infinitely thin ones.
    const CloudLayoutData flat = BarLayout( 64u, 0u, 64u );
    ASSERT_TRUE( flat.HasPattern() );
    EXPECT_EQ( MeasureCloudLayoutStrokes( flat, 0u, 0.0f ).PaintedTexels, 0u )
         << "a channel with the same value everywhere reported strokes, so a flat slot would be given a "
            "legibility verdict about nothing";
}

// THE RUNS WRAP, because the painting tiles the world. A band that leaves the right edge and returns at the
// left is ONE stroke; measured as two it would report half its real width and the panel would warn about a
// figure that is perfectly legible.
TEST( CloudPlacementSpectrum, TheStrokeMeasureJoinsABarThatStraddlesThePaintingsEdge )
{
    const CloudLayoutData straddling = BarLayout( 64u, /*firstColumn=*/62u, /*width=*/8u );
    ASSERT_TRUE( straddling.HasPattern() );

    const CloudLayoutStrokeStats stats = MeasureCloudLayoutStrokes( straddling, 0u, 0.0f );

    EXPECT_FLOAT_EQ( stats.MedianTexels, 8.0f )
         << "a bar split across the edge measured " << stats.MedianTexels
         << " texels, so the runs are not wrapping and every figure that touches an edge is slandered";
}

// AND THE FRACTION AN ARTIST ACTS ON MOVES WITH THE LIMIT, in both directions. A number that answered the
// same either side of the cell would be the dead setting §1.3 forbids wearing a percentage.
TEST( CloudPlacementSpectrum, TheStrokeFractionBelowTheLimitAnswersOnBothSidesOfTheCell )
{
    const CloudLayoutData bar = BarLayout( 64u, 8u, /*width=*/4u );
    ASSERT_TRUE( bar.HasPattern() );

    EXPECT_FLOAT_EQ( MeasureCloudLayoutStrokes( bar, 0u, /*limitTexels=*/5.0f ).FractionBelowLimit, 1.0f )
         << "a 4-texel stroke was not counted as finer than a 5-texel cell";
    EXPECT_FLOAT_EQ( MeasureCloudLayoutStrokes( bar, 0u, /*limitTexels=*/3.0f ).FractionBelowLimit, 0.0f )
         << "a 4-texel stroke was counted as finer than a 3-texel cell";

    // A limit of nothing means nothing is below it — the state the panel is in before a layer is read.
    EXPECT_FLOAT_EQ( MeasureCloudLayoutStrokes( bar, 0u, 0.0f ).FractionBelowLimit, 0.0f );
}

// =====================================================================================================
// THE BRUSH
//
// Every test below asserts a RELATION between the brush and something that already existed, because the
// brush's whole value is that a painted stroke and the sky agree about how wide it is. A test of the
// brush alone would say the pixels are the pixels.
// =====================================================================================================

namespace
{
    /// One straight horizontal stroke across a canvas, painted with the engine's own brush and turned into
    /// a layout by the very function the panel and the command-line tool call. Nothing here fills a struct
    /// by hand: what is measured is the layout a FILE would carry.
    CloudLayoutData BrushedBar( uint32_t side, uint32_t channel, const CloudLayoutBrush& brush )
    {
        auto canvas = MakeCloudLayoutCanvas( side );
        EXPECT_TRUE( canvas ) << ( canvas ? "" : canvas.GetError() );
        if ( !canvas )
            return {};

        CloudLayoutCanvas surface = canvas.ExtractValue();

        const float     middle = static_cast<float>( side ) * 0.5f;
        const glm::vec2 from( -2.0f, middle );
        const glm::vec2 to( static_cast<float>( side ) + 2.0f, middle );

        const auto painted =
             PaintCloudLayoutPolyline( surface.Pattern, side, channel, kCloudLayoutChannels, { from, to }, brush );
        EXPECT_TRUE( painted ) << ( painted ? "" : painted.GetError() );

        auto made = MakeCloudLayoutFromCanvas( surface );
        EXPECT_TRUE( made ) << ( made ? "" : made.GetError() );
        return made ? made.ExtractValue() : CloudLayoutData{};
    }
} // namespace

// THE ONE RELATION THE PANEL'S ADVICE RESTS ON. It tells an artist "this stroke is 4.9 km wide, the cell
// is 3.0 km, you are fine" — and it computes that from CloudLayoutBrushWidthTexels while the verdict
// underneath measures the pixels with MeasureCloudLayoutStrokes. If those two ever disagree, the panel is
// confidently wrong about the only question it exists to answer.
TEST( CloudPlacementSpectrum, TheBrushLaysExactlyTheStrokeWidthItPromises )
{
    constexpr uint32_t kSide = 128u;

    for ( const float radius : { 4.0f, 9.0f, 21.0f } )
    {
        CloudLayoutBrush brush;
        brush.RadiusTexels = radius;
        brush.Hardness     = 1.0f;

        const CloudLayoutData bar = BrushedBar( kSide, 0u, brush );
        ASSERT_TRUE( bar.HasPattern() );

        const CloudLayoutStrokeStats stats = MeasureCloudLayoutStrokes( bar, 0u, /*limitTexels=*/0.0f );

        EXPECT_FLOAT_EQ( stats.MedianTexels, CloudLayoutBrushWidthTexels( brush ) )
             << "a hard brush of radius " << radius << " promised " << CloudLayoutBrushWidthTexels( brush )
             << " texels and measured " << stats.MedianTexels;
    }

    // A SOFTER BRUSH IS A NARROWER STROKE, and the promise has to follow it rather than quoting the
    // diameter regardless. Within a texel, because the profile crosses the measure's midpoint between two
    // rows and a canvas has no half-rows.
    for ( const float hardness : { 0.0f, 0.5f } )
    {
        CloudLayoutBrush brush;
        brush.RadiusTexels = 20.0f;
        brush.Hardness     = hardness;

        const CloudLayoutData bar = BrushedBar( kSide, 0u, brush );
        ASSERT_TRUE( bar.HasPattern() );

        EXPECT_NEAR( MeasureCloudLayoutStrokes( bar, 0u, 0.0f ).MedianTexels, CloudLayoutBrushWidthTexels( brush ),
                     1.0f )
             << "at hardness " << hardness << " the promise and the ruler parted company";
    }

    // AND THE INK DOES NOT MOVE THE WIDTH. A fainter stroke is not a thinner one — the measure thresholds
    // at the channel's OWN midpoint, and if that ever became a fixed 0.5 this test goes red instead of a
    // panel quietly warning about every soft painting anybody makes.
    CloudLayoutBrush faint;
    faint.RadiusTexels = 12.0f;
    faint.Hardness     = 1.0f;
    faint.Ink          = 0.35f;

    EXPECT_FLOAT_EQ( MeasureCloudLayoutStrokes( BrushedBar( kSide, 0u, faint ), 0u, 0.0f ).MedianTexels,
                     CloudLayoutBrushWidthTexels( faint ) )
         << "a stroke at a third of the ink measured a different width from the same stroke at full ink";
}

// THE PANEL'S GREEN AND AMBER, AS A RELATION. It goes green when the promised width clears the cell and
// amber when it does not; the verdict underneath reports the fraction of painted texels below that same
// limit. The two must answer the same way about the same stroke, or the artist is warned and reassured at
// once by one panel.
TEST( CloudPlacementSpectrum, TheBrushsPromiseAndTheLegibilityVerdictAgreeAboutTheCell )
{
    constexpr uint32_t kSide       = 128u;
    constexpr float    kCellTexels = 16.0f;

    CloudLayoutBrush wide;
    wide.RadiusTexels = 12.0f; // 24 texels, wider than the cell
    wide.Hardness     = 1.0f;

    CloudLayoutBrush thin;
    thin.RadiusTexels = 3.0f; // 6 texels, well under it
    thin.Hardness     = 1.0f;

    ASSERT_GT( CloudLayoutBrushWidthTexels( wide ), kCellTexels );
    ASSERT_LT( CloudLayoutBrushWidthTexels( thin ), kCellTexels );

    EXPECT_FLOAT_EQ(
         MeasureCloudLayoutStrokes( BrushedBar( kSide, 0u, wide ), 0u, kCellTexels ).FractionBelowLimit, 0.0f )
         << "the panel would go green on a stroke the ruler calls finer than a cell";

    EXPECT_FLOAT_EQ(
         MeasureCloudLayoutStrokes( BrushedBar( kSide, 0u, thin ), 0u, kCellTexels ).FractionBelowLimit, 1.0f )
         << "the panel would warn about a stroke the ruler calls wide enough";
}

// A STROKE IS LAID ONCE, AT ITS STRONGEST, so the same drag sampled at two different frame rates has to
// produce the identical canvas. Without that the width promised above would be a function of how fast the
// artist's hand moved and of how busy the machine was — and the same figure would come out different on a
// laptop and on a workstation.
TEST( CloudPlacementSpectrum, AStrokeIsTheSameWhateverRateTheMouseWasSampledAt )
{
    constexpr uint32_t kSide = 96u;

    CloudLayoutBrush brush;
    brush.RadiusTexels = 7.0f;
    brush.Hardness     = 0.4f; // soft, where a build-up brush would differ most

    const glm::vec2 from( 8.0f, 20.0f );
    const glm::vec2 to( 84.0f, 70.0f );

    auto coarse = MakeCloudLayoutCanvas( kSide );
    auto fine   = MakeCloudLayoutCanvas( kSide );
    ASSERT_TRUE( coarse );
    ASSERT_TRUE( fine );

    std::vector<unsigned char> coarsePixels = coarse.GetValue().Pattern;
    std::vector<unsigned char> finePixels   = fine.GetValue().Pattern;

    ASSERT_TRUE( PaintCloudLayoutPolyline( coarsePixels, kSide, 0u, kCloudLayoutChannels, { from, to }, brush ) );

    std::vector<glm::vec2> many;
    for ( int step = 0; step <= 40; ++step )
    {
        const float t = static_cast<float>( step ) / 40.0f;
        many.emplace_back( from + ( to - from ) * t );
    }
    ASSERT_TRUE( PaintCloudLayoutPolyline( finePixels, kSide, 0u, kCloudLayoutChannels, many, brush ) );

    EXPECT_EQ( coarsePixels, finePixels )
         << "the same drag came out differently when it was sampled forty times instead of once, so the "
            "stroke accumulates and its width depends on the frame rate";
}

// THE FOOTPRINT WRAPS, because the painting tiles the world and everything past the region is a REPEAT
// sample of it. A stroke laid on the seam that did not come out of the other edge would put a hard
// discontinuity across every region face — the one defect the whole `.dclayout` design exists to avoid,
// and the reason Layout Repeats is an integer.
TEST( CloudPlacementSpectrum, ABrushStrokeOnTheSeamComesOutOfTheOtherEdge )
{
    constexpr uint32_t kSide = 64u;

    CloudLayoutBrush brush;
    brush.RadiusTexels = 6.0f;
    brush.Hardness     = 1.0f;

    auto canvas = MakeCloudLayoutCanvas( kSide );
    ASSERT_TRUE( canvas );
    CloudLayoutCanvas surface = canvas.ExtractValue();

    // Straight down the left-hand seam, so half the stroke belongs to the right-hand edge.
    ASSERT_TRUE( PaintCloudLayoutPolyline(
         surface.Pattern, kSide, 0u, kCloudLayoutChannels,
         { glm::vec2( 0.0f, -4.0f ), glm::vec2( 0.0f, static_cast<float>( kSide ) + 4.0f ) }, brush ) );

    EXPECT_GT( surface.Pattern[( 32u * kSide + 0u ) * 4u], 200u ) << "the seam column itself was not painted";
    EXPECT_GT( surface.Pattern[( 32u * kSide + ( kSide - 1u ) ) * 4u], 200u )
         << "the stroke stopped at the edge instead of wrapping, so a figure on the seam is cut in half at "
            "every region face";

    auto made = MakeCloudLayoutFromCanvas( surface );
    ASSERT_TRUE( made ) << made.GetError();

    EXPECT_FLOAT_EQ( MeasureCloudLayoutStrokes( made.GetValue(), 0u, 0.0f ).MedianTexels,
                     CloudLayoutBrushWidthTexels( brush ) )
         << "a stroke on the seam measured as two half-strokes, so the wrap and the ruler disagree";
}

// A BLANK CANVAS SAYS NOTHING OF ITS OWN — and that turns out to be a different statement from "leaves
// the sky where it was", which is what this test asserted on its first run and what the run disproved.
//
// ⚠️ THE DISPROVED VERSION IS WORTH MORE THAN THE ONE THAT PASSED. The first form compared the map with a
// blank canvas bound against the map with no painting at all and expected them equal. They are not, and
// nothing is wrong: binding ANY painting hands the cell's coverage to the painting and switches the
// procedural weather patch OFF, because §1.3 and §4.2 allow exactly one modulator per number
// (OnlyOneSourceModulatesACellsCoverage, above, is the test that pins it). So an artist who starts a
// blank canvas gets a FLAT sky at the slider, not the sky they were looking at — which is a real thing
// they have to be told, and the panel now tells them.
//
// What the blank canvas does guarantee is that it contributes nothing itself: a flat pattern is exactly
// its own mean and subtracts to zero, and an alpha at NEUTRAL adds exactly zero. Both are asserted here
// exactly rather than approximately, because both are exact.
TEST( CloudPlacementSpectrum, ABlankCanvasIsExactlyTheCoverageSliderAndSwitchesTheWeatherPatchOff )
{
    auto canvas = MakeCloudLayoutCanvas( 64u );
    ASSERT_TRUE( canvas );

    EXPECT_FALSE( canvas.GetValue().HasMask() )
         << "a blank canvas came with a mask plane, so every new painting would carry a table that does "
            "nothing and an empty slot would not be free";

    CloudLayoutCanvas surface = canvas.ExtractValue();

    // A MASK IS ADDED ON PURPOSE: the NEUTRAL mask is the interesting case, because a mask flooded with
    // opaque white here would add a whole unit of coverage to every cell in the sky. SetCloudLayoutCanvasMask
    // is what an artist presses, and what it lays down is the value that does nothing.
    ASSERT_TRUE( SetCloudLayoutCanvasMask( surface, true ) );

    auto made = MakeCloudLayoutFromCanvas( surface );
    ASSERT_TRUE( made ) << made.GetError();
    ASSERT_TRUE( made.GetValue().HasMask() );

    CloudProceduralFieldParams painted = ShippedParams();
    painted.Coverage                   = 0.50f;

    auto blank            = std::make_shared<const CloudLayoutData>( made.ExtractValue() );
    painted.PatternSource = blank;
    painted.MaskSource    = blank;

    auto withCanvas = BuildCloudLayoutPreview( painted, 0u, painted.RegionSizeKm, kMapSide );
    ASSERT_TRUE( withCanvas ) << withCanvas.GetError();

    for ( const float cell : withCanvas.GetValue().Coverage )
        ASSERT_FLOAT_EQ( cell, painted.Coverage )
             << "an untouched canvas moved a cell off the slider, so a blank painting is not silent";

    // AND THE PATTERN SLIDER STILL MOVES THE SKY ON A CANVAS NOBODY DREW ON — which looks wrong and is the
    // switch above seen from the other side. CellsPatternMoves compares the two ENDS of the slider, and at
    // zero the painting stands down and the procedural weather patch comes back; so what the slider moves
    // here is not the drawing, it is which of the two sources owns the cell. Asserted rather than
    // tolerated, because the panel prints this number and somebody will otherwise "fix" it to zero.
    EXPECT_GT( withCanvas.GetValue().CellsPatternMoves, 0u )
         << "the two ends of the pattern slider agreed on a blank canvas, so turning it down no longer "
            "hands the sky back to the weather patch";

    // AND THE SKY IT REPLACED WAS NOT FLAT. Named here rather than left to be discovered: this is the
    // difference an artist sees the moment they press New Canvas.
    CloudProceduralFieldParams bare = ShippedParams();
    bare.Coverage                   = 0.50f;

    auto without = BuildCloudLayoutPreview( bare, 0u, bare.RegionSizeKm, kMapSide );
    ASSERT_TRUE( without ) << without.GetError();

    const auto& unpainted = without.GetValue().Coverage;
    ASSERT_FALSE( unpainted.empty() );
    EXPECT_NE( *std::max_element( unpainted.begin(), unpainted.end() ),
               *std::min_element( unpainted.begin(), unpainted.end() ) )
         << "the unpainted sky was already flat, so this scene cannot show that a painting replaces the "
            "procedural weather patch and the panel's warning about it is untested";
}

// A PAINTING GOES BACK ON THE CANVAS IT CAME OFF, byte for byte, or the panel's "Edit this painting" is a
// slow way to lose somebody's work. The round trip is what makes the brush an editor rather than a
// one-way bake.
TEST( CloudPlacementSpectrum, APaintedLayoutReopensAsExactlyTheCanvasItWasPaintedOn )
{
    constexpr uint32_t kSide = 64u;

    CloudLayoutBrush brush;
    brush.RadiusTexels = 5.0f;
    brush.Hardness     = 0.7f;

    auto canvas = MakeCloudLayoutCanvas( kSide );
    ASSERT_TRUE( canvas );
    CloudLayoutCanvas surface = canvas.ExtractValue();
    ASSERT_TRUE( SetCloudLayoutCanvasMask( surface, true ) );

    // ALL FIVE PLANES CARRY A FIGURE, and the mask's is a DIFFERENT one from the fourth species slot's.
    // Before O-4 that combination could not be expressed at all — the mask was the pattern's alpha, four
    // planes for five tables — and MakeCloudLayoutCanvasFromLayout refused it by name. This is the case
    // the split exists for, so it is the case the round trip is measured on.
    for ( uint32_t channel = 0; channel < kCloudLayoutChannels; ++channel )
        ASSERT_TRUE( PaintCloudLayoutPolyline( surface.Pattern, kSide, channel, kCloudLayoutChannels,
                                               { glm::vec2( 10.0f, 12.0f ), glm::vec2( 50.0f, 44.0f ) }, brush ) );

    ASSERT_TRUE( PaintCloudLayoutPolyline( surface.Mask, kSide, 0u, 1u,
                                           { glm::vec2( 50.0f, 12.0f ), glm::vec2( 10.0f, 44.0f ) }, brush ) );

    ASSERT_NE( surface.Mask[( 20u * kSide + 40u )], surface.Pattern[( 20u * kSide + 40u ) * 4u + 3u] )
         << "the fixture's mask and its fourth pattern channel agree, so it is the case that always "
            "worked and this test is measuring itself";

    auto made = MakeCloudLayoutFromCanvas( surface );
    ASSERT_TRUE( made ) << made.GetError();

    auto reopened = MakeCloudLayoutCanvasFromLayout( made.GetValue() );
    ASSERT_TRUE( reopened ) << reopened.GetError();

    EXPECT_EQ( reopened.GetValue().Side, kSide );
    EXPECT_TRUE( reopened.GetValue().HasMask() )
         << "a layout with a mask reopened as a canvas that would bake without one";
    EXPECT_EQ( reopened.GetValue().Pattern, surface.Pattern )
         << "the pattern came back different from the one the layout was painted on";
    EXPECT_EQ( reopened.GetValue().Mask, surface.Mask )
         << "the mask came back different from the one the layout was painted on";
}

// AND THE LAYOUT THAT USED TO BE REFUSED NOW OPENS WITH BOTH TABLES INTACT. This test is the inverse of
// the one it replaces: while the mask was the pattern's alpha, a `.dclayout` whose mask differed from its
// fourth channel needed five planes and a canvas had four, so it could be bound and rendered but never
// edited — MakeCloudLayoutCanvasFromLayout said so by name. O-4 gave the mask its own plane, and the
// refusal has nothing left to refuse. Kept as a test rather than deleted with the refusal, because
// "this case now works" is the deliverable and a case nobody asserts is a case that regresses quietly.
TEST( CloudPlacementSpectrum, AMaskDrawnApartFromTheFourthChannelOpensWithBothTablesIntact )
{
    constexpr uint32_t kSide  = 32u;
    const size_t       texels = static_cast<size_t>( kSide ) * kSide;

    CloudLayoutData data;
    data.Resolution = kSide;
    data.Pattern.assign( texels * 4u, 0u );
    data.Mask.assign( texels, kCloudLayoutMaskNeutral );

    // One texel where the mask and the fourth pattern channel disagree is the whole of the condition.
    data.Mask[7] = 200u;

    auto encoded = EncodeCloudLayout( data );
    ASSERT_TRUE( encoded ) << encoded.GetError();
    auto decoded = DecodeCloudLayout( encoded.GetValue() );
    ASSERT_TRUE( decoded ) << decoded.GetError();

    auto opened = MakeCloudLayoutCanvasFromLayout( decoded.GetValue() );
    ASSERT_TRUE( opened ) << opened.GetError();

    EXPECT_EQ( opened.GetValue().Pattern, decoded.GetValue().Pattern )
         << "the pattern was altered on the way onto the canvas";
    EXPECT_EQ( opened.GetValue().Mask, decoded.GetValue().Mask )
         << "the mask was altered or dropped on the way onto the canvas, which is exactly the silent "
            "loss the old refusal existed to prevent";
    EXPECT_EQ( opened.GetValue().Mask[7], 200u )
         << "the one texel that makes this layout need a fifth plane did not survive";
}

// THE ERASER IS THE SAME BRUSH WITH THE REST VALUE AS ITS INK, and the rest value differs per plane — a
// pattern channel rests at nothing, the mask rests at NEUTRAL. An eraser that returned the mask to 0
// would not clear it; it would turn it into "remove all the cloud here", which is the opposite.
TEST( CloudPlacementSpectrum, ErasingReturnsThePatternToNothingAndTheMaskToNeutral )
{
    constexpr uint32_t kSide = 48u;

    CloudLayoutBrush lay;
    lay.RadiusTexels = 8.0f;
    lay.Hardness     = 1.0f;
    lay.Ink          = 1.0f;

    auto canvas = MakeCloudLayoutCanvas( kSide );
    ASSERT_TRUE( canvas );
    CloudLayoutCanvas surface = canvas.ExtractValue();
    ASSERT_TRUE( SetCloudLayoutCanvasMask( surface, true ) );

    const std::vector<glm::vec2> path = { glm::vec2( 8.0f, 24.0f ), glm::vec2( 40.0f, 24.0f ) };

    // THE SAME STROKE ON A FOUR-CHANNEL PLANE AND ON A ONE-CHANNEL PLANE. That the brush takes a stride
    // is the other half of what this asserts: a stroke laid on the mask with the pattern's stride of four
    // would paint every fourth texel and leave a comb.
    ASSERT_TRUE( PaintCloudLayoutPolyline( surface.Pattern, kSide, 0u, kCloudLayoutChannels, path, lay ) );
    ASSERT_TRUE( PaintCloudLayoutPolyline( surface.Mask, kSide, 0u, 1u, path, lay ) );

    const size_t middle = 24u * kSide + 24u;
    ASSERT_EQ( surface.Pattern[middle * 4u], 255u );
    ASSERT_EQ( surface.Mask[middle], 255u );

    // AND NO GAPS: the neighbour texel along the stroke is painted too, which is what a wrong stride
    // would break and what a single-texel probe would miss.
    ASSERT_EQ( surface.Mask[middle + 1u], 255u ) << "the stroke on the mask skipped a texel, so the plane "
                                                    "was walked with the wrong stride";

    CloudLayoutBrush erasePattern = lay;
    erasePattern.Ink              = 0.0f;

    CloudLayoutBrush eraseMask = lay;
    eraseMask.Ink              = static_cast<float>( kCloudLayoutMaskNeutral ) / 255.0f;

    ASSERT_TRUE(
         PaintCloudLayoutPolyline( surface.Pattern, kSide, 0u, kCloudLayoutChannels, path, erasePattern ) );
    ASSERT_TRUE( PaintCloudLayoutPolyline( surface.Mask, kSide, 0u, 1u, path, eraseMask ) );

    EXPECT_EQ( surface.Pattern[middle * 4u], 0u ) << "erasing a pattern channel left cloud behind";
    EXPECT_EQ( surface.Mask[middle], kCloudLayoutMaskNeutral )
         << "erasing the mask left it at " << static_cast<int>( surface.Mask[middle] )
         << " rather than at neutral, so an erased mask still says something about the sky";
}

// THE SHELL A TYPE ASKS FOR AND THE BODY THE BAKE THEN PUTS IN IT ARE ONE STATEMENT, NOT TWO.
//
// `EveryLumpStandsInsideItsTypesOwnBand` above asserts one half of that relation — nothing pokes OUT of the
// shell. This asserts the other half, which nothing did: nothing may be inside the shell that no lump can
// reach. A shell taller than its own contents is not an error and draws no wrong pixel; it silently spends
// two budgets that are divided by it —
//
//   * `kCloudProceduralVolumeHeight` rows over the shell's thickness, so the vertical voxel grows with it;
//   * the march's step, which is `segment / MaxSteps` past CLOUD_DISTANCE_TO_MAX_STEPS_KM, so a taller
//     shell is a longer segment and a coarser SEARCH.
//
// The way to get one is an ANVIL THAT IS DECLARED AND NEVER DRAWN, and it was reachable: the envelope asked
// `AnvilStrength > 0` while the generator asked `> 1e-3` and `AnvilThicknessKm > 1e-4`, so a type could take
// the shell to its canopy's altitude and put nothing there. Both sites call Graphic::CloudTypeHasAnvil now.
//
// IT IS DRIVEN THROUGH THE GENERATOR AND NOT THROUGH THE PREDICATE, deliberately: asserting the predicate
// against itself would pass with the two sites still disagreeing, which is exactly the failure the fix is
// about. The question asked here is the one that matters — does the layer's shell contain any altitude the
// emitted lumps cannot occupy?
TEST( CloudPlacementSpectrum, TheShellHoldsNoAltitudeTheBakeCannotFill )
{
    // The strengths bracket Graphic::kCloudAnvilMinStrength: one below it (never drawn) and the shipped
    // cumulonimbus' own, which is drawn. A canopy declared at 16 km over a 0.90..9.00 km tower is what makes
    // the two answers differ by kilometres rather than by a rounding.
    const auto check = []( float anvilStrength, float anvilThicknessKm, const char* what )
    {
        CloudProceduralFieldParams params = ShippedParams();

        Desert::Graphic::CloudTypeShape& shape = params.Species[0].Shape;
        shape.BaseAltitudeKm                   = 0.90f;
        shape.TopAltitudeKm                    = 9.00f;
        shape.EdgeTopFraction                  = 0.12f;
        shape.AnvilAltitudeKm                  = 16.0f;
        shape.AnvilThicknessKm                 = anvilThicknessKm;
        shape.AnvilStrength                    = anvilStrength;
        params.Species[0].CellKm               = 6.0f;

        const Desert::Graphic::CloudEnvelopeKm envelope = Desert::Graphic::CloudTypeSetEnvelopeKm( &shape, 1u );

        params.LayerBottomKm    = envelope.BottomKm;
        params.LayerThicknessKm = envelope.TopKm - envelope.BottomKm;

        const glm::vec2                       origin = CloudProceduralRegionOriginKm( params, 0.0f, 0.0f );
        const std::vector<CloudModellingBlob> blobs  = GenerateCloudProceduralBlobs( params, 0u, origin );

        ASSERT_FALSE( blobs.empty() ) << what;

        float highest = -std::numeric_limits<float>::max();
        for ( const CloudModellingBlob& blob : blobs )
            highest = std::max( highest, blob.CentreKm.y + blob.RadiiKm.y );

        std::printf( "[CloudPlacementSpectrum] %s: shell %.2f..%.2f km, tallest lump reaches %.3f km\n", what,
                     envelope.BottomKm, envelope.TopKm, highest );

        // ONE VOXEL OF THE SHELL IS THE TOLERANCE, the same tolerance the containment test above uses and
        // for the same reason: a row of the volume is the finest altitude the shell can distinguish, so a
        // ceiling within one row of the tallest body is a ceiling that costs nothing.
        const float toleranceKm = params.LayerThicknessKm / static_cast<float>( kCloudProceduralVolumeHeight );

        EXPECT_LE( envelope.TopKm, highest + toleranceKm )
             << what << ": the shell reaches " << envelope.TopKm << " km and the tallest lump only " << highest
             << " km, so " << ( envelope.TopKm - highest )
             << " km of shell holds nothing — the volume's rows and the march's step are divided by it "
                "for no cloud at all";
    };

    // The case the fix is about. Below the generator's strength threshold the canopy is never emitted, so a
    // shell taken to 16 km is 7 km of guaranteed-empty air. Before Graphic::CloudTypeHasAnvil existed the
    // envelope answered 16.00 km here and the tallest lump 9.00 km.
    check( 0.5f * Desert::Graphic::kCloudAnvilMinStrength, 1.80f, "an anvil under the generator's strength" );

    // And the same for a canopy whose thickness is under the generator's floor.
    check( 0.85f, 0.5f * Desert::Graphic::kCloudAnvilMinThicknessKm, "an anvil under the generator's thickness" );

    // THE NEGATIVE CONTROL, and it is what stops the assertion above from being satisfied by a predicate
    // that simply always says "no anvil": the shipped cumulonimbus' own numbers DO reach the canopy, so the
    // shell must still be taken up to it.
    check( 0.85f, 1.80f, "the shipped cumulonimbus' canopy" );
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
