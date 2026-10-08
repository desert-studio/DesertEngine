// The procedural MODELLING VOLUME — phase Э5's producer, and the suite that holds it to the four
// properties the phase is judged on.
//
// WHAT IS UNDER TEST. Engine/Assets/CloudProceduralVolume.cpp: the hash that puts lumps in the sky, the
// bake that turns them into a camera-centric periodic volume, and the three relations it is obliged to
// keep — that no lump is thinner than the march can find, that no lump is NARROWER THAN THE VOLUME CAN
// CARRY (the other half of the same statement, and the half nothing asserted until Р22), and that nothing
// inside the region moves when the region scrolls.
//
// EACH TEST HERE WAS VERIFIED BY BREAKING THE THING IT CLAIMS TO MEASURE, and the record of which breaks
// turned it red is in Docs/Clouds/CALIBRATION.md §E5. A break that changes nothing is a hole in the suite,
// not luck.

#include <Engine/Assets/CloudProceduralVolume.hpp>

#include "CloudProceduralScheduleReference.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <functional>
#include <chrono>
#include <cmath>
#include <limits>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

using Desert::Assets::BakeCloudProceduralVolume;
using Desert::Assets::CloudModellingBlob;
using Desert::Assets::CloudModellingBlobDistanceKm;
using Desert::Assets::CloudModellingJoinKm;
using Desert::Assets::CloudModellingJoinTerm;
using Desert::Assets::CloudModellingPreparedBlob;
using Desert::Assets::CloudProceduralColumnKept;
using Desert::Assets::CloudProceduralFieldParams;
using Desert::Assets::CloudProceduralLocalWeathers;
using Desert::Assets::CloudProceduralLump;
using Desert::Assets::CloudProceduralLumpSet;
using Desert::Assets::CloudProceduralRegionOriginKm;
using Desert::Assets::CloudProceduralSnapKm;
using Desert::Assets::CloudProceduralSpecies;
using Desert::Assets::CloudProceduralVoxelBytes;
using Desert::Assets::EvaluateCloudProceduralProfile;
using Desert::Assets::GenerateCloudProceduralBlobs;
using Desert::Assets::GenerateCloudProceduralLumps;
using Desert::Assets::kCloudProceduralVolumeHeight;
using Desert::Assets::kCloudProceduralVolumeSide;
using Desert::Assets::kCloudProceduralVolumeSideMin;
using Desert::Assets::PrepareCloudModellingBlob;
using Desert::Assets::SortCloudModellingBlobs;
using Desert::Assets::ValidateCloudProceduralParams;

namespace
{
    // The component's own Max Steps. The library and the generator are both calibrated against the value
    // the component SHIPS with, so it is stated once here and used by every relation below.
    constexpr float kComponentMaxSteps = 256.0f;

    float ResolvableChordKm()
    {
        return Desert::Tests::CloudProceduralScheduleRef::CloudFinestResolvableChordKm( kComponentMaxSteps );
    }

    /// A cumulus mediocris in one slot: the ordinary sky, and the case every test below starts from.
    CloudProceduralFieldParams MakeParams()
    {
        CloudProceduralFieldParams params;
        params.RegionSizeKm      = 48.0f;
        params.LayerBottomKm     = 1.5f;
        params.LayerThicknessKm  = 3.5f;
        params.BlendRadiusKm     = 0.06f;
        params.Coverage          = 0.35f;
        params.CoverageContrast  = 1.0f;
        params.Seed              = 7u;
        params.WindAxis          = glm::vec2( 1.0f, 0.25f );
        params.ResolvableChordKm = ResolvableChordKm();

        CloudProceduralSpecies species;
        species.CellKm                    = 3.0f;
        species.Anisotropy                = 1.0f;
        species.Shape.BaseAltitudeKm      = 1.8f;
        species.Shape.TopAltitudeKm       = 3.4f;
        species.Shape.EdgeTopFraction     = 0.35f;
        species.Shape.BaseRampFraction    = 0.25f;
        species.Shape.Profile             = Desert::Graphic::CloudProfileFromTaper( 0.45f );
        species.Shape.AnvilAltitudeKm     = 0.0f;
        species.Shape.AnvilThicknessKm    = 0.0f;
        species.Shape.AnvilStrength       = 0.0f;
        species.Shape.DetailCharacter     = 1.0f;
        species.Shape.DetailFactor        = 1.0f;
        species.Shape.DensityFactor       = 1.0f;
        species.Shape.ExtinctionFactor    = 1.0f;
        species.Shape.PlacementScale      = 1.0f;
        species.Shape.PlacementAnisotropy = 1.0f;

        params.Species.push_back( species );
        return params;
    }

    /// The join, evaluated by GATHERING every lump at one point — the form phase Э4's sculpted bake uses,
    /// written here over the WHOLE list so that the bake's binned subset can be held against it.
    ///
    /// It is deliberately the slow, obvious implementation: a reference that shares the bake's own culling
    /// would agree with it about a mistake.
    ///
    /// `shape` is the silhouette noise at the point (CloudProceduralShapeNoise, SHAPE-NOISE): every lump's
    /// distance moves by its own reach times it BEFORE the join, as in the bake and the preview — the body a
    /// voxel must lie in is the displaced one.
    float ReferenceProfile( const std::vector<CloudModellingBlob>& blobs, const glm::vec3& pointKm,
                            float blendRadiusKm, float profileDepthKm, float regionSizeKm, float shape )
    {
        std::vector<CloudModellingPreparedBlob> prepared;
        std::vector<float>                      reaches;
        std::vector<float>                      distances;

        // THE WRAPS ARE PART OF THE FIELD, not part of the bake's optimisation: the volume is periodic by
        // construction, so the reference has to be periodic too or it would be measuring a different
        // function. Three by three covers every copy that can reach a point inside the region.
        for ( const CloudModellingBlob& blob : blobs )
        {
            for ( int wz = -1; wz <= 1; ++wz )
            {
                for ( int wx = -1; wx <= 1; ++wx )
                {
                    CloudModellingBlob shifted = blob;
                    shifted.CentreKm.x += static_cast<float>( wx ) * regionSizeKm;
                    shifted.CentreKm.z += static_cast<float>( wz ) * regionSizeKm;
                    prepared.push_back( PrepareCloudModellingBlob( shifted ) );
                    reaches.push_back( CloudProceduralShapeReachKm( blob ) );
                }
            }
        }

        float nearest = 0.0f;
        for ( size_t k = 0; k < prepared.size(); ++k )
        {
            const float distance = CloudModellingBlobDistanceKm( prepared[k], pointKm ) + reaches[k] * shape;
            distances.push_back( distance );
            nearest = ( k == 0 ) ? distance : std::min( nearest, distance );
        }

        if ( distances.empty() )
            return 0.0f;

        const float invBlend = 1.0f / blendRadiusKm;

        float sum = 0.0f;
        for ( size_t k = 0; k < distances.size(); ++k )
            sum += CloudModellingJoinTerm( prepared[k].Weight, distances[k], nearest, invBlend );

        const float joined = CloudModellingJoinKm( nearest, sum, blendRadiusKm );
        return std::clamp( -joined / profileDepthKm, 0.0f, 1.0f );
    }

    size_t VoxelIndex( uint32_t x, uint32_t y, uint32_t z )
    {
        return ( ( static_cast<size_t>( z ) * kCloudProceduralVolumeHeight + y ) * kCloudProceduralVolumeSide +
                 x ) *
               4u;
    }
} // namespace

// ---------------------------------------------------------------------------------------------------
// 1. THE GENERATOR IS A PURE FUNCTION
// ---------------------------------------------------------------------------------------------------
//
// A sky that is not reproducible cannot be compared between two builds, cached, or debugged from a
// screenshot — and every measurement this phase reports assumes that a re-run gives the same frame.
TEST( CloudProceduralField, TheGeneratorIsAPureFunctionOfItsInputs )
{
    const CloudProceduralFieldParams params = MakeParams();
    const glm::vec2                  origin = CloudProceduralRegionOriginKm( params, 0.0f, 0.0f );

    const std::vector<CloudModellingBlob> first  = GenerateCloudProceduralBlobs( params, 0u, origin );
    const std::vector<CloudModellingBlob> second = GenerateCloudProceduralBlobs( params, 0u, origin );

    ASSERT_FALSE( first.empty() ) << "an ordinary sky produced no lumps at all";
    ASSERT_EQ( first.size(), second.size() );

    for ( size_t i = 0; i < first.size(); ++i )
    {
        EXPECT_EQ( first[i].CentreKm, second[i].CentreKm ) << "lump " << i << " moved between two calls";
        EXPECT_EQ( first[i].RadiiKm, second[i].RadiiKm ) << "lump " << i << " changed size between two calls";
        EXPECT_EQ( first[i].DetailType, second[i].DetailType );
        EXPECT_EQ( first[i].DensityScale, second[i].DensityScale );
    }

    // AND THE SEED REACHES IT. A generator that ignored its seed would pass the loop above perfectly.
    CloudProceduralFieldParams reseeded = params;
    reseeded.Seed                       = params.Seed + 1u;

    const std::vector<CloudModellingBlob> other = GenerateCloudProceduralBlobs( reseeded, 0u, origin );

    bool differs = other.size() != first.size();
    for ( size_t i = 0; !differs && i < first.size(); ++i )
        differs = ( first[i].CentreKm != other[i].CentreKm );

    EXPECT_TRUE( differs ) << "a different seed produced the identical sky, so the seed is not read";

    std::printf( "[CloudProceduralField] one 48 km region of one species holds %zu lumps\n", first.size() );
}

// ---------------------------------------------------------------------------------------------------
// 2. THE UNION IS ORDER-INDEPENDENT, AND THE BAKE AGREES WITH THE GATHER
// ---------------------------------------------------------------------------------------------------
//
// TWO PROPERTIES IN ONE TEST because they are one statement: the bake walks a BINNED SUBSET of the lumps
// in whatever order the lattice emitted them, and it must give the answer a gather over the whole list in
// canonical order gives. If the bake's culling were too tight, or the bin lists lost the canonical order,
// or the join were order-sensitive, this is where it shows.
namespace
{
    /// The lumps grouped by cluster (exact site), each group in the canonical order, keeping only the clusters
    /// whose rank is under @p cover (2 keeps them all).
    std::vector<std::vector<CloudModellingBlob>> ClustersOf( const std::vector<CloudProceduralLump>& lumps,
                                                             float                                   cover )
    {
        std::vector<glm::vec2>                       sites;
        std::vector<std::vector<CloudModellingBlob>> groups;
        for ( const CloudProceduralLump& lump : lumps )
        {
            // A CLUSTER IS BAKED WHEN THE SLIDER IS OVER ITS RANK (CloudProceduralClusterReach > 0).
            if ( cover <= 1.0f && !( lump.Rank < cover ) )
                continue;
            const auto found = std::find( sites.begin(), sites.end(), lump.ClusterKm );
            if ( found == sites.end() )
            {
                sites.push_back( lump.ClusterKm );
                groups.emplace_back();
                groups.back().push_back( lump.Blob );
            }
            else
                groups[static_cast<size_t>( found - sites.begin() )].push_back( lump.Blob );
        }
        for ( std::vector<CloudModellingBlob>& group : groups )
            SortCloudModellingBlobs( group );
        return groups;
    }

    float ClusteredProfile( const std::vector<std::vector<CloudModellingBlob>>& clusters, const glm::vec3& pointKm,
                            const CloudProceduralFieldParams& params )
    {
        const float shape = CloudProceduralShapeNoise( params, 0u, pointKm );
        float       best  = 0.0f;
        for ( const std::vector<CloudModellingBlob>& cluster : clusters )
        {
            // PROFILE-BODY: normalised by the cluster's own body depth, the deepest lump floored at two voxels.
            float deepestKm = 0.0f;
            for ( const CloudModellingBlob& blob : cluster )
                deepestKm = std::max( deepestKm, CloudProceduralLumpDepthKm( blob ) );
            best = std::max( best, ReferenceProfile( cluster, pointKm, params.BlendRadiusKm,
                                                     CloudProceduralBodyDepthKm( params, deepestKm ),
                                                     params.RegionSizeKm, shape ) );
        }
        return best;
    }
} // namespace

// A DEAD NEIGHBOUR LEAVES NO VOXELS AT A LIVING CLUSTER (JOIN-PER-CLUSTER, CLOUD-AUDIT S6). The smooth minimum
// over every lump of a species bridged neighbouring clusters, and when one of them was cut by the cover the
// march kept the living one's half of the bridge — cut flat on the bisector. With the join per cluster, every
// voxel the cut keeps lies inside a body of a living cluster on its own.
//
// MUTATION: make the bake's per-cluster test `placed[column[j]].Cluster == cluster` always true
// (CloudProceduralVolume.cpp, the sum loop) and the bridge voxels go red here.
TEST( CloudProceduralField, ADeadNeighbourLeavesNoVoxelsAtALivingCluster )
{
    const CloudProceduralFieldParams params = MakeParams();
    const glm::vec2                  origin = CloudProceduralRegionOriginKm( params, 0.0f, 0.0f );
    const auto                       baked  = BakeCloudProceduralVolume( params, origin, {} );
    ASSERT_TRUE( baked ) << ( baked ? std::string{} : baked.GetError() );

    const float     cover = params.Coverage;
    const auto      lumps = GenerateCloudProceduralLumps( params, 0u, origin, CloudProceduralLumpSet::EveryCell );
    const std::vector<std::vector<CloudModellingBlob>> living = ClustersOf( lumps, cover );
    ASSERT_FALSE( living.empty() );

    const float voxelXKm = params.RegionSizeKm / static_cast<float>( kCloudProceduralVolumeSide );
    const float voxelYKm = params.LayerThicknessKm / static_cast<float>( kCloudProceduralVolumeHeight );

    size_t kept = 0, orphaned = 0;
    for ( uint32_t z = 0; z < kCloudProceduralVolumeSide; z += 3u )
        for ( uint32_t y = 0; y < kCloudProceduralVolumeHeight; ++y )
            for ( uint32_t x = 0; x < kCloudProceduralVolumeSide; x += 3u )
            {
                const size_t        at      = VoxelIndex( x, y, z );
                const unsigned char profile = baked.GetValue()[at];
                if ( profile <= 1u )
                    continue;
                ++kept;
                const glm::vec3 point( origin.x + ( static_cast<float>( x ) + 0.5f ) * voxelXKm,
                                       params.LayerBottomKm + ( static_cast<float>( y ) + 0.5f ) * voxelYKm,
                                       origin.y + ( static_cast<float>( z ) + 0.5f ) * voxelXKm );
                if ( ClusteredProfile( living, point, params ) <= 0.0f )
                    ++orphaned;
            }

    EXPECT_GT( kept, 0u ) << "the cover kept nothing, so the test is vacuous";
    EXPECT_EQ( orphaned, 0u ) << orphaned << " of " << kept
                              << " kept voxels lie in no living cluster's body — a dead neighbour's bridge";
}

TEST( CloudProceduralField, TheBakedVolumeAgreesWithAGatherOverEveryLumpInAnyOrder )
{
    const CloudProceduralFieldParams params = MakeParams();
    const glm::vec2                  origin = CloudProceduralRegionOriginKm( params, 0.0f, 0.0f );

    const auto baked = BakeCloudProceduralVolume( params, origin, {} );
    ASSERT_TRUE( baked ) << ( baked ? std::string{} : baked.GetError() );
    ASSERT_EQ( baked.GetValue().size(), CloudProceduralVoxelBytes( kCloudProceduralVolumeSide ) );

    // THE KEPT CELLS' LUMPS, what the bake draws since CUT-AT-BAKE: the Coverage cut is the bake's, each
    // cluster cut by its reach. The gather is the PREVIEW (EvaluateCloudProceduralProfile), which shares the
    // bake's voxel — the join per cluster, the altitude density, the cut — and differs only in the SET: every
    // lump at every wrap, where the bake asks the bin. The wraps are part of the field (the volume is
    // periodic), so each copy carries its cluster site with it and stays its own cluster.
    std::vector<CloudModellingBlob>        blobs;
    std::vector<CloudProceduralLump>       wrapped;
    const std::vector<CloudProceduralLump> lumps =
         GenerateCloudProceduralLumps( params, 0u, origin, CloudProceduralLumpSet::KeptCells );
    for ( const CloudProceduralLump& lump : lumps )
    {
        blobs.push_back( lump.Blob );
        for ( int wz = -1; wz <= 1; ++wz )
            for ( int wx = -1; wx <= 1; ++wx )
            {
                const glm::vec2     shift( static_cast<float>( wx ) * params.RegionSizeKm,
                                           static_cast<float>( wz ) * params.RegionSizeKm );
                CloudProceduralLump copy = lump;
                copy.Blob.CentreKm.x += shift.x;
                copy.Blob.CentreKm.z += shift.y;
                copy.ClusterKm += shift;
                wrapped.push_back( copy );
            }
    }
    ASSERT_FALSE( blobs.empty() );

    // SHUFFLED, then sorted back by the SAME canonical sort the bake uses. The join is commutative in real
    // arithmetic and not in floating point, and the sort is what makes the bytes a function of the SET.
    std::vector<CloudModellingBlob> shuffled = blobs;
    std::mt19937                    rng( 12345u );
    std::shuffle( shuffled.begin(), shuffled.end(), rng );
    SortCloudModellingBlobs( shuffled );

    ASSERT_EQ( shuffled.size(), blobs.size() );
    for ( size_t i = 0; i < blobs.size(); ++i )
        ASSERT_EQ( blobs[i].CentreKm, shuffled[i].CentreKm )
             << "the canonical sort did not put a shuffled list back in the same order at lump " << i;

    // THE PROBES ARE CHOSEN WHERE THERE IS CLOUD, and that correction is the finding of this test rather
    // than a convenience. A fixed lattice of 605 probes found eleven voxels with anything in them, because
    // a sky at a coverage of 0.35 is 1.9 per cent cloud BY VOLUME — the layer is thicker than the band any
    // one species occupies — so the agreement it measured was almost entirely an agreement about zero.
    //
    // Half the probes are taken from voxels the bake wrote something into and half from voxels it did not,
    // so the test measures the join where it is doing work AND checks that the culling has not invented
    // cloud in empty sky.
    std::vector<glm::u32vec3> inside;
    std::vector<glm::u32vec3> outside;

    size_t filled = 0;
    for ( uint32_t z = 0; z < kCloudProceduralVolumeSide; ++z )
        for ( uint32_t y = 0; y < kCloudProceduralVolumeHeight; ++y )
            for ( uint32_t x = 0; x < kCloudProceduralVolumeSide; ++x )
            {
                const bool solid = baked.GetValue()[VoxelIndex( x, y, z )] != 0u;
                if ( solid )
                    ++filled;

                std::vector<glm::u32vec3>& bucket = solid ? inside : outside;
                if ( bucket.size() < 200u && ( ( x * 7u + y * 13u + z * 31u ) % 97u ) == 0u )
                    bucket.push_back( glm::u32vec3( x, y, z ) );
            }

    std::printf( "[CloudProceduralField] the volume is %.2f%% cloud by voxel; probing %zu voxels inside it "
                 "and %zu outside\n",
                 100.0 * static_cast<double>( filled ) /
                      static_cast<double>( CloudProceduralVoxelBytes( kCloudProceduralVolumeSide ) / 4u ),
                 inside.size(), outside.size() );

    ASSERT_GE( inside.size(), 50u ) << "the bake produced almost no cloud, so there is nothing to compare";

    const float voxelXKm = params.RegionSizeKm / static_cast<float>( kCloudProceduralVolumeSide );
    const float voxelZKm = params.RegionSizeKm / static_cast<float>( kCloudProceduralVolumeSide );
    const float voxelYKm = params.LayerThicknessKm / static_cast<float>( kCloudProceduralVolumeHeight );

    int    checked   = 0;
    double worstStep = 0.0;

    for ( const std::vector<glm::u32vec3>* bucket : { &inside, &outside } )
    {
        for ( const glm::u32vec3& at : *bucket )
        {
            const glm::vec3 point( origin.x + ( static_cast<float>( at.x ) + 0.5f ) * voxelXKm,
                                   params.LayerBottomKm + ( static_cast<float>( at.y ) + 0.5f ) * voxelYKm,
                                   origin.y + ( static_cast<float>( at.z ) + 0.5f ) * voxelZKm );

            const float expected = EvaluateCloudProceduralProfile( params, 0u, wrapped, point );

            const unsigned char actual = baked.GetValue()[VoxelIndex( at.x, at.y, at.z )];
            const double        steps  = std::abs( static_cast<double>( actual ) / 255.0 - expected ) * 255.0;

            worstStep = std::max( worstStep, steps );
            ++checked;

            // ONE QUANTISATION STEP. The bake drops lumps more than kJoinCutoffRadii blend radii past the
            // nearest one, and the whole argument for that cut is that it is below the byte the volume is
            // stored in. This is that argument, asserted.
            EXPECT_LE( steps, 1.0 ) << "voxel (" << at.x << ", " << at.y << ", " << at.z << ") baked "
                                    << static_cast<int>( actual ) << "/255 where a gather over every lump "
                                    << "gives " << expected * 255.0 << "/255";
        }
    }

    std::printf( "[CloudProceduralField] %d probes, worst disagreement with the gather %.3f of a 255th\n", checked,
                 worstStep );

    // NOT A VACUOUS PASS. A volume that was entirely empty would satisfy every EXPECT above, and the
    // ASSERT on `inside` above is what makes that impossible — this states the same thing about the
    // fraction, so a bake that collapsed to a handful of lumps is caught even though it is not empty.
    EXPECT_GT( filled, ( CloudProceduralVoxelBytes( kCloudProceduralVolumeSide ) / 4u ) / 400u )
         << "under a quarter of a per cent of the volume has cloud in it, so the sky this agreed about is "
            "not one anybody would look at";
}

// H-BASE: THE CROSS-SECTION SHRINKS TOWARDS THE BASE. The altitude density multiplies the profile before the
// coverage remap, so at the base row of a type's band the area that clears the remap's threshold is smaller
// than a fifth of the band up, where the curve is full — the base is rounded, not the floor of a box.
// Mutation: drop the `* CloudProceduralAltitudeDensity(...)` factor in the bake (CloudProceduralVolume.cpp,
// the profile line) and the two areas come out equal or inverted.
TEST( CloudProceduralField, TheCrossSectionShrinksTowardsTheBase )
{
    const CloudProceduralFieldParams params = MakeParams();
    const glm::vec2                  origin = CloudProceduralRegionOriginKm( params, 0.0f, 0.0f );

    const auto baked = BakeCloudProceduralVolume( params, origin );
    ASSERT_TRUE( baked ) << ( baked ? std::string{} : baked.GetError() );
    const std::vector<unsigned char>& voxels = baked.GetValue();

    const Desert::Graphic::CloudTypeShape& shape    = params.Species[0].Shape;
    const float                            voxelYKm = params.LayerThicknessKm / kCloudProceduralVolumeHeight;
    const auto rowAt = [&]( float altitudeKm )
    { return static_cast<uint32_t>( std::clamp( ( altitudeKm - params.LayerBottomKm ) / voxelYKm, 0.0f,
                                                static_cast<float>( kCloudProceduralVolumeHeight - 1 ) ) ); };
    const float bandKm = shape.TopAltitudeKm - shape.BaseAltitudeKm;

    // The remap at a mid cover: g = 0.5 keeps what is deeper than half the profile.
    const auto areaOfRow = [&]( uint32_t y )
    {
        size_t area = 0;
        for ( uint32_t z = 0; z < kCloudProceduralVolumeSide; ++z )
            for ( uint32_t x = 0; x < kCloudProceduralVolumeSide; ++x )
                area += voxels[VoxelIndex( x, y, z )] > 127u ? 1u : 0u;
        return area;
    };

    const uint32_t baseRow = rowAt( shape.BaseAltitudeKm + 0.5f * voxelYKm );
    const uint32_t bodyRow = rowAt( shape.BaseAltitudeKm + 0.25f * bandKm );
    ASSERT_LT( baseRow, bodyRow );

    const size_t baseArea = areaOfRow( baseRow );
    const size_t bodyArea = areaOfRow( bodyRow );
    std::printf( "[CloudProceduralField] cross-section past the remap: base row %u = %zu voxels, a quarter up "
                 "row %u = %zu\n",
                 baseRow, baseArea, bodyRow, bodyArea );

    ASSERT_GT( bodyArea, 0u ) << "nothing clears the threshold a quarter up the band, so there is no body";
    EXPECT_LT( static_cast<double>( baseArea ), 0.5 * static_cast<double>( bodyArea ) )
         << "the base is as wide as the body: the altitude density is not reaching the profile";
}

// ---------------------------------------------------------------------------------------------------
// 3. NO LUMP IS THINNER THAN THE MARCH CAN FIND — AT EVERY TIER
// ---------------------------------------------------------------------------------------------------
//
// THE RELATION THAT HAS BITTEN THIS PROGRAMME TWICE. The march SEARCHES at the coarse step and only drops
// to the fine tier once a coarse sample has found material, so a body that fits between two coarse samples
// is never seen — and whether it fits is decided by the ray's jitter, which is the definition of speckle.
//
// The generator's answer is a CLAMP rather than a refusal, because a type authored with a forty-metre band
// is a legal thing to write in a `.decloudtype` and the honest response is a lobe the march can see. This
// asserts the clamp holds, including for shapes deliberately authored below it.
TEST( CloudProceduralField, NoGeneratedLumpIsThinnerThanTheMarchCanFindAtAnyTier )
{
    const float chordKm = ResolvableChordKm();
    ASSERT_GT( chordKm, 0.0f );

    std::printf( "[CloudProceduralField] the march resolves chords down to %.0f m at Max Steps %.0f\n",
                 chordKm * 1000.0f, kComponentMaxSteps );

    // THE TIERS. Graphic::CloudQualityScale carries no Max Steps field — phase Э3 measured that halving it
    // would put five of nine types past Nyquist and refused — so every tier marches with the component's
    // own count and the bound below is the same at all four. The loop states that rather than assuming it:
    // the day a tier gains a step count, this is the line that fails.
    for ( const float maxSteps : { 256.0f, 256.0f, 256.0f, 256.0f } )
    {
        const float tierChordKm =
             Desert::Tests::CloudProceduralScheduleRef::CloudFinestResolvableChordKm( maxSteps );

        // FOUR SHAPES, and three of them are adversarial: a stratus whose band is thinner than one chord, a
        // species on a cell smaller than a chord, and one whose taper would shrink the top of every stack
        // to nothing.
        struct Case
        {
            const char* Name;
            float       BaseKm;
            float       TopKm;
            float       CellKm;
            float       Taper;
        };

        for ( const Case& item :
              { Case{ "cumulus", 1.8f, 3.4f, 3.0f, 0.45f }, Case{ "razor stratus", 0.4f, 0.44f, 4.0f, 0.0f },
                Case{ "pinhead cells", 1.8f, 3.4f, 0.05f, 0.2f }, Case{ "total taper", 1.5f, 3.0f, 2.0f, 1.0f } } )
        {
            CloudProceduralFieldParams params      = MakeParams();
            params.ResolvableChordKm               = tierChordKm;
            params.Coverage                        = 1.0f;
            params.Species[0].CellKm               = item.CellKm;
            params.Species[0].Shape.BaseAltitudeKm = item.BaseKm;
            params.Species[0].Shape.TopAltitudeKm  = item.TopKm;
            params.Species[0].Shape.Profile        = Desert::Graphic::CloudProfileFromTaper( item.Taper );
            params.LayerBottomKm                   = std::min( params.LayerBottomKm, item.BaseKm );
            params.LayerThicknessKm = std::max( params.LayerThicknessKm, item.TopKm - params.LayerBottomKm );

            const glm::vec2 origin = CloudProceduralRegionOriginKm( params, 0.0f, 0.0f );

            const std::vector<CloudModellingBlob> blobs = GenerateCloudProceduralBlobs( params, 0u, origin );
            ASSERT_FALSE( blobs.empty() ) << item.Name << " placed nothing at full coverage";

            float smallest = blobs.front().RadiiKm.x;
            for ( const CloudModellingBlob& blob : blobs )
            {
                smallest = std::min( { smallest, blob.RadiiKm.x, blob.RadiiKm.y, blob.RadiiKm.z } );

                EXPECT_GE( 2.0f * blob.RadiiKm.x, tierChordKm )
                     << item.Name << ": a lump is only " << blob.RadiiKm.x * 2000.0f << " m across x";
                EXPECT_GE( 2.0f * blob.RadiiKm.y, tierChordKm )
                     << item.Name << ": a lump is only " << blob.RadiiKm.y * 2000.0f << " m across y";
                EXPECT_GE( 2.0f * blob.RadiiKm.z, tierChordKm )
                     << item.Name << ": a lump is only " << blob.RadiiKm.z * 2000.0f << " m across z";
            }

            std::printf( "[CloudProceduralField] %-14s %5zu lumps, smallest semi-axis %5.0f m = %.2fx the "
                         "%.0f m the march resolves\n",
                         item.Name, blobs.size(), smallest * 1000.0f, 2.0f * smallest / tierChordKm,
                         tierChordKm * 1000.0f );
        }
    }
}

// ---------------------------------------------------------------------------------------------------
// 3a. AND NO LUMP IS NARROWER THAN THE VOLUME CAN CARRY — THE OTHER HALF OF THE SAME RELATION
// ---------------------------------------------------------------------------------------------------
//
// TWO SIEVES STAND BETWEEN A LUMP AND THE EYE AND THE SUITE ONLY EVER CHECKED ONE. Test 3 above asserts a
// lump clears the MARCH's search lattice; `ValidateCloudProceduralParams` asserts the volume is not FINER
// than that lattice. Neither says anything about the direction that actually binds: a lump has to survive
// the VOLUME first, and trilinear filtering cannot express a feature narrower than two voxels — 375 m at
// the shipped 48 km region against the 125 m the march resolves. The generator's floor was the march's,
// so it authorised lumps three times finer than the container they are written into, and what a sampler
// returns for one of those is not a small cloud but a smear the size of the filter.
//
// IT IS ASSERTED OVER THE KNOBS' OWN RANGES AND NOT ONLY AT THE SHIPPED POINT, AND THAT IS THE WHOLE
// DESIGN OF THIS TEST. At the defaults the old floor never bit — the narrowest lump the shipped congestus
// emits is 459 m — so a test pinned there goes green over every sky below. Counted before the floor
// existed, at Coverage 1 so the population is the whole field: `PlacementDensity` at 8 gives 38 of 11 904
// lumps under two voxels, `PlacementSizeVariety` at 1 gives 76 of 2 646, the smallest cell the cell floor
// permits gives 14 367 of 41 658 (34.5 %) with the narrowest at 130 m, and all three together give
// 177 168 of 188 640 (93.9 %). Every one of those settings is inside a slider's own published range.
//
// THE COMPANION CLAIM, and it is the one the cell floor was making without delivering: a cluster on the
// smallest cell `CloudProceduralCellExtentKm` permits is four voxels wide, and its argument is that four
// voxels is "the narrowest cluster the volume can carry with an inside and two edges". A cluster is six
// lobes, so that bound says nothing about a lobe — at that exact cell a third of the lobes were under two
// voxels. The floor below is what makes the cell floor's own sentence true.
TEST( CloudProceduralField, NoGeneratedLumpIsNarrowerThanTheVolumeCanCarry )
{
    const CloudProceduralFieldParams shipped = MakeParams();

    const float voxelKm = shipped.RegionSizeKm / static_cast<float>( kCloudProceduralVolumeSide );
    const float floorKm = Desert::Assets::CloudProceduralLumpFloorKm( shipped );

    std::printf( "[CloudProceduralField] voxel %.1f m; the volume expresses %.0f m, the march finds %.0f m; "
                 "the lump floor is %.0f m across\n",
                 voxelKm * 1000.0f, 2000.0f * voxelKm, shipped.ResolvableChordKm * 1000.0f, 2000.0f * floorKm );

    // THE FLOOR IS THE LARGER OF THE TWO BOUNDS, stated as a relation so that neither can be dropped by a
    // later edit. It is not "the floor is 187.5 m" — that would be this test checking its own arithmetic.
    EXPECT_GE( floorKm, 0.5f * shipped.ResolvableChordKm )
         << "the lump floor no longer clears the march's search lattice, so a lump can be placed that only "
            "the ray's jitter will ever find";
    EXPECT_GE( 2.0f * floorKm, 2.0f * voxelKm )
         << "the lump floor no longer clears two voxels, so the generator may write a feature the trilinear "
            "sampler cannot express and the volume returns a smear of the filter's own size";

    // EVERY PLACEMENT KNOB AT BOTH ENDS OF ITS RANGE, plus the smallest cell the cell floor permits — the
    // four settings the census found sub-voxel lumps under. `Coverage` is held at 1 so that every lattice
    // cell is alive and the population is the whole field rather than a sample of it.
    struct Case
    {
        const char* Name;
        float       CellKm;
        float       Density;
        float       Scatter;
        float       Variety;
    };

    for ( const Case& item : { Case{ "shipped", 3.00f, 1.75f, 1.00f, 0.75f },
                               Case{ "smallest cell the floor allows", 0.75f, 1.75f, 1.00f, 0.75f },
                               Case{ "cell below that floor", 0.20f, 1.75f, 1.00f, 0.75f },
                               Case{ "densest", 3.00f, 8.00f, 1.00f, 0.75f },
                               Case{ "widest size spread", 3.00f, 1.75f, 1.00f, 1.00f },
                               Case{ "all three at once", 0.75f, 8.00f, 4.00f, 1.00f } } )
    {
        CloudProceduralFieldParams params = MakeParams();
        params.Coverage                   = 1.0f;
        params.Species[0].CellKm          = item.CellKm;
        params.PlacementDensity           = item.Density;
        params.PlacementScatter           = item.Scatter;
        params.PlacementSizeVariety       = item.Variety;

        const glm::vec2 origin = CloudProceduralRegionOriginKm( params, 0.0f, 0.0f );

        const std::vector<CloudModellingBlob> blobs = GenerateCloudProceduralBlobs( params, 0u, origin );
        ASSERT_FALSE( blobs.empty() ) << item.Name << " placed nothing at full coverage";

        float narrowestKm = blobs.front().RadiiKm.x;
        long  underVoxels = 0;

        for ( const CloudModellingBlob& blob : blobs )
        {
            const float narrowHorizontalKm = std::min( blob.RadiiKm.x, blob.RadiiKm.z );
            narrowestKm                    = std::min( narrowestKm, narrowHorizontalKm );

            if ( 2.0f * narrowHorizontalKm < 2.0f * voxelKm )
                ++underVoxels;
        }

        std::printf( "[CloudProceduralField] %-30s %6zu lumps, narrowest %5.0f m = %.2f voxels, %ld under two\n",
                     item.Name, blobs.size(), 2000.0f * narrowestKm, 2.0f * narrowestKm / voxelKm, underVoxels );

        EXPECT_EQ( underVoxels, 0 )
             << item.Name << ": " << underVoxels << " of " << blobs.size()
             << " lumps are narrower than the two voxels the volume can express — the narrowest is "
             << 2000.0f * narrowestKm << " m against " << 2000.0f * voxelKm << " m";
    }
}

// ---------------------------------------------------------------------------------------------------
// 4. NOTHING INSIDE THE REGION MOVES WHEN THE REGION SCROLLS
// ---------------------------------------------------------------------------------------------------
//
// THE PROPERTY THAT MAKES THE VOLUME CAMERA-CENTRIC AT ALL. The region follows the camera in snapped steps,
// and a lump's identity is the hash of its ABSOLUTE lattice cell — so a cell that is inside the region
// before a shift and after it must produce exactly the same lump. If it did not, the sky would boil as the
// camera walked, which is the defect the shadow map's own snap was measured against (0.545/255 with the
// snap against 2.291/255 without it).
TEST( CloudProceduralField, ScrollingTheRegionByOneSnapDoesNotMoveTheFieldInsideIt )
{
    const CloudProceduralFieldParams params = MakeParams();

    const float snapKm = CloudProceduralSnapKm( params );
    ASSERT_GT( snapKm, 0.0f );

    const glm::vec2 before = CloudProceduralRegionOriginKm( params, 0.0f, 0.0f );
    const glm::vec2 after  = CloudProceduralRegionOriginKm( params, snapKm, 0.0f );

    ASSERT_NE( before.x, after.x ) << "a camera moved by one whole snap step did not move the region, so "
                                      "this test compares a region with itself";
    EXPECT_FLOAT_EQ( after.x - before.x, snapKm ) << "the region moved by something other than one snap";

    const std::vector<CloudModellingBlob> a = GenerateCloudProceduralBlobs( params, 0u, before );
    const std::vector<CloudModellingBlob> b = GenerateCloudProceduralBlobs( params, 0u, after );

    ASSERT_FALSE( a.empty() );
    ASSERT_FALSE( b.empty() );

    // The overlap of the two regions, pulled in by the distance a lump can reach, is where the field is
    // obliged to be identical. Outside it a lump has left one region or entered the other, which is what a
    // window is for.
    const float reachKm = 4.0f;

    const float minX = std::max( before.x, after.x ) + reachKm;
    const float maxX = std::min( before.x, after.x ) + params.RegionSizeKm - reachKm;

    int matched = 0;
    for ( const CloudModellingBlob& blob : a )
    {
        if ( blob.CentreKm.x < minX || blob.CentreKm.x > maxX )
            continue;

        const auto found =
             std::find_if( b.begin(), b.end(), [&]( const CloudModellingBlob& other )
                           { return other.CentreKm == blob.CentreKm && other.RadiiKm == blob.RadiiKm; } );

        ASSERT_NE( found, b.end() ) << "a lump at (" << blob.CentreKm.x << ", " << blob.CentreKm.y << ", "
                                    << blob.CentreKm.z
                                    << ") km is well inside both regions and yet did not survive a shift of "
                                    << snapKm << " km — the hash is reading the region rather than the world";
        ++matched;
    }

    std::printf( "[CloudProceduralField] %d of %zu lumps lie in the overlap of two regions one snap "
                 "(%.1f km) apart, and every one of them is unmoved\n",
                 matched, a.size(), snapKm );

    EXPECT_GT( matched, static_cast<int>( a.size() ) / 4 )
         << "too few lumps fell in the overlap for this to have measured anything";
}

// ---------------------------------------------------------------------------------------------------
// 5. THE VOLUME IS SEAMLESS ACROSS ITS OWN WRAP
// ---------------------------------------------------------------------------------------------------
//
// Every sampler in this engine is LINEAR/REPEAT, so what happens beyond the region is the region again —
// that is the degenerate far path of ANALYSIS_APPROACH.md §3 point 3. It is only a far path and not a
// defect if the volume is PERIODIC: a volume baked without wrapping its lumps shows a hard discontinuity
// down the seam, and at the horizon that is a straight vertical edge in the sky.
//
// MEASURED AS A COMPARISON OF TWO NUMBERS rather than asserted as an equality, because the field genuinely
// varies from column to column and "equal across the seam" would be false for a correct volume too. The
// step across the wrap must be no worse than the ordinary step between neighbours.
TEST( CloudProceduralField, TheVolumeIsPeriodicSoRepeatSamplingShowsNoSeam )
{
    const CloudProceduralFieldParams params = MakeParams();
    const glm::vec2                  origin = CloudProceduralRegionOriginKm( params, 0.0f, 0.0f );

    const auto baked = BakeCloudProceduralVolume( params, origin );
    ASSERT_TRUE( baked ) << ( baked ? std::string{} : baked.GetError() );

    const std::vector<unsigned char>& voxels = baked.GetValue();

    double seamSum     = 0.0;
    double interiorSum = 0.0;
    int    count       = 0;

    for ( uint32_t z = 0; z < kCloudProceduralVolumeSide; ++z )
    {
        for ( uint32_t y = 0; y < kCloudProceduralVolumeHeight; ++y )
        {
            const int last  = voxels[VoxelIndex( kCloudProceduralVolumeSide - 1u, y, z )];
            const int first = voxels[VoxelIndex( 0u, y, z )];
            const int one   = voxels[VoxelIndex( 1u, y, z )];

            seamSum += std::abs( last - first );
            interiorSum += std::abs( one - first );
            ++count;
        }
    }

    const double seam     = seamSum / count;
    const double interior = interiorSum / count;

    std::printf( "[CloudProceduralField] mean step across the wrap %.3f/255 against %.3f/255 between "
                 "ordinary neighbours\n",
                 seam, interior );

    // A factor of two of slack over the ordinary neighbour step, which is generous: a volume baked WITHOUT
    // the wrap measures the two edges of unrelated sky against each other and lands an order of magnitude
    // above this, because the seam is then the difference between two independent samples of the field
    // rather than between two adjacent ones.
    EXPECT_LE( seam, 2.0 * interior + 0.5 )
         << "the step across the volume's own wrap is " << seam << "/255 against " << interior
         << "/255 between ordinary neighbours, so REPEAT sampling puts a visible edge in the sky at every "
            "region boundary — the bake is not splatting its lumps at the wrapped positions";

    // NOT VACUOUS: a volume of all zeros has a seam of zero and an interior step of zero.
    EXPECT_GT( interior, 0.0 ) << "the volume is flat, so neither number above measured anything";
}

namespace
{
    using namespace Desert::Assets;

    // THE SKY THE MARCH KEEPS (FARWX-a): the bake holds every cell with its rank, and a column has cloud
    // when any of its voxels is kept at the local cover of the column's WORLD site — rank under the cover and
    // within the air's reach of its body (CloudProceduralColumnKept, FARWX-b15). Measured at many whole-region
    // shifts, because the weather is a world field and one region holds only a couple of its systems.
    glm::vec2 RegionShiftKm( const CloudProceduralFieldParams& params, int shift )
    {
        // Whole regions, so the bake's column index is unchanged; scattered so the weather is not.
        return glm::vec2( static_cast<float>( ( shift * 7 ) % 41 ), static_cast<float>( ( shift * 13 ) % 37 ) ) *
               params.RegionSizeKm * static_cast<float>( 1 + shift / 41 );
    }

    std::vector<float> KeptColumns( const CloudProceduralFieldParams& params,
                                    const std::vector<unsigned char>& bake, const glm::vec2& origin, int shift )
    {
        const uint32_t     side  = params.VolumeSideVoxels;
        const float        voxel = params.RegionSizeKm / static_cast<float>( side );
        const glm::vec2    base  = origin + RegionShiftKm( params, shift );
        std::vector<float> map( static_cast<size_t>( side ) * side, 0.0f );
        for ( uint32_t z = 0; z < side; ++z )
            for ( uint32_t x = 0; x < side; ++x )
            {
                const glm::vec2 world = base + glm::vec2( ( static_cast<float>( x ) + 0.5f ) * voxel,
                                                          ( static_cast<float>( z ) + 0.5f ) * voxel );
                const size_t    at    = static_cast<size_t>( z ) * side + x;
                map[at] =
                     CloudProceduralColumnKept( bake, side, x, z, CloudProceduralLocalWeathers( params, world ) )
                          ? 1.0f
                          : 0.0f;
            }
        return map;
    }

    double KeptCover( const CloudProceduralFieldParams& shipped, int shifts )
    {
        // AT THE COARSEST GRID THE COMPONENT OFFERS: the cover is the same sky sampled more coarsely
        // (ACoarserGridIsTheSameSkySampledMoreCoarsely holds it within a hundredth), and the bake and the
        // world weather per column are the whole cost of this measurement — a quarter of it at 128.
        CloudProceduralFieldParams params = shipped;
        params.VolumeSideVoxels           = kCloudProceduralVolumeSideMin;

        const glm::vec2 origin = CloudProceduralRegionOriginKm( params, 0.0f, 0.0f );
        const auto      baked  = BakeCloudProceduralVolume( params, origin, {} );
        if ( !baked )
            return -1.0;
        const size_t columns = static_cast<size_t>( params.VolumeSideVoxels ) * params.VolumeSideVoxels;
        double       sum     = 0.0;
        for ( int shift = 0; shift < shifts; ++shift )
        {
            const std::vector<float> map = KeptColumns( params, baked.GetValue(), origin, shift );
            for ( float v : map )
                sum += v;
        }
        return sum / ( static_cast<double>( shifts ) * static_cast<double>( columns ) );
    }
} // namespace

// ---------------------------------------------------------------------------------------------------
// 6. COVERAGE IS NUBIS' "HOW BUSY THE WEATHER IS" (WX-NUBIS): EMPTY AT ZERO, MONOTONE, THE ZEROS STAY CLEAR
// ---------------------------------------------------------------------------------------------------
//
// Owner, 10-08: Coverage is no longer the fraction of sky. localCover = Coverage * W, W >= 0 the weather map
// with true zeros, so 0 is empty, more Coverage is never less cloud, and a column whose W is zero is clear at
// every Coverage — an overcast sky is a cloud type, not Coverage 1.
TEST( CloudProceduralField, CoverageIsEmptyAtZeroAndTheWeathersZerosStayClearAtOne )
{
    CloudProceduralFieldParams params = MakeParams();

    params.Coverage        = 0.0f;
    const glm::vec2 origin = CloudProceduralRegionOriginKm( params, 0.0f, 0.0f );

    EXPECT_TRUE( GenerateCloudProceduralBlobs( params, 0u, origin ).empty() )
         << "a coverage of zero put cloud in a sky the artist asked to be empty";

    const double emptyCover = KeptCover( params, 4 );
    ASSERT_GE( emptyCover, 0.0 ) << "the bake failed";
    EXPECT_EQ( emptyCover, 0.0 ) << "a coverage of zero kept cloud over " << emptyCover << " of the sky";

    // AT COVERAGE ONE THE WEATHER'S ZEROS ARE STILL CLEAR. Census of the cut itself: every column the
    // weather map empties keeps nothing, and the sky is short of full by at least half the empty fraction.
    params.Coverage = 1.0f;
    {
        CloudProceduralFieldParams coarse = params;
        coarse.VolumeSideVoxels           = kCloudProceduralVolumeSideMin;
        const glm::vec2 at                = CloudProceduralRegionOriginKm( coarse, 0.0f, 0.0f );
        const auto      baked             = BakeCloudProceduralVolume( coarse, at, {} );
        ASSERT_TRUE( baked ) << "the bake failed";
        const uint32_t side  = coarse.VolumeSideVoxels;
        const float    voxel = coarse.RegionSizeKm / static_cast<float>( side );
        size_t         zeros = 0, keptInZeros = 0;
        for ( int shift = 0; shift < 8; ++shift )
        {
            const glm::vec2 base = at + RegionShiftKm( coarse, shift );
            for ( uint32_t z = 0; z < side; ++z )
                for ( uint32_t x = 0; x < side; ++x )
                {
                    const glm::vec2 world = base + glm::vec2( ( static_cast<float>( x ) + 0.5f ) * voxel,
                                                              ( static_cast<float>( z ) + 0.5f ) * voxel );
                    const glm::vec4 covers = CloudProceduralLocalWeathers( coarse, world );
                    if ( std::max( std::max( covers.x, covers.y ), std::max( covers.z, covers.w ) ) > 0.0f )
                        continue;
                    ++zeros;
                    keptInZeros += CloudProceduralColumnKept( baked.GetValue(), side, x, z, covers ) ? 1u : 0u;
                }
        }
        EXPECT_GT( zeros, size_t{ 0 } ) << "the shipped weather strength has no true zeros";
        EXPECT_EQ( keptInZeros, size_t{ 0 } ) << keptInZeros << " columns the weather empties still hold cloud";
    }

    const double fullCover = KeptCover( params, 16 );
    const double ps        = static_cast<double>( params.PatchStrength );
    const double zeroShare = ps - 0.15 * std::min( ps, 1.0 - ps ); // CloudWeatherPresence's true zeros
    EXPECT_LT( fullCover, 1.0 - 0.5 * zeroShare )
         << "a coverage of one kept cloud over " << fullCover << " of the sky: the weather's gaps were filled";
    const std::vector<CloudModellingBlob> full = GenerateCloudProceduralBlobs( params, 0u, origin );

    params.Coverage                            = 0.5f;
    const std::vector<CloudModellingBlob> half = GenerateCloudProceduralBlobs( params, 0u, origin );

    ASSERT_FALSE( full.empty() );
    ASSERT_FALSE( half.empty() );

    // The lump COUNT is not proportional to the coverage — a shallow cell grows a shorter stack — so the
    // property asserted is monotonicity plus the two exact ends, which is what the slider promises.
    EXPECT_GT( full.size(), half.size() )
         << "raising the coverage from a half to one did not add cloud to the sky";

    std::printf( "[CloudProceduralField] coverage 0 / 1 keeps %.4f / %.4f of the sky\n", emptyCover, fullCover );
    std::printf( "[CloudProceduralField] coverage 0 / 0.5 / 1 gives %zu / %zu / %zu lumps\n", size_t{ 0 },
                 half.size(), full.size() );
}

// ---------------------------------------------------------------------------------------------------
// 7. THE REGION AGAINST THE VOLUME'S OWN RESOLUTION
// ---------------------------------------------------------------------------------------------------
//
// The voxel is RegionSize/Width, trilinear filtering cannot express a feature narrower than two of them,
// and the march searches at the resolvable chord. Those three numbers are one relation and this is where
// it is kept — a region shrunk past the bound is refused with both figures in the message rather than
// quietly filling the volume with structure no ray can find.
TEST( CloudProceduralField, AValidatedRegionAlwaysClearsTheMarchsSearchLattice )
{
    CloudProceduralFieldParams params = MakeParams();

    EXPECT_TRUE( ValidateCloudProceduralParams( params ) ) << ValidateCloudProceduralParams( params ).GetError();

    const float voxelKm = params.RegionSizeKm / static_cast<float>( kCloudProceduralVolumeSide );
    std::printf( "[CloudProceduralField] a %.0f km region over %u voxels is %.1f m per voxel, finest "
                 "feature %.0f m, against the %.0f m the march resolves\n",
                 params.RegionSizeKm, kCloudProceduralVolumeSide, voxelKm * 1000.0f, 2.0f * voxelKm * 1000.0f,
                 params.ResolvableChordKm * 1000.0f );

    // The exact bound, from below. A region of `chord/2 * Width` is the smallest legal one.
    params.RegionSizeKm =
         0.5f * params.ResolvableChordKm * static_cast<float>( kCloudProceduralVolumeSide ) * 0.99f;

    const auto refused = ValidateCloudProceduralParams( params );
    EXPECT_FALSE( refused ) << "a region of " << params.RegionSizeKm
                            << " km puts the voxel below half the march's search chord and was accepted";
}

// ---------------------------------------------------------------------------------------------------
// 8. WHAT THE BAKE COSTS — the phase's own exit criterion, measured rather than assumed
// ---------------------------------------------------------------------------------------------------
//
// ANALYSIS_APPROACH.md §3 ends the variant with "the cost of generating the volume when the region shifts
// is a quantity to be MEASURED, not assumed; it is the exit criterion of the phase". This is that
// measurement, and it lives in the suite rather than in a document so that it is re-taken on every run
// instead of being a number somebody trusted after it stopped being true.
//
// IT ASSERTS A CEILING AND NOT A TARGET. The bake runs on a worker thread and the volume is swapped in when
// it finishes, so its cost is a latency and not a frame time — but a bake that took minutes would mean the
// sky lagged the camera by minutes, and that is what the bound below is about.
TEST( CloudProceduralField, TheCostOfRebakingTheRegionIsMeasured )
{
    for ( const int speciesCount : { 1, 2, 4 } )
    {
        CloudProceduralFieldParams params = MakeParams();
        while ( static_cast<int>( params.Species.size() ) < speciesCount )
        {
            CloudProceduralSpecies extra = params.Species.front();
            extra.CellKm                 = 2.0f + 1.5f * static_cast<float>( params.Species.size() );
            params.Species.push_back( extra );
        }

        const glm::vec2 origin = CloudProceduralRegionOriginKm( params, 0.0f, 0.0f );
        const size_t    lumps  = Desert::Assets::CountCloudProceduralBlobs( params, origin );

        // THE MINIMUM OF THREE and not the mean: this machine is shared with other agents, and a mean
        // measures them (desert-engine-verify §5a).
        double best = 0.0;
        for ( int run = 0; run < 3; ++run )
        {
            const auto start = std::chrono::steady_clock::now();
            const auto baked = BakeCloudProceduralVolume( params, origin );
            const auto stop  = std::chrono::steady_clock::now();

            ASSERT_TRUE( baked ) << ( baked ? std::string{} : baked.GetError() );

            const double ms = std::chrono::duration<double, std::milli>( stop - start ).count();
            best            = ( run == 0 ) ? ms : std::min( best, ms );
        }

        std::printf( "[CloudProceduralField] %d species, %zu lumps, %u x %u x %u voxels: %.1f ms per rebake "
                     "(best of 3, DEBUG build)\n",
                     speciesCount, lumps, kCloudProceduralVolumeSide, kCloudProceduralVolumeHeight,
                     kCloudProceduralVolumeSide, best );

        EXPECT_LT( best, 60000.0 ) << "a rebake of " << best
                                   << " ms means the sky lags the camera by a minute; the region's size, the "
                                      "volume's resolution or the lump count has to come down";
    }
}

// ---------------------------------------------------------------------------------------------------
// 6b. THE SKY'S COVER RISES WITH COVERAGE, AND ITS GAPS ARE WEATHER-SIZED (WX-NUBIS)
// ---------------------------------------------------------------------------------------------------
//
// What a person looking up sees is the top-down projection of what the cut keeps. Coverage no longer has to
// equal it (owner, 10-08); it has to move it one way only. And the clear sky between weather systems has to be
// GAPS a sight line can see through — runs of empty columns at least a quarter of the weather's shortest
// wavelength (PatchTileKm) — not a uniform thinning, at every Coverage up to one.
TEST( CloudProceduralField, TheSkysCoverRisesWithCoverageAndItsGapsAreWeatherSized )
{
    double previous = -1.0;
    for ( const float coverage : { 0.15f, 0.35f, 0.50f, 0.75f, 1.0f } )
    {
        CloudProceduralFieldParams params = MakeParams();
        params.Coverage                   = coverage;

        const double measured = KeptCover( params, 16 );
        ASSERT_GE( measured, 0.0 ) << "the bake failed";
        std::printf( "[CloudProceduralField] coverage %.2f -> %.3f of the sky has cloud in the column\n", coverage,
                     measured );
        EXPECT_GT( measured, previous ) << "raising Coverage to " << coverage << " did not add cloud to the sky";
        previous = measured;
    }

    // THE GAPS, by census on the kept map at Coverage one: the longest clear run along a row, over shifts.
    CloudProceduralFieldParams params = MakeParams();
    params.Coverage                   = 1.0f;
    params.VolumeSideVoxels           = kCloudProceduralVolumeSideMin;
    const glm::vec2 origin            = CloudProceduralRegionOriginKm( params, 0.0f, 0.0f );
    const auto      baked             = BakeCloudProceduralVolume( params, origin, {} );
    ASSERT_TRUE( baked ) << "the bake failed";

    const uint32_t side    = params.VolumeSideVoxels;
    const float    voxelKm = params.RegionSizeKm / static_cast<float>( side );
    float          longest = 0.0f;
    for ( int shift = 0; shift < 16; ++shift )
    {
        const std::vector<float> map = KeptColumns( params, baked.GetValue(), origin, shift );
        for ( uint32_t z = 0; z < side; ++z )
        {
            uint32_t run = 0;
            for ( uint32_t x = 0; x < side; ++x )
            {
                run     = map[static_cast<size_t>( z ) * side + x] > 0.0f ? 0u : run + 1u;
                longest = std::max( longest, static_cast<float>( run ) * voxelKm );
            }
        }
    }
    std::printf( "[CloudProceduralField] at coverage 1 the longest clear run is %.1f km\n", longest );
    EXPECT_GE( longest, 0.25f * params.PatchTileKm )
         << "at Coverage 1 the longest clear run was " << longest << " km: the weather's zeros were filled in";
}

// ---------------------------------------------------------------------------------------------------
// 7. THE BAKE'S OWN BUDGET (O8) — the grid is a parameter, and the three things that must stay true
// ---------------------------------------------------------------------------------------------------
//
// WHY THE GRID BECAME A PARAMETER. The resolution used to be a constant, so a 512-pixel material-preview
// pane baked exactly the volume a whole level bakes, and an artist dragging Coverage waited 15.42 s from
// their last edit to a sky that showed it. What is under test here is not "smaller is faster" — that is
// arithmetic — but the properties a caller is entitled to rely on: the block that comes back is the size
// the parameters asked for, the slider's whole travel is legal and nothing outside it is silently
// clamped, and the SKY IS THE SAME SKY, only sampled more coarsely.

TEST( CloudProceduralBudget, TheBlockIsExactlyTheSizeTheParametersAskedFor )
{
    // A LENGTH THAT DOES NOT MATCH THE EXTENTS THE CALLER THEN BUILDS AN IMAGE WITH is the kind of defect
    // that reads as a corrupt sky rather than as a wrong number, so it is asserted for every side the
    // component's own Range permits rather than for the default alone.
    for ( const uint32_t side : { kCloudProceduralVolumeSideMin, 160u, 192u, kCloudProceduralVolumeSide } )
    {
        CloudProceduralFieldParams params = MakeParams();
        params.VolumeSideVoxels           = side;

        const glm::vec2 origin = CloudProceduralRegionOriginKm( params, 0.0f, 0.0f );
        const auto      baked  = BakeCloudProceduralVolume( params, origin );

        ASSERT_TRUE( baked ) << "side " << side << ": " << ( baked ? std::string{} : baked.GetError() );
        EXPECT_EQ( baked.GetValue().size(), CloudProceduralVoxelBytes( side ) )
             << "side " << side << " produced a block of a different length than its own extents describe";
    }
}

TEST( CloudProceduralBudget, TheRangeTheComponentOffersIsExactlyTheRangeTheBakeAccepts )
{
    // THE DEAD-SETTING TEST, FROM BOTH ENDS. The component offers 64..256; a value inside that must bake,
    // and a value outside it must be REFUSED BY NAME rather than silently clamped — a silent clamp is how
    // a slider ends up with a half that does nothing (contract §1.3).
    CloudProceduralFieldParams params = MakeParams();

    params.VolumeSideVoxels = kCloudProceduralVolumeSideMin;
    EXPECT_TRUE( ValidateCloudProceduralParams( params ) );

    params.VolumeSideVoxels = kCloudProceduralVolumeSide;
    EXPECT_TRUE( ValidateCloudProceduralParams( params ) );

    params.VolumeSideVoxels = kCloudProceduralVolumeSideMin - 1u;
    EXPECT_FALSE( ValidateCloudProceduralParams( params ) ) << "a grid below the floor was accepted";

    params.VolumeSideVoxels = kCloudProceduralVolumeSide + 1u;
    EXPECT_FALSE( ValidateCloudProceduralParams( params ) ) << "a grid above the ceiling was accepted";

    params.VolumeSideVoxels = 0u;
    EXPECT_FALSE( ValidateCloudProceduralParams( params ) ) << "a grid of no voxels was accepted";
}

// THE RELATION THE WHOLE FEATURE STANDS ON, and it is not "the pixels match". A coarser grid is a coarser
// PICTURE and its bytes cannot equal the fine one's; what must survive is that it is a picture of the SAME
// SKY — the same clusters in the same places covering the same fraction of the ground.
//
// MEASURED AS THE TOP-DOWN COLUMN COVERAGE, which is the quantity the Coverage slider addresses and the
// one the sky's calibration is written against. A grid that had started placing clouds elsewhere, or
// growing them to fit itself, would move it.
TEST( CloudProceduralBudget, ACoarserGridIsTheSameSkySampledMoreCoarsely )
{
    struct Row
    {
        uint32_t Side;
        double   Columns;
    };
    std::vector<Row> rows;

    for ( const uint32_t side : { kCloudProceduralVolumeSide, 192u, kCloudProceduralVolumeSideMin } )
    {
        CloudProceduralFieldParams params = MakeParams();
        params.VolumeSideVoxels           = side;

        const glm::vec2 origin = CloudProceduralRegionOriginKm( params, 0.0f, 0.0f );
        const auto      baked  = BakeCloudProceduralVolume( params, origin );
        ASSERT_TRUE( baked ) << ( baked ? std::string{} : baked.GetError() );

        size_t columns = 0;
        for ( uint32_t z = 0; z < side; ++z )
            for ( uint32_t x = 0; x < side; ++x )
                for ( uint32_t y = 0; y < kCloudProceduralVolumeHeight; ++y )
                {
                    const size_t at =
                         ( ( static_cast<size_t>( z ) * kCloudProceduralVolumeHeight + y ) * side + x ) * 4u;
                    if ( baked.GetValue()[at] != 0u )
                    {
                        ++columns;
                        break;
                    }
                }

        rows.push_back( Row{ side, static_cast<double>( columns ) /
                                        static_cast<double>( static_cast<size_t>( side ) * side ) } );
    }

    for ( const Row& row : rows )
        std::printf( "[CloudProceduralBudget] side %3u -> %.4f of the sky covered\n", row.Side, row.Columns );

    // A HUNDREDTH OF THE SKY. The reference is the shipped 256, and the tolerance is a tenth of the 0.10
    // the Coverage slider itself is held to above — so a budget that moved the sky as much as a tenth of
    // one notch of Coverage fails here.
    //
    // AND IT IS WHERE THE FLOOR CAME FROM, which is worth saying because it is the one number in this
    // feature that a reader would otherwise assume was picked. The first draft of
    // kCloudProceduralVolumeSideMin was 64, derived from the cell floor; this assertion is what said no.
    // At 64 the same sky came out at 0.3613 covered against the reference's 0.3177 — four points of sky
    // that the CONTAINER invented, because the lump floor there is three times the narrowest body it is
    // applied to. The floor is 128 and the refusal of 64 is recorded on the constant itself.
    ASSERT_EQ( rows.size(), 3u );
    for ( size_t i = 1; i < rows.size(); ++i )
        EXPECT_NEAR( rows[i].Columns, rows[0].Columns, 0.01 )
             << "side " << rows[i].Side << " covers a different fraction of the sky than the shipped "
             << rows[0].Side << " does, so it is not the same sky sampled more coarsely";
}

// ---------------------------------------------------------------------------------------------------
// 8. THE PROGRESS HOOK (Г9) — a pure function with a callback in it is a claim somebody has to check
// ---------------------------------------------------------------------------------------------------

TEST( CloudProceduralBudget, BakingWithAProgressHookProducesTheIdenticalBytes )
{
    const CloudProceduralFieldParams params = MakeParams();
    const glm::vec2                  origin = CloudProceduralRegionOriginKm( params, 0.0f, 0.0f );

    const auto plain = BakeCloudProceduralVolume( params, origin );
    ASSERT_TRUE( plain ) << ( plain ? std::string{} : plain.GetError() );

    float last     = -1.0f;
    int   calls    = 0;
    bool  monotone = true;

    const auto hooked = BakeCloudProceduralVolume( params, origin,
                                                   [&]( float fraction )
                                                   {
                                                       monotone = monotone && fraction >= last;
                                                       last     = fraction;
                                                       ++calls;
                                                       return true;
                                                   } );
    ASSERT_TRUE( hooked ) << ( hooked ? std::string{} : hooked.GetError() );

    EXPECT_EQ( plain.GetValue(), hooked.GetValue() )
         << "the progress hook changed the bytes, so the bake is not the pure function it is documented as";

    // THE HOOK MUST ACTUALLY BE CALLED, or the cancellation test below is exercising a branch that never
    // runs — and a callback nobody invokes is the shape of a feature that reports itself as working.
    EXPECT_GT( calls, 1 ) << "the progress hook was never called during a bake of a whole region";
    EXPECT_TRUE( monotone ) << "the reported fraction went backwards";
    EXPECT_FLOAT_EQ( last, 1.0f ) << "a finished bake did not report itself finished";
}

TEST( CloudProceduralBudget, ACancelledBakeStopsEarlyAndSaysSoRatherThanReturningAPartialVolume )
{
    const CloudProceduralFieldParams params = MakeParams();
    const glm::vec2                  origin = CloudProceduralRegionOriginKm( params, 0.0f, 0.0f );

    int        calls     = 0;
    const auto cancelled = BakeCloudProceduralVolume( params, origin,
                                                      [&]( float )
                                                      {
                                                          ++calls;
                                                          return false; // stop at the first opportunity
                                                      } );

    // AN ERROR AND NOT AN EMPTY SUCCESS. Contract §1.4: a caller that cannot tell "nothing here" from
    // "stopped before finishing" reads the first as the second, and this one would upload a volume of
    // zeros as a sky.
    EXPECT_FALSE( cancelled ) << "a cancelled bake returned a volume";
    EXPECT_EQ( calls, 1 ) << "the bake carried on past a callback that said stop";
}

// THE PROFILE RISES THROUGH THE WHOLE BODY, AND THE CUT KEEPS ITS GRADIENT (PROFILE-BODY). A body depth is
// never under two voxels of the grid in its coarser direction, and the coverage remap's slope in the profile
// is at most 1 at every reach — together: the baked ramp spans at least two voxels however near its
// threshold a cluster stands, so the trilinear fetch never draws it as a facet.
// MUTATION: divide by `g` again in CloudProceduralCoverRemap and the slope row goes red; drop the floor in
// CloudProceduralBodyDepthKm and the depth row goes red.
TEST( CloudProceduralField, TheProfileRampSpansAtLeastTwoVoxelsAtEveryReach )
{
    const CloudProceduralFieldParams params = MakeParams();
    const float voxelKm = std::max( params.RegionSizeKm / static_cast<float>( params.VolumeSideVoxels ),
                                    params.LayerThicknessKm / static_cast<float>( kCloudProceduralVolumeHeight ) );

    for ( const float lumpKm : { 0.0f, 0.05f, 0.2f, 1.0f, 3.0f } )
        EXPECT_GE( CloudProceduralBodyDepthKm( params, lumpKm ), std::max( 2.0f * voxelKm, lumpKm ) - 1e-6f )
             << "a body " << lumpKm << " km deep is normalised over less than two voxels";

    const float step = 1.0f / 256.0f;
    for ( const float reach : { 0.02f, 0.1f, 0.3f, 0.7f, 1.0f } )
        for ( float profile = 0.0f; profile + step <= 1.0f; profile += step )
            EXPECT_LE( CloudProceduralCoverRemap( profile + step, reach ) -
                            CloudProceduralCoverRemap( profile, reach ),
                       step + 1e-6f )
                 << "the remap steepens the profile at reach " << reach << ", profile " << profile;

    EXPECT_EQ( CloudProceduralCoverRemap( 0.0f, 1.0f ), 0.0f ) << "air became cloud";
    EXPECT_EQ( CloudProceduralCoverRemap( 1.0f, 0.0f ), 0.0f ) << "a reach of zero drew cloud";
    EXPECT_FLOAT_EQ( CloudProceduralCoverRemap( 0.6f, 1.0f ), 0.6f ) << "a full reach changed the profile";
}

// THE SILHOUETTE NOISE TILES WITH THE VOLUME AND STAYS IN ITS RANGE (SHAPE-NOISE). It moves every lump's
// distance before the join, so a noise that did not repeat at RegionSizeKm would put a seam where the baked
// volume wraps, and the preview at a point would stop being the bake at its wrap. It also has to MOVE: a
// constant would leave every silhouette the ellipsoid join it was.
// MUTATION: drop the std::round in CloudProceduralShapeNoise's cell count and the period rows go red; make it
// return 0 and the variation row goes red.
TEST( CloudProceduralField, TheSilhouetteNoiseTilesWithTheVolumeAndVaries )
{
    const CloudProceduralFieldParams params = MakeParams();
    ASSERT_FALSE( params.Species.empty() );

    float lowest  = 1.0f;
    float highest = -1.0f;
    for ( uint32_t i = 0; i < 64u; ++i )
    {
        const glm::vec3 point( 0.37f + 0.731f * static_cast<float>( i ),
                               params.LayerBottomKm + 0.05f * static_cast<float>( i % 40u ),
                               1.13f + 0.419f * static_cast<float>( i ) );
        const float     here = CloudProceduralShapeNoise( params, 0u, point );
        EXPECT_GE( here, -1.0f );
        EXPECT_LE( here, 1.0f );
        EXPECT_NEAR( here, CloudProceduralShapeNoise( params, 0u, point + glm::vec3( params.RegionSizeKm, 0, 0 ) ),
                     1e-3f )
             << "the noise does not repeat along x at the region";
        EXPECT_NEAR( here, CloudProceduralShapeNoise( params, 0u, point - glm::vec3( 0, 0, params.RegionSizeKm ) ),
                     1e-3f )
             << "the noise does not repeat along z at the region";
        lowest  = std::min( lowest, here );
        highest = std::max( highest, here );
    }
    EXPECT_GT( highest - lowest, 0.5f ) << "the silhouette noise barely moves, so the bodies stay ellipsoids";
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}

// -------------------------------------------------------------------------------------------------------
// THE DDC KEY OF THE MODELLING VOLUME — every input the bake reads, and nothing it does not
// -------------------------------------------------------------------------------------------------------
namespace
{
    using Desert::Assets::CloudProceduralVolumeCacheKey;

    CloudProceduralFieldParams MakePaintedParams()
    {
        CloudProceduralFieldParams params = MakeParams();
        auto                       layout = std::make_shared<Desert::Assets::CloudLayoutData>();
        layout->ContentHash               = 0x1234abcdu;
        params.PatternSource              = layout;
        return params;
    }
} // namespace

TEST( CloudProceduralCacheKey, TheSameInputsGiveTheSameKeyAndACopyIsTheSameInputs )
{
    const glm::vec2                  origin( -24.0f, -24.0f );
    const CloudProceduralFieldParams params = MakePaintedParams();
    // The copy IS the subject: a key that hashed an address or padding would differ between the two.
    // NOLINTNEXTLINE(performance-unnecessary-copy-initialization)
    const CloudProceduralFieldParams copy = params;
    EXPECT_EQ( CloudProceduralVolumeCacheKey( params, origin ), CloudProceduralVolumeCacheKey( params, origin ) );
    EXPECT_EQ( CloudProceduralVolumeCacheKey( params, origin ), CloudProceduralVolumeCacheKey( copy, origin ) );

    // By value, as CloudProceduralParamsEqual compares: -0 and +0 are one authored number.
    CloudProceduralFieldParams negativeZero     = params;
    negativeZero.Species[0].Shape.AnvilStrength = -0.0f;
    ASSERT_TRUE( Desert::Assets::CloudProceduralParamsEqual( params, negativeZero ) );
    EXPECT_EQ( CloudProceduralVolumeCacheKey( params, origin ),
               CloudProceduralVolumeCacheKey( negativeZero, origin ) );
}

TEST( CloudProceduralCacheKey, EveryFieldTheComparisonSeesChangesTheKey )
{
    using Params = CloudProceduralFieldParams;
    const glm::vec2 origin( -24.0f, -24.0f );
    const Params    base = MakePaintedParams();

    // One row per field CloudProceduralParamsEqual compares, on a base WITH a painting (the only state in
    // which the placement is read). Each row must also be a change the comparison sees, or the row proves
    // nothing about the two agreeing.
    const std::vector<std::pair<const char*, std::function<void( Params& )>>> rows = {
         { "VolumeSideVoxels", []( Params& p ) { p.VolumeSideVoxels /= 2u; } },
         { "RegionSizeKm", []( Params& p ) { p.RegionSizeKm += 1.0f; } },
         { "LayerBottomKm", []( Params& p ) { p.LayerBottomKm += 0.1f; } },
         { "LayerThicknessKm", []( Params& p ) { p.LayerThicknessKm += 0.1f; } },
         { "BlendRadiusKm", []( Params& p ) { p.BlendRadiusKm += 0.01f; } },
         { "Coverage", []( Params& p ) { p.Coverage += 0.01f; } },
         { "CoverageContrast", []( Params& p ) { p.CoverageContrast += 0.1f; } },
         { "Seed", []( Params& p ) { p.Seed += 1u; } },
         { "WindAxis.x", []( Params& p ) { p.WindAxis.x += 0.1f; } },
         { "WindAxis.y", []( Params& p ) { p.WindAxis.y += 0.1f; } },
         { "ResolvableChordKm", []( Params& p ) { p.ResolvableChordKm += 0.01f; } },
         { "PlacementDensity", []( Params& p ) { p.PlacementDensity += 0.1f; } },
         { "PlacementScatter", []( Params& p ) { p.PlacementScatter -= 0.1f; } },
         { "PlacementSizeVariety", []( Params& p ) { p.PlacementSizeVariety += 0.1f; } },
         { "PatchStrength", []( Params& p ) { p.PatchStrength += 0.1f; } },
         { "PatchTileKm", []( Params& p ) { p.PatchTileKm += 1.0f; } },
         { "PatternSource hash",
           []( Params& p )
           {
               auto other         = std::make_shared<Desert::Assets::CloudLayoutData>( *p.PatternSource );
               other->ContentHash = 0x9999u;
               p.PatternSource    = other;
           } },
         { "MaskSource hash",
           []( Params& p )
           {
               auto mask         = std::make_shared<Desert::Assets::CloudLayoutData>();
               mask->ContentHash = 0x5555u;
               p.MaskSource      = mask;
           } },
         { "LayoutPlacement.RepeatsPerRegion", []( Params& p ) { p.LayoutPlacement.RepeatsPerRegion += 1u; } },
         { "LayoutPlacement.QuarterTurns", []( Params& p ) { p.LayoutPlacement.QuarterTurns += 1u; } },
         { "LayoutPlacement.OffsetKm.x", []( Params& p ) { p.LayoutPlacement.OffsetKm.x += 1.0f; } },
         { "LayoutPlacement.OffsetKm.y", []( Params& p ) { p.LayoutPlacement.OffsetKm.y += 1.0f; } },
         { "LayoutPlacement.PatternStrength", []( Params& p ) { p.LayoutPlacement.PatternStrength -= 0.1f; } },
         { "LayoutPlacement.MaskStrength", []( Params& p ) { p.LayoutPlacement.MaskStrength -= 0.1f; } },
         { "Species count", []( Params& p ) { p.Species.push_back( p.Species[0] ); } },
         { "Species.CellKm", []( Params& p ) { p.Species[0].CellKm += 0.5f; } },
         { "Species.Anisotropy", []( Params& p ) { p.Species[0].Anisotropy += 0.1f; } },
         { "Shape.BaseAltitudeKm", []( Params& p ) { p.Species[0].Shape.BaseAltitudeKm += 0.1f; } },
         { "Shape.TopAltitudeKm", []( Params& p ) { p.Species[0].Shape.TopAltitudeKm += 0.1f; } },
         { "Shape.EdgeTopFraction", []( Params& p ) { p.Species[0].Shape.EdgeTopFraction += 0.1f; } },
         { "Shape.BaseRampFraction", []( Params& p ) { p.Species[0].Shape.BaseRampFraction += 0.1f; } },
         { "Shape.Profile first", []( Params& p ) { p.Species[0].Shape.Profile.HalfWidth.front() += 0.01f; } },
         { "Shape.Profile last", []( Params& p ) { p.Species[0].Shape.Profile.HalfWidth.back() += 0.01f; } },
         { "Shape.AnvilAltitudeKm", []( Params& p ) { p.Species[0].Shape.AnvilAltitudeKm += 0.1f; } },
         { "Shape.AnvilThicknessKm", []( Params& p ) { p.Species[0].Shape.AnvilThicknessKm += 0.1f; } },
         { "Shape.AnvilStrength", []( Params& p ) { p.Species[0].Shape.AnvilStrength += 0.1f; } },
         { "Shape.DetailCharacter", []( Params& p ) { p.Species[0].Shape.DetailCharacter -= 0.1f; } },
         { "Shape.DetailFactor", []( Params& p ) { p.Species[0].Shape.DetailFactor += 0.1f; } },
         { "Shape.DensityFactor", []( Params& p ) { p.Species[0].Shape.DensityFactor += 0.1f; } },
         { "Shape.ExtinctionFactor", []( Params& p ) { p.Species[0].Shape.ExtinctionFactor += 0.1f; } },
         { "Shape.PlacementScale", []( Params& p ) { p.Species[0].Shape.PlacementScale += 0.1f; } },
         { "Shape.PlacementAnisotropy", []( Params& p ) { p.Species[0].Shape.PlacementAnisotropy += 0.1f; } },
    };

    const uint64_t baseKey = CloudProceduralVolumeCacheKey( base, origin );
    for ( const auto& [name, edit] : rows )
    {
        Params changed = base;
        edit( changed );
        ASSERT_FALSE( Desert::Assets::CloudProceduralParamsEqual( base, changed ) ) << name;
        EXPECT_NE( CloudProceduralVolumeCacheKey( changed, origin ), baseKey ) << name << " is not in the key";
    }

    // The region origin is not a parameter but is an input: the same sky over the next region is other bytes.
    EXPECT_NE( CloudProceduralVolumeCacheKey( base, origin + glm::vec2( 3.0f, 0.0f ) ), baseKey );
    EXPECT_NE( CloudProceduralVolumeCacheKey( base, origin + glm::vec2( 0.0f, 3.0f ) ), baseKey );
}

TEST( CloudProceduralCacheKey, ThePlacementIsKeyedOnlyWhenAPaintingIsBound )
{
    // Unpainted, the bake never reads the placement, and CloudProceduralParamsEqual says "same" — so a key
    // that differed would miss on a sky whose bytes are identical.
    const glm::vec2                  origin( 0.0f, 0.0f );
    const CloudProceduralFieldParams unpainted = MakeParams();
    CloudProceduralFieldParams       moved     = unpainted;
    moved.LayoutPlacement.RepeatsPerRegion += 3u;
    ASSERT_TRUE( Desert::Assets::CloudProceduralParamsEqual( unpainted, moved ) );
    EXPECT_EQ( CloudProceduralVolumeCacheKey( unpainted, origin ),
               CloudProceduralVolumeCacheKey( moved, origin ) );
}

TEST( CloudProceduralCacheKey, TheDeriverVersionChangesTheKey )
{
    constexpr const auto&          kCurrent = Desert::Assets::kCloudModellingDeriver;
    constexpr Common::DDC::Deriver kNextVersion{
         kCurrent.Bucket, kCurrent.Extension, { kCurrent.Version.Hi, kCurrent.Version.Lo + 1u } };
    const CloudProceduralFieldParams params = MakeParams();
    const glm::vec2                  origin( 0.0f, 0.0f );
    EXPECT_NE( CloudProceduralVolumeCacheKey( params, origin ),
               CloudProceduralVolumeCacheKey( params, origin, kNextVersion ) );
}
