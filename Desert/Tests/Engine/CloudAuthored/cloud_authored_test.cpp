// Slot A of the cloud field's seam: the sculpted body, its container, the union that joins it to the
// procedural producer, and the cutout that keeps the procedural producer out of it.
//
// WHAT THIS SUITE IS FOR, said as the defect it prevents. The seam has two sides that can drift and
// nothing else checks them:
//
//   * the GENERATOR writes voxels and the SHADER addresses them — two statements of one layout, in two
//     languages, neither able to see the other. `VolumeAndItsReadingAgree` is that relation.
//   * the C++ payload writes bytes into a storage buffer and the shader reads a struct out of it. The
//     reference header asserts that by copying the BYTES rather than the members.
//   * the union is claimed to be order-independent, which is the property that lets an artist sculpt in
//     any order and a scene list its hero clouds in any order. Two tests assert it: one on the bake
//     (shuffle the lumps) and one on the seam (swap the instances).
//
// AND ONE CLAIM THAT IS THE WHOLE POINT OF THE PHASE. The procedural field cannot make a fused mass —
// the Alligator's `best - second` lays a zero between every pair of cells by construction — so the
// accepting frame of Э4 is a cloud that IS one connected body. `TheBodyIsOneConnectedMass` measures
// exactly that, on the shipped volume, by flood fill.
//
// GPU-free: the shader text is compiled as C++ (see CloudAuthoredReference.hpp).

#include "CloudAuthoredReference.hpp"

#include <Common/Core/Constants.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <numeric>
#include <random>
#include <span>
#include <sstream>
#include <string>
#include <vector>

using namespace Desert::Tests::CloudAuthoredRef;

namespace
{
    namespace Assets  = Desert::Assets;
    namespace Graphic = Desert::Graphic;

    constexpr float kLayerBottomKm    = 2.2f; // the shipped cumulus congestus' base
    constexpr float kLayerThicknessKm = 3.6f; // ... and 5.8 - 2.2, its own envelope

    /// A hero cloud placed at a field-space position, with an optional rotation about Y and a uniform
    /// scale. Built through the packer the renderer uses, never by hand: what is under test includes the
    /// packer.
    Graphic::CloudAuthoredInstanceGpu MakeInstance( glm::vec3 centreFieldKm, float scale = 1.0f,
                                                    float yawRadians = 0.0f, float strength = 1.0f,
                                                    bool cutout = false, uint32_t slot = 0u,
                                                    uint32_t slabCount = 1u )
    {
        Desert::ECS::HeroCloudData data;
        data.Strength                = strength;
        data.SuppressProceduralField = cutout;
        data.DetailFactor            = 1.0f;
        data.DensityFactor           = 1.0f;
        data.ExtinctionFactor        = 1.0f;

        const glm::vec3 worldCm{ centreFieldKm.x * Graphic::kCloudWorldUnitsPerKm,
                                 ( centreFieldKm.y + kLayerBottomKm ) * Graphic::kCloudWorldUnitsPerKm,
                                 centreFieldKm.z * Graphic::kCloudWorldUnitsPerKm };

        const float c = std::cos( yawRadians );
        const float s = std::sin( yawRadians );

        glm::mat4 transform( 1.0f );
        transform[0] = glm::vec4( c * scale, 0.0f, -s * scale, 0.0f );
        transform[1] = glm::vec4( 0.0f, scale, 0.0f, 0.0f );
        transform[2] = glm::vec4( s * scale, 0.0f, c * scale, 0.0f );
        transform[3] = glm::vec4( worldCm, 1.0f );

        const auto packed =
             Graphic::PackCloudAuthoredInstance( transform, Body().Recipe.SizeKm, kLayerBottomKm, data,
                                                 Graphic::CloudAuthoredAtlasSlabBaseW( slot, slabCount ) );
        EXPECT_TRUE( packed.Valid );
        return packed.Instance;
    }

    /// An instance of a CATALOGUE body, which is what the atlas tests need: the shipped example alone
    /// cannot tell "read slab 1" from "read slab 0", because both slabs would hold the same cloud.
    Graphic::CloudAuthoredInstanceGpu MakeCatalogueInstance( Assets::CloudModellingSpecies species,
                                                             glm::vec3 centreFieldKm, float scale, uint32_t slot,
                                                             uint32_t slabCount )
    {
        Desert::ECS::HeroCloudData data;
        data.Strength                = 1.0f;
        data.SuppressProceduralField = false;

        const glm::vec3 worldCm{ centreFieldKm.x * Graphic::kCloudWorldUnitsPerKm,
                                 ( centreFieldKm.y + kLayerBottomKm ) * Graphic::kCloudWorldUnitsPerKm,
                                 centreFieldKm.z * Graphic::kCloudWorldUnitsPerKm };

        glm::mat4 transform( 1.0f );
        transform[0] = glm::vec4( scale, 0.0f, 0.0f, 0.0f );
        transform[1] = glm::vec4( 0.0f, scale, 0.0f, 0.0f );
        transform[2] = glm::vec4( 0.0f, 0.0f, scale, 0.0f );
        transform[3] = glm::vec4( worldCm, 1.0f );

        const auto packed = Graphic::PackCloudAuthoredInstance(
             transform, Assets::CloudModellingCatalogueRecipe( species ).SizeKm, kLayerBottomKm, data,
             Graphic::CloudAuthoredAtlasSlabBaseW( slot, slabCount ) );
        EXPECT_TRUE( packed.Valid );
        return packed.Instance;
    }

    /// A field position where the PROCEDURAL producer has something in it, found rather than assumed: the
    /// coverage the shipped scene uses puts cloud over about a quarter of the sky, so a hardcoded point
    /// would be a point that stops being inside a cloud the first time anything is retuned.
    bool FindProceduralPoint( const CloudFieldParams& params, float heightFraction, glm::vec3& outPositionKm )
    {
        for ( int ix = -40; ix <= 40; ++ix )
        {
            for ( int iz = -40; iz <= 40; ++iz )
            {
                const glm::vec3 position( static_cast<float>( ix ) * 0.5f, heightFraction * kLayerThicknessKm,
                                          static_cast<float>( iz ) * 0.5f );

                ClearInstances();
                if ( SampleCloudField( params, heightFraction, position ).Profile > 0.2f )
                {
                    outPositionKm = position;
                    return true;
                }
            }
        }
        return false;
    }

    /// A point inside @p gpu's body where the AUTHORED profile is small, the ENVELOPE is saturated, and
    /// the PROCEDURAL field is bigger than the authored one — which is the only place in the sky where the
    /// cutout is observable at all.
    ///
    /// WHY IT HAS TO BE SEARCHED FOR, and this is the lesson the first version of these tests taught. At
    /// the body's centre the authored profile is 1, so `max` keeps it whether the cutout fired or not and
    /// the test passes with the cutout deleted. Winding the body down with Strength does not help either:
    /// Strength scales the cutout WITH the body by design, so a faint body has a faint cutout. What is
    /// left is the body's own OUTER SHELL — barely inside the surface, so the profile is a few per cent,
    /// and well inside the dilated envelope, so the cutout is full.
    bool FindCutoutPoint( const CloudFieldParams& params, const Graphic::CloudAuthoredInstanceGpu& gpu,
                          float fraction, glm::vec3& outPointKm, float& outAuthored )
    {
        ClearInstances();
        AddInstance( gpu );
        const CloudAuthoredInstance instance = CloudAuthoredInstanceAt( 0 );

        for ( int ix = 0; ix <= 40; ++ix )
        {
            for ( int iz = 0; iz <= 40; ++iz )
            {
                const glm::vec3 point( mix( gpu.BoundsMin.x, gpu.BoundsMax.x, static_cast<float>( ix ) / 40.0f ),
                                       fraction * kLayerThicknessKm,
                                       mix( gpu.BoundsMin.z, gpu.BoundsMax.z, static_cast<float>( iz ) / 40.0f ) );

                if ( !CloudAuthoredInVolume( CloudAuthoredLocalUvw( instance, point ) ) )
                    continue;

                const vec4 voxel = CLOUD_SAMPLE_AUTHORED( CloudAuthoredAtlasUvw(
                     CloudAuthoredLocalUvw( instance, point ), instance.BoundsMax.w, CLOUD_AUTHORED_SLAB_COUNT ) );

                if ( voxel.w < 0.98f || voxel.x <= 0.0f || voxel.x > 0.35f )
                    continue;

                ClearInstances();
                const float procedural = SampleCloudField( params, fraction, point ).Profile;

                ClearInstances();
                AddInstance( gpu );

                if ( procedural < voxel.x + 0.15f )
                    continue;

                outPointKm  = point;
                outAuthored = voxel.x;
                return true;
            }
        }
        return false;
    }

    size_t VoxelIndex( uint32_t x, uint32_t y, uint32_t z )
    {
        return ( ( static_cast<size_t>( z ) * Assets::kCloudModellingVolumeHeight + y ) *
                      Assets::kCloudModellingVolumeWidth +
                 x ) *
               Assets::kCloudModellingBytesPerVoxel;
    }
} // namespace

// ---------------------------------------------------------------------------------------------------
// The container and the generator
// ---------------------------------------------------------------------------------------------------

TEST( CloudModellingVolume, TheShippedRecipeBakes )
{
    const auto& body = Body();
    ASSERT_EQ( body.Voxels.size(), Assets::kCloudModellingVoxelBytes );
    EXPECT_EQ( body.Voxels.size(), 128u * 64u * 128u * 4u );

    // 4.00 MiB exactly, which is the arithmetic PLAN_AUTHORED_CLOUDS.md section 2 is built on.
    EXPECT_NEAR( static_cast<double>( body.Voxels.size() ) / ( 1024.0 * 1024.0 ), 4.0, 1e-9 );
}

TEST( CloudModellingVolume, TheContainerRoundTrips )
{
    Assets::CloudModellingVolumeData data = Body();

    const std::vector<unsigned char> encoded = Assets::EncodeCloudModellingPayload( data );

    // The payload's size is a formula and not an observation: a header that grows without
    // kCloudModellingHeaderSize moving would pass a test that meant nothing.
    EXPECT_EQ( encoded.size(), Assets::kCloudModellingHeaderSize +
                                    data.Recipe.Blobs.size() * Assets::kCloudModellingBlobBytes +
                                    Assets::kCloudModellingVoxelBytes );

    const auto decoded = Assets::DecodeCloudModellingPayload( encoded );
    ASSERT_TRUE( decoded ) << decoded.GetError();

    const Assets::CloudModellingVolumeData& read = decoded.GetValue();
    EXPECT_EQ( read.Voxels, data.Voxels );
    EXPECT_EQ( read.Recipe.Blobs.size(), data.Recipe.Blobs.size() );
    EXPECT_FLOAT_EQ( read.Recipe.SizeKm.x, data.Recipe.SizeKm.x );
    EXPECT_FLOAT_EQ( read.Recipe.SizeKm.y, data.Recipe.SizeKm.y );
    EXPECT_FLOAT_EQ( read.Recipe.SizeKm.z, data.Recipe.SizeKm.z );
    EXPECT_FLOAT_EQ( read.Recipe.BlendRadiusKm, data.Recipe.BlendRadiusKm );
    EXPECT_FLOAT_EQ( read.Recipe.ProfileDepthKm, data.Recipe.ProfileDepthKm );
    EXPECT_FLOAT_EQ( read.Recipe.EnvelopeMarginKm, data.Recipe.EnvelopeMarginKm );

    for ( size_t i = 0; i < read.Recipe.Blobs.size(); ++i )
    {
        EXPECT_FLOAT_EQ( read.Recipe.Blobs[i].CentreKm.y, data.Recipe.Blobs[i].CentreKm.y ) << "lump " << i;
        EXPECT_FLOAT_EQ( read.Recipe.Blobs[i].RadiiKm.x, data.Recipe.Blobs[i].RadiiKm.x ) << "lump " << i;
        EXPECT_FLOAT_EQ( read.Recipe.Blobs[i].DetailType, data.Recipe.Blobs[i].DetailType ) << "lump " << i;
        EXPECT_FLOAT_EQ( read.Recipe.Blobs[i].DensityScale, data.Recipe.Blobs[i].DensityScale ) << "lump " << i;
    }
}

TEST( CloudModellingVolume, ACorruptPayloadIsRefusedRatherThanRead )
{
    std::vector<unsigned char> encoded = Assets::EncodeCloudModellingPayload( Body() );

    // One bit, in the middle of the voxels. Length is unchanged, so only the checksum can catch it — and
    // the failure it would otherwise produce is a cloud with a wrong edge, which reads as a tuning
    // problem rather than as a corrupt file.
    encoded[encoded.size() / 2] ^= 0x01u;

    const auto decoded = Assets::DecodeCloudModellingPayload( encoded );
    EXPECT_FALSE( decoded );
    EXPECT_NE( decoded.GetError().find( "checksum" ), std::string::npos ) << decoded.GetError();
}

namespace
{
    // An envelope written by hand, so a test can state a kind or a version the encoder never would.
    std::vector<unsigned char> HandWrittenEnvelope( Common::Content::ContentKind kind, uint32_t version )
    {
        namespace CC                             = Common::Content;
        const std::vector<unsigned char> payload = Assets::EncodeCloudModellingPayload( Body() );
        CC::AssetEnvelope                envelope;
        envelope.Asset.Kind                           = kind;
        envelope.Asset.Guid                           = CC::AssetGuid::Generate();
        envelope.Asset.Subsystems                     = { { Assets::kCloudModellingSubsystemTag, version } };
        const std::span<const std::byte> payloadBytes = std::as_bytes( std::span( payload ) );
        envelope.Sections.push_back( { CC::EnvelopeSection::Payload, CC::EnvelopeCodec::Stored,
                                       std::vector<std::byte>( payloadBytes.begin(), payloadBytes.end() ) } );
        const auto file = CC::WriteAssetEnvelope( envelope );
        EXPECT_TRUE( file ) << file.GetError();
        if ( !file )
            return {};
        std::vector<unsigned char> out( file.GetValue().size() );
        std::memcpy( out.data(), file.GetValue().data(), file.GetValue().size() );
        return out;
    }
} // namespace

TEST( CloudModellingVolume, TheEnvelopeStatesKindGuidAndTheContainerVersionUnderDcmv )
{
    // A volume never written has a null GUID; the encoder mints one, and a re-encode of what it decoded
    // keeps it - the property every reference to the volume leans on.
    const auto first = Assets::EncodeCloudModellingVolume( Body() );
    ASSERT_TRUE( first ) << first.GetError();

    namespace CC                        = Common::Content;
    const CC::SubsystemVersion kKnown[] = {
         { Assets::kCloudModellingSubsystemTag, Assets::kCloudModellingContainerVersion } };
    const auto header = CC::ReadEnvelopeHeader( std::as_bytes( std::span( first.GetValue() ) ),
                                                CC::AssetHeaderReadContext{ kKnown } );
    ASSERT_TRUE( header ) << header.GetError();
    EXPECT_EQ( header.GetValue().Asset.Kind, CC::ContentKind::CloudModellingVolume );
    EXPECT_FALSE( header.GetValue().Asset.Guid.IsNull() );
    ASSERT_EQ( header.GetValue().Asset.Subsystems.size(), 1u );
    EXPECT_EQ( header.GetValue().Asset.Subsystems[0].Tag, Assets::kCloudModellingSubsystemTag );
    EXPECT_EQ( header.GetValue().Asset.Subsystems[0].Version, 3u );

    const auto decoded = Assets::DecodeCloudModellingVolume( first.GetValue() );
    ASSERT_TRUE( decoded ) << decoded.GetError();
    EXPECT_EQ( decoded.GetValue().Guid, header.GetValue().Asset.Guid );
    const auto second = Assets::EncodeCloudModellingVolume( decoded.GetValue() );
    ASSERT_TRUE( second ) << second.GetError();
    EXPECT_EQ( second.GetValue(), first.GetValue() ) << "a decode/encode round trip must be byte-identical";
}

TEST( CloudModellingVolume, ABareContainerIsRefusedByNameAndNamesTheMigrator )
{
    // The version-2 shape: magic, container version 2, then the payload. It has no GUID to give the volume.
    std::vector<unsigned char> bare( Assets::kCloudModellingMagic,
                                     Assets::kCloudModellingMagic + sizeof( Assets::kCloudModellingMagic ) );
    bare.insert( bare.end(), { 2u, 0u, 0u, 0u } );
    const std::vector<unsigned char> payload = Assets::EncodeCloudModellingPayload( Body() );
    bare.insert( bare.end(), payload.begin(), payload.end() );

    const auto decoded = Assets::DecodeCloudModellingVolume( bare );
    ASSERT_FALSE( decoded );
    EXPECT_NE( decoded.GetError().find( "bare 'DCMV' container version 2" ), std::string::npos )
         << decoded.GetError();
    EXPECT_NE( decoded.GetError().find( "SceneMigrator" ), std::string::npos ) << decoded.GetError();
}

TEST( CloudModellingVolume, AFutureContainerVersionIsRefusedByNumberRatherThanMisread )
{
    const auto file    = HandWrittenEnvelope( Common::Content::ContentKind::CloudModellingVolume,
                                              Assets::kCloudModellingContainerVersion + 7u );
    const auto decoded = Assets::DecodeCloudModellingVolume( file );
    ASSERT_FALSE( decoded );
    EXPECT_NE( decoded.GetError().find( std::to_string( Assets::kCloudModellingContainerVersion + 7u ) ),
               std::string::npos )
         << decoded.GetError();
}

TEST( CloudModellingVolume, AnEnvelopeOfAnotherKindIsRefusedByItsKind )
{
    // The payload is a perfectly good modelling volume; only the envelope says it is a noise volume.
    const auto file    = HandWrittenEnvelope( Common::Content::ContentKind::CloudNoiseVolume,
                                              Assets::kCloudModellingContainerVersion );
    const auto decoded = Assets::DecodeCloudModellingVolume( file );
    ASSERT_FALSE( decoded );
    EXPECT_NE( decoded.GetError().find( "not CloudModellingVolume" ), std::string::npos ) << decoded.GetError();

    std::istringstream in( std::string( file.begin(), file.end() ) );
    const auto         guid = Assets::ReadCloudModellingVolumeGuid( in );
    ASSERT_FALSE( guid );
    EXPECT_NE( guid.GetError().find( "not CloudModellingVolume" ), std::string::npos ) << guid.GetError();
}

TEST( CloudModellingVolume, TheJoinIsOrderIndependent )
{
    // COMMUTATIVE AND ASSOCIATIVE is the first of the three properties the exponential smooth-min was
    // chosen for (PLAN_AUTHORED_CLOUDS.md section 3), and it is what lets an artist sculpt in any order
    // and a baker store no order. Asserted on the BYTES, which is the strongest form available: not
    // "close enough", identical.
    Assets::CloudModellingVolumeRecipe shuffled = Body().Recipe;

    std::mt19937 rng( 20260820u );
    std::shuffle( shuffled.Blobs.begin(), shuffled.Blobs.end(), rng );

    const auto rebaked = Assets::GenerateCloudModellingVolume( shuffled );
    ASSERT_TRUE( rebaked ) << rebaked.GetError();

    size_t      differing = 0;
    int         maxDelta  = 0;
    const auto& before    = Body().Voxels;
    const auto& after     = rebaked.GetValue();
    for ( size_t i = 0; i < before.size(); ++i )
    {
        const int delta = std::abs( static_cast<int>( before[i] ) - static_cast<int>( after[i] ) );
        if ( delta != 0 )
        {
            ++differing;
            maxDelta = std::max( maxDelta, delta );
        }
    }
    EXPECT_EQ( differing, 0u ) << "shuffling the lumps moved " << differing << " bytes, by up to " << maxDelta;
}

TEST( CloudModellingVolume, TheBodyIsOneConnectedMass )
{
    // THE ACCEPTING CLAIM OF THE WHOLE PHASE, measured rather than asserted.
    //
    // The procedural producer cannot make this shape and the reason is structural: its coverage field is
    // an Alligator, `best - second`, which is exactly zero on the bisector between every pair of feature
    // points — so two neighbouring lobes are ALWAYS separated by a surface of zeroes and a threshold can
    // never join them. Three tasks measured that independently before this phase was approved.
    //
    // A smooth-min union has the opposite property by construction, and this is what that means in the
    // shipped volume: every voxel with any body in it belongs to ONE six-connected component.
    const auto& body = Body();

    const uint32_t width  = Assets::kCloudModellingVolumeWidth;
    const uint32_t height = Assets::kCloudModellingVolumeHeight;
    const uint32_t depth  = Assets::kCloudModellingVolumeDepth;

    std::vector<char> visited( static_cast<size_t>( width ) * height * depth, 0 );

    const auto solid = [&]( uint32_t x, uint32_t y, uint32_t z )
    { return body.Voxels[VoxelIndex( x, y, z )] > 0u; };

    const auto flat = [&]( uint32_t x, uint32_t y, uint32_t z )
    { return ( static_cast<size_t>( z ) * height + y ) * width + x; };

    size_t                               components = 0;
    size_t                               solidCount = 0;
    std::vector<std::array<uint32_t, 3>> stack;

    for ( uint32_t z = 0; z < depth; ++z )
    {
        for ( uint32_t y = 0; y < height; ++y )
        {
            for ( uint32_t x = 0; x < width; ++x )
            {
                if ( !solid( x, y, z ) || visited[flat( x, y, z )] )
                    continue;

                ++components;
                stack.push_back( { x, y, z } );
                visited[flat( x, y, z )] = 1;

                while ( !stack.empty() )
                {
                    const auto voxel = stack.back();
                    stack.pop_back();
                    ++solidCount;

                    const int offsets[6][3] = { { 1, 0, 0 },  { -1, 0, 0 }, { 0, 1, 0 },
                                                { 0, -1, 0 }, { 0, 0, 1 },  { 0, 0, -1 } };
                    for ( const auto& offset : offsets )
                    {
                        const long nx = static_cast<long>( voxel[0] ) + offset[0];
                        const long ny = static_cast<long>( voxel[1] ) + offset[1];
                        const long nz = static_cast<long>( voxel[2] ) + offset[2];

                        if ( nx < 0 || ny < 0 || nz < 0 || nx >= width || ny >= height || nz >= depth )
                            continue;

                        const uint32_t ux = static_cast<uint32_t>( nx );
                        const uint32_t uy = static_cast<uint32_t>( ny );
                        const uint32_t uz = static_cast<uint32_t>( nz );

                        if ( !solid( ux, uy, uz ) || visited[flat( ux, uy, uz )] )
                            continue;

                        visited[flat( ux, uy, uz )] = 1;
                        stack.push_back( { ux, uy, uz } );
                    }
                }
            }
        }
    }

    EXPECT_EQ( components, 1u ) << "the sculpted body has fallen into " << components
                                << " pieces; lumps that do not overlap are a string of beads, not a cumulus";

    // And it is a body rather than a speck: about a tenth of the box, which is what a cumulus of these
    // proportions occupies in a box drawn round it.
    const double occupancy =
         static_cast<double>( solidCount ) / static_cast<double>( static_cast<size_t>( width ) * height * depth );
    EXPECT_GT( occupancy, 0.04 );
    EXPECT_LT( occupancy, 0.30 );
}

TEST( CloudModellingVolume, TheBlendRadiusAddsMaterialInTheCreases )
{
    // WHAT THE CONNECTIVITY TEST ABOVE DOES *NOT* MEASURE, found by breaking it: dropping the blend
    // radius to a micrometre leaves the body in one piece, because these lumps OVERLAP and a union of
    // overlapping ellipsoids is connected at any sharpness. Connectivity is a property of the recipe;
    // smoothness is the property of the JOIN, and it needs its own assertion or the third of the three
    // reasons the exponential smooth-min was chosen is untested.
    //
    // The measurable consequence is that the join is a strict LOWER bound on the distance — `smin <= min`
    // — so a wider blend radius pushes the surface outward and fills the creases where two lumps meet.
    // More blend, more body, monotonically.
    Assets::CloudModellingVolumeRecipe sharp = Body().Recipe;
    sharp.BlendRadiusKm                      = 0.001f;

    const auto sharpBake = Assets::GenerateCloudModellingVolume( sharp );
    ASSERT_TRUE( sharpBake ) << sharpBake.GetError();

    const auto solidCount = []( const std::vector<unsigned char>& voxels )
    {
        size_t n = 0;
        for ( size_t i = 0; i < voxels.size(); i += 4 )
        {
            if ( voxels[i] > 0u )
                ++n;
        }
        return n;
    };

    const size_t smooth = solidCount( Body().Voxels );
    const size_t hard   = solidCount( sharpBake.GetValue() );

    EXPECT_GT( smooth, hard ) << "the 50 m blend radius added no material at all against a 1 m one, so the "
                                 "join is behaving like a plain min and the creases between the lumps are "
                                 "sharp";
}

TEST( CloudModellingVolume, TheEnvelopeIsConservative )
{
    // The cutout reads the envelope, so it MUST cover the body: a voxel with profile and no envelope is a
    // piece of cloud the procedural field is allowed to grow through, which is the exact defect the
    // cutout exists to prevent.
    const auto& body = Body();

    size_t violations = 0;
    for ( size_t i = 0; i < body.Voxels.size(); i += 4 )
    {
        if ( body.Voxels[i] > 0u && body.Voxels[i + 3] == 0u )
            ++violations;
    }

    EXPECT_EQ( violations, 0u );
}

TEST( CloudModellingVolume, ValidateRefusesABodyThatWouldTouchItsBox )
{
    Assets::CloudModellingVolumeRecipe recipe = Body().Recipe;

    // A BOX THE LUMPS THEMSELVES FIT IN, and nothing else. The recipe reaches 0.76 x 0.30 x 0.54 km from
    // its centre; the join inflates that by 0.104 km and the envelope dilates it by another 0.09, so a
    // half-size of 0.85 x 0.40 x 0.65 accepts the lumps and must still refuse the body.
    //
    // Sized this way rather than halved, because a halved box is refused even with the slack term
    // deleted — which is what breaking the slack term proved, and it means the halved version was testing
    // the wrong half of the arithmetic.
    recipe.SizeKm = glm::vec3( 1.70f, 0.80f, 1.30f );

    const auto valid = Assets::ValidateCloudModellingRecipe( recipe );
    EXPECT_FALSE( valid );
    EXPECT_NE( valid.GetError().find( "does not fit" ), std::string::npos ) << valid.GetError();
}

TEST( CloudModellingVolume, ThePathsTheAssetSystemAndTheFormatAgreeOn )
{
    // Two statements of one directory, so it is a test rather than a hope — the same relation
    // Desert/Tests/Engine/CloudType asserts for the type library.
    const std::string full = Common::Constants::Path::CLOUD_VOLUME_PATH.generic_string();
    const std::string tail = Assets::kCloudModellingAssetsRelativeDir;

    ASSERT_GE( full.size(), tail.size() );
    EXPECT_EQ( full.compare( full.size() - tail.size(), tail.size(), tail ), 0 ) << full << " vs " << tail;
}

// ---------------------------------------------------------------------------------------------------
// The volume and its reading
// ---------------------------------------------------------------------------------------------------

TEST( CloudAuthored, TheShaderAndTheEngineAgreeOnTheVolumeShape )
{
    EXPECT_EQ( static_cast<uint32_t>( CLOUD_MODELLING_VOLUME_WIDTH ), Assets::kCloudModellingVolumeWidth );
    EXPECT_EQ( static_cast<uint32_t>( CLOUD_MODELLING_VOLUME_HEIGHT ), Assets::kCloudModellingVolumeHeight );
    EXPECT_EQ( static_cast<uint32_t>( CLOUD_MODELLING_VOLUME_DEPTH ), Assets::kCloudModellingVolumeDepth );
    EXPECT_EQ( static_cast<uint32_t>( CLOUD_AUTHORED_SLOTS ), Graphic::kCloudAuthoredSlots );
}

TEST( CloudAuthored, VolumeAndItsReadingAgree )
{
    // THE RELATION THIS SUITE EXISTS FOR. The generator writes `((z * H + y) * W + x) * 4` and the shader
    // addresses `uvw * extent - 0.5`; both are individually reasonable and a disagreement between them is
    // a cloud whose lumps are in the wrong places, which looks like a sculpting mistake.
    //
    // Read at TEXEL CENTRES, where a trilinear filter returns exactly the texel it sits on, so the
    // comparison is exact rather than approximate — any error is an addressing error and not a filtering
    // one.
    const auto& body = Body();

    size_t compared = 0;
    for ( uint32_t z = 1; z < Assets::kCloudModellingVolumeDepth; z += 7 )
    {
        for ( uint32_t y = 1; y < Assets::kCloudModellingVolumeHeight; y += 5 )
        {
            for ( uint32_t x = 1; x < Assets::kCloudModellingVolumeWidth; x += 7 )
            {
                const glm::vec3 uvw( ( static_cast<float>( x ) + 0.5f ) /
                                          static_cast<float>( Assets::kCloudModellingVolumeWidth ),
                                     ( static_cast<float>( y ) + 0.5f ) /
                                          static_cast<float>( Assets::kCloudModellingVolumeHeight ),
                                     ( static_cast<float>( z ) + 0.5f ) /
                                          static_cast<float>( Assets::kCloudModellingVolumeDepth ) );

                const vec4   read  = CLOUD_SAMPLE_AUTHORED( CloudAuthoredAtlasUvw( uvw, 0.0f, 1 ) );
                const size_t index = VoxelIndex( x, y, z );

                ASSERT_NEAR( read.x, static_cast<float>( body.Voxels[index + 0] ) / 255.0f, 1e-5f )
                     << "profile at " << x << ", " << y << ", " << z;
                ASSERT_NEAR( read.y, static_cast<float>( body.Voxels[index + 1] ) / 255.0f, 1e-5f );
                ASSERT_NEAR( read.z, static_cast<float>( body.Voxels[index + 2] ) / 255.0f, 1e-5f );
                ASSERT_NEAR( read.w, static_cast<float>( body.Voxels[index + 3] ) / 255.0f, 1e-5f );
                ++compared;
            }
        }
    }

    EXPECT_GT( compared, 4000u );
}

TEST( CloudAuthored, TheGeneratorAndTheShaderAgreeAboutWhereNotOnlyAboutWhat )
{
    // THE HOLE THE TEST ABOVE HAD, found by breaking it: it compares the shader's addressing against the
    // test's own copy of the voxel layout, so TRANSPOSING THE GENERATOR'S WRITE ORDER left it green —
    // both sides permuted together and the comparison could not see it. A relation whose two sides are
    // the same statement is not a relation.
    //
    // The way to close it is through something neither side can permute: the SHAPE the recipe describes.
    // Every lump's centre is deep inside the body by construction, so reading the volume at each lump's
    // own local coordinate must find body there — and a transposed write order scatters those readings
    // across the volume, where the odds of landing inside a 10-per-cent-occupied body eight times running
    // are one in ten million.
    const Assets::CloudModellingVolumeRecipe& recipe = Body().Recipe;

    for ( size_t k = 0; k < recipe.Blobs.size(); ++k )
    {
        const glm::vec3 local = recipe.Blobs[k].CentreKm / recipe.SizeKm + glm::vec3( 0.5f );
        const vec4      voxel = CLOUD_SAMPLE_AUTHORED( CloudAuthoredAtlasUvw( local, 0.0f, 1 ) );

        EXPECT_GT( voxel.x, 0.0f ) << "lump " << k << " has no body at its own centre";
        EXPECT_GT( voxel.w, 0.0f ) << "lump " << k << " is not inside its own envelope";
    }

    // ... and the body is NOT symmetric about its middle in the vertical, which is what pins the axis
    // ORDER rather than merely the addressing within an axis: a cumulus has a flat base and a crown, so
    // the lower half of the box carries more of it than the upper half.
    size_t lower = 0;
    size_t upper = 0;
    for ( uint32_t z = 0; z < Assets::kCloudModellingVolumeDepth; ++z )
    {
        for ( uint32_t y = 0; y < Assets::kCloudModellingVolumeHeight; ++y )
        {
            for ( uint32_t x = 0; x < Assets::kCloudModellingVolumeWidth; ++x )
            {
                if ( Body().Voxels[VoxelIndex( x, y, z )] == 0u )
                    continue;
                ( y < Assets::kCloudModellingVolumeHeight / 2 ? lower : upper ) += 1;
            }
        }
    }

    EXPECT_GT( lower, upper * 3u / 2u )
         << "the body is nearly symmetric top to bottom, so this assertion cannot pin the axis order; "
            "lower "
         << lower << " upper " << upper;
}

TEST( CloudAuthored, TheBoundaryOfTheVolumeIsEmptyAndReadsAsItself )
{
    // THE ONE GUARANTEE THAT ACTUALLY CARRIES THE ADDRESSING, and it took breaking two other things to
    // find out that it is the one. `GenerateCloudModellingVolume` walks all six faces after the bake and
    // REFUSES a body that reaches any of them, and `ValidateCloudModellingRecipe` refuses the recipe
    // before it. So the outermost shell of every `.dcmv` is four zeroes.
    //
    // WHY THAT MATTERS MORE THAN IT LOOKS. Every sampler in this engine is REPEAT, and the seam pulls a
    // coordinate in by half a texel before it fetches. Both of those are second lines of defence: an
    // empty shell means a coordinate that lands on the boundary — by the clamp, by a wrap, or by a bounds
    // test that was skipped — reads zero and contributes nothing. Take the shell away and all three of
    // them become load-bearing at once.
    //
    // MEASURED, NOT ASSUMED: breaking the half-texel clamp to use the wrong axis extent leaves every test
    // in this suite green, and so does deleting the exact bounds test in the seam. Neither is redundant —
    // one bounds a cost and the other bounds a fetch — but neither is what keeps the picture right, and
    // saying that here is worth more than a test that pretends otherwise.
    const auto& body = Body();

    size_t nonEmpty = 0;
    for ( uint32_t z = 0; z < Assets::kCloudModellingVolumeDepth; ++z )
    {
        for ( uint32_t y = 0; y < Assets::kCloudModellingVolumeHeight; ++y )
        {
            for ( uint32_t x = 0; x < Assets::kCloudModellingVolumeWidth; ++x )
            {
                const bool onFace = ( x == 0 || x == Assets::kCloudModellingVolumeWidth - 1 || y == 0 ||
                                      y == Assets::kCloudModellingVolumeHeight - 1 || z == 0 ||
                                      z == Assets::kCloudModellingVolumeDepth - 1 );
                if ( !onFace )
                    continue;

                const size_t index = VoxelIndex( x, y, z );
                if ( body.Voxels[index + 0] != 0u || body.Voxels[index + 3] != 0u )
                    ++nonEmpty;
            }
        }
    }

    EXPECT_EQ( nonEmpty, 0u ) << "the body reaches its own box, so a coordinate on the boundary no longer "
                                 "reads as nothing and the REPEAT sampler becomes visible";

    // And the corner texels read back as themselves through the seam's own clamp, which is what says the
    // clamp does not move a legal coordinate off the texel it belongs to.
    for ( uint32_t z : { 0u, Assets::kCloudModellingVolumeDepth - 1u } )
    {
        for ( uint32_t y : { 0u, Assets::kCloudModellingVolumeHeight - 1u } )
        {
            for ( uint32_t x : { 0u, Assets::kCloudModellingVolumeWidth - 1u } )
            {
                const glm::vec3 uvw( ( static_cast<float>( x ) + 0.5f ) /
                                          static_cast<float>( Assets::kCloudModellingVolumeWidth ),
                                     ( static_cast<float>( y ) + 0.5f ) /
                                          static_cast<float>( Assets::kCloudModellingVolumeHeight ),
                                     ( static_cast<float>( z ) + 0.5f ) /
                                          static_cast<float>( Assets::kCloudModellingVolumeDepth ) );

                const vec4   read  = CLOUD_SAMPLE_AUTHORED( CloudAuthoredAtlasUvw( uvw, 0.0f, 1 ) );
                const size_t index = VoxelIndex( x, y, z );

                EXPECT_NEAR( read.x, static_cast<float>( body.Voxels[index + 0] ) / 255.0f, 1e-5f )
                     << "corner " << x << ", " << y << ", " << z;
                EXPECT_NEAR( read.w, static_cast<float>( body.Voxels[index + 3] ) / 255.0f, 1e-5f );
            }
        }
    }
}

TEST( CloudAuthored, TheInstanceMapsItsOwnCentreAndCorners )
{
    const Graphic::CloudAuthoredInstanceGpu gpu = MakeInstance( glm::vec3( 3.0f, 1.4f, -2.0f ) );

    ClearInstances();
    AddInstance( gpu );

    const CloudAuthoredInstance instance = CloudAuthoredInstanceAt( 0 );

    const vec3 centre = ( vec3( gpu.BoundsMin ) + vec3( gpu.BoundsMax ) ) * 0.5f;
    const vec3 middle = CloudAuthoredLocalUvw( instance, centre );

    EXPECT_NEAR( middle.x, 0.5f, 1e-4f );
    EXPECT_NEAR( middle.y, 0.5f, 1e-4f );
    EXPECT_NEAR( middle.z, 0.5f, 1e-4f );

    // An unrotated instance's bounds ARE its box, so its corners land exactly on 0 and 1 — which is the
    // statement that the packer's inverse and its bounds describe the same box.
    const vec3 low = CloudAuthoredLocalUvw( instance, vec3( gpu.BoundsMin ) );
    EXPECT_NEAR( low.x, 0.0f, 1e-4f );
    EXPECT_NEAR( low.y, 0.0f, 1e-4f );
    EXPECT_NEAR( low.z, 0.0f, 1e-4f );

    const vec3 high = CloudAuthoredLocalUvw( instance, vec3( gpu.BoundsMax ) );
    EXPECT_NEAR( high.x, 1.0f, 1e-4f );
    EXPECT_NEAR( high.y, 1.0f, 1e-4f );
    EXPECT_NEAR( high.z, 1.0f, 1e-4f );
}

TEST( CloudAuthored, BoundsContainTheRotatedBoxAndTheExactTestRejectsTheDifference )
{
    // A box rotated 45 degrees about Y has an axis-aligned hull whose horizontal extent is sqrt(2) times
    // its own. The cheap test lets those corners through and the exact one must not: every sampler in
    // this engine is REPEAT, so a coordinate outside [0, 1] would fetch the far side of the body.
    const float                             yaw = 0.7853981634f; // 45 degrees
    const Graphic::CloudAuthoredInstanceGpu gpu = MakeInstance( glm::vec3( 0.0f, 1.4f, 0.0f ), 1.0f, yaw );

    ClearInstances();
    AddInstance( gpu );
    const CloudAuthoredInstance instance = CloudAuthoredInstanceAt( 0 );

    const float halfX = 0.5f * ( gpu.BoundsMax.x - gpu.BoundsMin.x );
    EXPECT_NEAR( halfX, 0.5f * ( Body().Recipe.SizeKm.x + Body().Recipe.SizeKm.z ) * 0.70710678f, 1e-3f );

    // A corner of the axis-aligned hull: inside the cheap test, outside the body.
    const vec3 corner( gpu.BoundsMax.x - 1e-3f, 1.4f, gpu.BoundsMax.z - 1e-3f );
    EXPECT_TRUE( CloudAuthoredInBounds( instance, corner ) );
    EXPECT_FALSE( CloudAuthoredInVolume( CloudAuthoredLocalUvw( instance, corner ) ) );

    // ... and the centre is inside both.
    const vec3 centre( 0.0f, 1.4f, 0.0f );
    EXPECT_TRUE( CloudAuthoredInBounds( instance, centre ) );
    EXPECT_TRUE( CloudAuthoredInVolume( CloudAuthoredLocalUvw( instance, centre ) ) );

    // THE BOX MUST NEVER REJECT WHAT THE BODY WOULD ACCEPT, which is the one thing a conservative gate
    // owes and the one way it can be wrong. Swept over the instance's own box rather than spot-checked,
    // because the failure of a bound is at its corners.
    for ( int ix = 0; ix <= 8; ++ix )
    {
        for ( int iy = 0; iy <= 8; ++iy )
        {
            for ( int iz = 0; iz <= 8; ++iz )
            {
                const vec3 point( mix( gpu.BoundsMin.x, gpu.BoundsMax.x, static_cast<float>( ix ) / 8.0f ),
                                  mix( gpu.BoundsMin.y, gpu.BoundsMax.y, static_cast<float>( iy ) / 8.0f ),
                                  mix( gpu.BoundsMin.z, gpu.BoundsMax.z, static_cast<float>( iz ) / 8.0f ) );

                if ( CloudAuthoredInVolume( CloudAuthoredLocalUvw( instance, point ) ) )
                    ASSERT_TRUE( CloudAuthoredInBounds( instance, point ) )
                         << "the box rejected a point inside the body at " << ix << ", " << iy << ", " << iz;
            }
        }
    }

    // AND A POINT WELL OUTSIDE IS REJECTED BY THE CHEAP TEST, which is the whole of what it buys. Note
    // what this does NOT claim: removing the box entirely changes no answer, because the exact test that
    // follows rejects the same points. It is a COST gate, and its effect is in the slope in
    // Docs/Clouds/CALIBRATION.md section A0, not in a pixel.
    EXPECT_FALSE( CloudAuthoredInBounds( instance, vec3( gpu.BoundsMax.x + 1.0f, 1.4f, 0.0f ) ) );
    EXPECT_FALSE( CloudAuthoredInBounds( instance, vec3( 0.0f, gpu.BoundsMax.y + 1.0f, 0.0f ) ) );
    EXPECT_FALSE( CloudAuthoredInBounds( instance, vec3( 0.0f, 1.4f, gpu.BoundsMin.z - 1.0f ) ) );
}

TEST( CloudAuthored, APointInsideTheHullButOutsideTheRotatedBodyIsExactlyProcedural )
{
    // A corner of a rotated instance's axis-aligned hull is INSIDE the cheap box and OUTSIDE the body,
    // which is the one place in the sky where the second gate has anything to decide. The OUTCOME is what
    // is asserted: such a point contributes nothing.
    //
    // AND IT HOLDS FOR TWO INDEPENDENT REASONS, which was found by deleting the gate and watching this
    // stay green. The exact test rejects the point; and if it did not, the seam's half-texel clamp would
    // pull the coordinate onto the volume's boundary, which the bake guarantees is empty
    // (TheBoundaryOfTheVolumeIsEmptyAndReadsAsItself). The gate is therefore a COST bound — it saves a 3D
    // fetch for every point in the difference between a rotated box and its hull, which is up to 3.4x the
    // body's own volume — and not the thing that keeps the picture right. Both are worth having and only
    // one of them is worth calling a correctness test.
    CloudFieldParams params = ParamsAtCoverage( 0.85f );

    const float                             yaw = 0.7853981634f;
    const Graphic::CloudAuthoredInstanceGpu gpu = MakeInstance( glm::vec3( 0.0f, 1.4f, 0.0f ), 1.0f, yaw );

    const glm::vec3 corner( gpu.BoundsMax.x - 0.02f, 1.4f, gpu.BoundsMax.z - 0.02f );
    const float     fraction = corner.y / kLayerThicknessKm;

    ClearInstances();
    const CloudFieldSample bare = SampleCloudField( params, fraction, corner );

    ClearInstances();
    AddInstance( gpu );
    const CloudAuthoredInstance instance = CloudAuthoredInstanceAt( 0 );
    ASSERT_TRUE( CloudAuthoredInBounds( instance, corner ) ) << "the chosen corner is not inside the hull";
    ASSERT_FALSE( CloudAuthoredInVolume( CloudAuthoredLocalUvw( instance, corner ) ) )
         << "the chosen corner is inside the body, so it tests nothing";

    const CloudFieldSample with = SampleCloudField( params, fraction, corner );

    EXPECT_EQ( with.Profile, bare.Profile );
    EXPECT_EQ( with.DensityScale, bare.DensityScale );
}

TEST( CloudAuthored, ADegenerateTransformIsRefusedRatherThanInverted )
{
    Desert::ECS::HeroCloudData data;

    glm::mat4 flattened( 1.0f );
    flattened[1] = glm::vec4( 0.0f ); // scale 0 on Y

    const auto packed = Graphic::PackCloudAuthoredInstance( flattened, Body().Recipe.SizeKm, kLayerBottomKm, data,
                                                            Graphic::CloudAuthoredAtlasSlabBaseW( 0u, 1u ) );
    EXPECT_FALSE( packed.Valid );
}

// ---------------------------------------------------------------------------------------------------
// The seam
// ---------------------------------------------------------------------------------------------------

TEST( CloudAuthored, OutsideEveryInstanceTheAnswerIsBitForBitTheProceduralOne )
{
    // THE ZERO-COST CLAIM, in its testable half. A sky with a hero cloud somewhere else in it must render
    // exactly what it rendered without one — not nearly, exactly — because the union of a zero is the
    // identity and the cutout of a zero is the identity. Anything less would show as a whole sky moving
    // when one cloud was added to a corner of it.
    CloudFieldParams params = DefaultParams();

    ClearInstances();
    glm::vec3 point;
    ASSERT_TRUE( FindProceduralPoint( params, 0.45f, point ) );

    ClearInstances();
    const CloudFieldSample without = SampleCloudField( params, 0.45f, point );

    ClearInstances();
    AddInstance( MakeInstance( point + glm::vec3( 40.0f, 0.0f, 40.0f ) ) );
    const CloudFieldSample with = SampleCloudField( params, 0.45f, point );

    EXPECT_EQ( with.Profile, without.Profile );
    EXPECT_EQ( with.DetailType, without.DetailType );
    EXPECT_EQ( with.DensityScale, without.DensityScale );
    EXPECT_EQ( with.DetailFactor, without.DetailFactor );
    EXPECT_EQ( with.ExtinctionFactor, without.ExtinctionFactor );
}

TEST( CloudAuthored, TheUnionIsAMaxAndDoesNotDependOnTheOrDER )
{
    // Two bodies overlapping at one point, listed both ways round. `max` is commutative, and the winner's
    // material numbers travel with it — so the answer must be identical, field for field, whichever order
    // the scene happened to list its entities in.
    CloudFieldParams params = DefaultParams();

    const glm::vec3 centre( 0.0f, 0.5f, 0.0f );

    // The second is scaled down and given different material numbers, so "which one won" is visible in
    // the sample rather than having to be inferred.
    Graphic::CloudAuthoredInstanceGpu big   = MakeInstance( centre );
    Graphic::CloudAuthoredInstanceGpu small = MakeInstance( centre + glm::vec3( 0.35f, 0.0f, 0.0f ), 0.5f );
    small.Row0.w                            = 2.0f; // DetailFactor
    small.Row1.w                            = 3.0f; // DensityFactor
    small.Row2.w                            = 4.0f; // ExtinctionFactor

    size_t compared = 0;
    for ( int ix = -12; ix <= 12; ++ix )
    {
        for ( int iy = -6; iy <= 6; ++iy )
        {
            const glm::vec3 point( centre.x + static_cast<float>( ix ) * 0.06f,
                                   centre.y + static_cast<float>( iy ) * 0.05f, centre.z );
            const float     fraction = point.y / kLayerThicknessKm;

            ClearInstances();
            AddInstance( big );
            AddInstance( small );
            const CloudFieldSample forwards = SampleCloudField( params, fraction, point );

            ClearInstances();
            AddInstance( small );
            AddInstance( big );
            const CloudFieldSample backwards = SampleCloudField( params, fraction, point );

            ASSERT_EQ( forwards.Profile, backwards.Profile ) << "at " << ix << ", " << iy;
            ASSERT_EQ( forwards.DetailType, backwards.DetailType );
            ASSERT_EQ( forwards.DensityScale, backwards.DensityScale );
            ASSERT_EQ( forwards.DetailFactor, backwards.DetailFactor );
            ASSERT_EQ( forwards.ExtinctionFactor, backwards.ExtinctionFactor );

            // And it really is the MAX of the two, not one of them: each alone, then together.
            ClearInstances();
            AddInstance( big );
            const float onlyBig = SampleCloudField( params, fraction, point ).Profile;

            ClearInstances();
            AddInstance( small );
            const float onlySmall = SampleCloudField( params, fraction, point ).Profile;

            ASSERT_FLOAT_EQ( forwards.Profile, std::max( onlyBig, onlySmall ) );

            if ( forwards.Profile > 0.0f )
                ++compared;
        }
    }

    EXPECT_GT( compared, 20u ) << "the two bodies never overlapped the sampled points, so nothing was tested";
}

TEST( CloudAuthored, TheAuthoredWinnerTakesAllItsOwnMaterialNumbers )
{
    // A blend of two bodies' detail character is a third character that is neither, and the seam between
    // them is exactly where a floating average shows as a smear of the wrong kind of edge. So the winner
    // takes all four, and this is the assertion of it.
    CloudFieldParams params = DefaultParams();

    const glm::vec3 centre( 0.0f, 0.5f, 0.0f );

    Graphic::CloudAuthoredInstanceGpu solitary = MakeInstance( centre );
    solitary.Row0.w                            = 2.5f;
    solitary.Row1.w                            = 0.5f;
    solitary.Row2.w                            = 4.0f;

    ClearInstances();
    AddInstance( solitary );

    const CloudFieldSample sample = SampleCloudField( params, centre.y / kLayerThicknessKm, centre );
    ASSERT_GT( sample.Profile, 0.0f );

    EXPECT_FLOAT_EQ( sample.DetailFactor, 2.5f );
    EXPECT_FLOAT_EQ( sample.ExtinctionFactor, 4.0f );

    // The density is the LAYER's, the instance's factor and the volume's own per-voxel scale, in that
    // order — the same three-level composition the procedural producer performs.
    const CloudAuthoredInstance instance = CloudAuthoredInstanceAt( 0 );
    const vec4                  voxel    = CLOUD_SAMPLE_AUTHORED( CloudAuthoredAtlasUvw(
         CloudAuthoredLocalUvw( instance, centre ), instance.BoundsMax.w, CLOUD_AUTHORED_SLAB_COUNT ) );
    EXPECT_FLOAT_EQ( sample.DensityScale, params.DensityScale * 0.5f * voxel.z );
}

TEST( CloudAuthored, TheWinnersMaterialNumbersAreBoundedBecauseNothingUpstreamOfThemIs )
{
    // THE AUTHORED SIDE IS THE ONE WITH NO VALIDATOR IN FRONT OF IT, which is why this test is here and
    // not only in Desert/Tests/Engine/CloudField. A procedural species' three factors are held to [0, 8]
    // by Assets::ValidateCloudTypeShape before the file will load at all. A hero cloud's come from
    // ECS::HeroCloudData, whose reflected Range(0, 4) constrains the editor's SLIDER and nothing else:
    // Core/Serialize/ComponentRegistry.cpp clamps nothing on the way in, and
    // Graphic::PackCloudAuthoredInstance's `std::max(x, 0.0f)` is a lower bound that leaves +inf exactly
    // as it found it.
    //
    // What that costs if the shader trusts it: the layer's Detail Strength and Density Scale are sliders
    // whose ZERO is a documented position, `0 * (+inf)` is NaN, and a NaN density is a NaN optical depth,
    // therefore a NaN transmittance, therefore a black or fully transparent hole in the sky with nothing
    // in the log. It went red once as a unit test before it was ever seen in a frame.
    const glm::vec3 centre( 0.0f, 0.5f, 0.0f );

    const float hostile[] = { std::numeric_limits<float>::infinity(), 1e30f, std::numeric_limits<float>::max() };

    for ( const float factor : hostile )
    {
        for ( const float slider : { 0.0f, 1.0f, 2.0f } )
        {
            CloudFieldParams params = DefaultParams();
            params.DensityScale     = slider;
            params.DetailStrength   = slider == 0.0f ? 0.0f : 0.4f;

            Graphic::CloudAuthoredInstanceGpu solitary = MakeInstance( centre );
            solitary.Row0.w                            = factor; // DetailFactor
            solitary.Row1.w                            = factor; // DensityFactor
            solitary.Row2.w                            = factor; // ExtinctionFactor

            ClearInstances();
            AddInstance( solitary );

            const CloudFieldSample sample = SampleCloudField( params, centre.y / kLayerThicknessKm, centre );
            ASSERT_GT( sample.Profile, 0.0f ) << "the fixture put no body in front of the assertion";

            EXPECT_TRUE( std::isfinite( sample.DetailFactor ) ) << "DetailFactor " << factor;
            EXPECT_TRUE( std::isfinite( sample.DensityScale ) )
                 << "Density Scale " << slider << " x DensityFactor " << factor;
            EXPECT_TRUE( std::isfinite( sample.ExtinctionFactor ) ) << "ExtinctionFactor " << factor;

            const float density = CloudSampleDensity( params, sample, centre );
            EXPECT_TRUE( std::isfinite( density ) ) << "factor " << factor << " slider " << slider;
            EXPECT_GE( density, 0.0f );
            EXPECT_LE( density, 1.0f );
        }
    }
}

TEST( CloudAuthored, StrengthScalesTheProfile )
{
    CloudFieldParams params = DefaultParams();

    const glm::vec3 centre( 0.0f, 0.5f, 0.0f );
    const float     fraction = centre.y / kLayerThicknessKm;

    ClearInstances();
    AddInstance( MakeInstance( centre, 1.0f, 0.0f, 1.0f ) );
    const float full = SampleCloudField( params, fraction, centre ).Profile;

    ClearInstances();
    AddInstance( MakeInstance( centre, 1.0f, 0.0f, 0.4f ) );
    const float faded = SampleCloudField( params, fraction, centre ).Profile;

    ASSERT_GT( full, 0.0f );
    EXPECT_NEAR( faded, full * 0.4f, 1e-5f );

    // At zero the body is gone and the sky is the procedural one again — which is what makes Strength a
    // fade rather than a switch, and what a cutscene needs to bring a hero cloud in.
    ClearInstances();
    AddInstance( MakeInstance( centre, 1.0f, 0.0f, 0.0f, /*cutout=*/true ) );
    const CloudFieldSample gone = SampleCloudField( params, fraction, centre );

    ClearInstances();
    const CloudFieldSample bare = SampleCloudField( params, fraction, centre );

    EXPECT_EQ( gone.Profile, bare.Profile );
}

// ---------------------------------------------------------------------------------------------------
// The cutout
// ---------------------------------------------------------------------------------------------------

TEST( CloudAuthored, TheCutoutRemovesTheProceduralFieldInsideTheEnvelopeAndLeavesItOutside )
{
    // WITHOUT THIS, a procedural blob grows through a sculpted cloud and the composition turns to soup
    // (ANALYSIS_APPROACH.md section 4.3). The assertion has three parts and all three are needed: inside
    // the envelope the procedural field must be gone, the authored body must survive, and OUTSIDE the
    // instance nothing may move — a cutout that took the whole box would cut a rectangular hole in the
    // deck around the cloud.
    // An overcast sky, so there is procedural field everywhere to remove. It is a BAKE now: coverage
    // decides what is in the modelling volume rather than what threshold the march applies.
    CloudFieldParams params = ParamsAtCoverage( 0.85f );

    constexpr float fraction = 0.45f;
    glm::vec3       centre;
    ASSERT_TRUE( FindProceduralPoint( params, fraction, centre ) );

    const Graphic::CloudAuthoredInstanceGpu open    = MakeInstance( centre, 1.0f, 0.0f, 1.0f, /*cutout=*/false );
    const Graphic::CloudAuthoredInstanceGpu cutting = MakeInstance( centre, 1.0f, 0.0f, 1.0f, /*cutout=*/true );

    glm::vec3 point;
    float     authored = 0.0f;
    ASSERT_TRUE( FindCutoutPoint( params, open, fraction, point, authored ) )
         << "no point where the procedural field wins inside the body's envelope, so this test would "
            "measure nothing";

    ClearInstances();
    const float bare = SampleCloudField( params, fraction, point ).Profile;

    // With the cutout OFF the union keeps the deeper of the two, and here that is the procedural field.
    ClearInstances();
    AddInstance( open );
    EXPECT_FLOAT_EQ( SampleCloudField( params, fraction, point ).Profile, bare );

    // With it ON the procedural field is gone and what remains is the authored body alone.
    ClearInstances();
    AddInstance( cutting );
    const CloudFieldSample cut = SampleCloudField( params, fraction, point );

    EXPECT_FLOAT_EQ( cut.Profile, authored );
    EXPECT_LT( cut.Profile, bare ) << "the cutout removed nothing";

    // ... and well outside the instance the procedural field is exactly what it was, bit for bit. This
    // is the half that fails if the cutout is applied over the box rather than over the body.
    const glm::vec3 elsewhere = centre + glm::vec3( 30.0f, 0.0f, 30.0f );

    ClearInstances();
    const CloudFieldSample bareElsewhere = SampleCloudField( params, fraction, elsewhere );

    ClearInstances();
    AddInstance( cutting );
    const CloudFieldSample cutElsewhere = SampleCloudField( params, fraction, elsewhere );

    EXPECT_EQ( cutElsewhere.Profile, bareElsewhere.Profile );
}

TEST( CloudAuthored, TheCutoutUnionsAcrossInstancesRatherThanFollowingTheWinner )
{
    // Two hero clouds standing in each other's boxes each suppress the procedural field over their own
    // body, and it has to be gone from BOTH: taking only the winner's cutout would let a blob grow
    // through the one that lost the profile by a hair.
    //
    // The arrangement is chosen so that the instance which CUTS is the one that LOSES: `deep` is the
    // same body scaled up, so at the test point it is further inside itself and wins the union, and it is
    // the one with the cutout switched OFF.
    CloudFieldParams params = ParamsAtCoverage( 0.85f );

    constexpr float fraction = 0.45f;
    glm::vec3       centre;
    ASSERT_TRUE( FindProceduralPoint( params, fraction, centre ) );

    const Graphic::CloudAuthoredInstanceGpu cutting = MakeInstance( centre, 1.0f, 0.0f, 1.0f, /*cutout=*/true );

    glm::vec3 point;
    float     authored = 0.0f;
    ASSERT_TRUE( FindCutoutPoint( params, cutting, fraction, point, authored ) );

    ClearInstances();
    const float bare = SampleCloudField( params, fraction, point ).Profile;

    // THE SECOND BODY'S STRENGTH IS SEARCHED FOR AND NOT CHOSEN, and the search is the whole precondition
    // of this test rather than tidiness.
    //
    // The arrangement needs `deep` to beat `cutting` and to LOSE to the procedural field, so that the
    // union's winner is a body with its cutout switched OFF and the thing the cutout has to remove is
    // still the thing on top. A fixed strength of 1 gave that against the old producer, whose profile
    // sat low almost everywhere; the profile is a normalised DISTANCE FIELD now — near 1 inside a body
    // and 0 at its surface — so at a point in the outer shell of one body the scaled-up copy of it is
    // deeper than the procedural field is, and the union took the authored answer. `onlyDeep` was 0.483
    // where `bare` was lower, and the test failed on its own precondition rather than on its claim.
    //
    // Searching for the strength states the precondition instead of assuming it, and fails loudly if no
    // strength satisfies it — which is what the ASSERT below is for.
    float                             deepStrength = 0.0f;
    Graphic::CloudAuthoredInstanceGpu deep{};

    for ( int step = 1; step <= 40; ++step )
    {
        const float strength = static_cast<float>( step ) / 40.0f;

        const Graphic::CloudAuthoredInstanceGpu candidate =
             MakeInstance( centre, 1.35f, 0.0f, strength, /*cutout=*/false );

        ClearInstances();
        AddInstance( candidate );

        const float profile = SampleCloudField( params, fraction, point ).Profile;

        // Beats the cutting body, loses to the procedural field. `<=` on the upper side because the union
        // reports the procedural value exactly once the authored one is under it.
        if ( profile > authored && profile <= bare )
        {
            deepStrength = strength;
            deep         = candidate;
            break;
        }
    }

    ASSERT_GT( deepStrength, 0.0f ) << "no strength of the second body both beats the cutting one (" << authored
                                    << ") and loses to the procedural field (" << bare
                                    << "), so this arrangement cannot measure what it claims";

    ClearInstances();
    AddInstance( deep );
    const float onlyDeep = SampleCloudField( params, fraction, point ).Profile;
    ASSERT_FLOAT_EQ( onlyDeep, bare ) << "the procedural field was supposed to win against the pair alone";

    ClearInstances();
    AddInstance( deep );
    AddInstance( cutting );
    const float both = SampleCloudField( params, fraction, point ).Profile;

    EXPECT_LT( both, bare ) << "the cutout of the instance that LOST the union was ignored, so a "
                               "procedural blob is still growing through a hero cloud";

    // ... and the other order gives the same answer, because the cutout unions rather than sequences.
    ClearInstances();
    AddInstance( cutting );
    AddInstance( deep );
    EXPECT_FLOAT_EQ( SampleCloudField( params, fraction, point ).Profile, both );
}

// ---------------------------------------------------------------------------------------------------
// The relation the renderer warns about
// ---------------------------------------------------------------------------------------------------

TEST( CloudAuthored, FitsLayerIsTheRelationBetweenTheBodyAndTheShell )
{
    // The march only samples between the two shells, so a body outside them is not clipped by anything an
    // artist can see — it is simply never sampled, and the symptom is a cumulus with its crown sliced
    // flat by an altitude nobody set. Stated as a predicate so the renderer can name both numbers.
    const Graphic::CloudAuthoredInstanceGpu inside = MakeInstance( glm::vec3( 0.0f, 1.8f, 0.0f ) );
    EXPECT_TRUE( Graphic::CloudAuthoredInstanceFitsLayer( inside, kLayerThicknessKm ) );

    // Half a kilometre above the base with a body a kilometre tall: its bottom is below the shell.
    const Graphic::CloudAuthoredInstanceGpu low = MakeInstance( glm::vec3( 0.0f, 0.3f, 0.0f ) );
    EXPECT_FALSE( Graphic::CloudAuthoredInstanceFitsLayer( low, kLayerThicknessKm ) );

    const Graphic::CloudAuthoredInstanceGpu high = MakeInstance( glm::vec3( 0.0f, 3.4f, 0.0f ) );
    EXPECT_FALSE( Graphic::CloudAuthoredInstanceFitsLayer( high, kLayerThicknessKm ) );
}

// ======================================================================================================
// PHASE A2 — SEVERAL BODIES IN ONE SKY: the atlas, its addressing, and what keeps two clouds apart
// ======================================================================================================

TEST( CloudAtlas, TheAtlasIsTheBodiesConcatenatedAndNothingElse )
{
    // WHY THE DEPTH AXIS WAS CHOSEN, as an assertion rather than as a comment. The volume's layout has x
    // fastest and z slowest, so stacking on z makes the atlas a CONCATENATION — which is what lets the
    // assembler be a run of memcpy and lets this test state the whole result in one comparison. Stacking
    // on y would be a 1 024-piece interleave for an identical picture, and this test is where that
    // decision stops being a claim.
    const std::vector<unsigned char>& first  = CatalogueBody( Assets::CloudModellingSpecies::Cumulonimbus );
    const std::vector<unsigned char>& second = CatalogueBody( Assets::CloudModellingSpecies::Freeform );

    const auto assembled = Assets::AssembleCloudModellingAtlas( { &first, &second } );
    ASSERT_TRUE( assembled ) << assembled.GetError();

    std::vector<unsigned char> expected;
    expected.insert( expected.end(), first.begin(), first.end() );
    expected.insert( expected.end(), second.begin(), second.end() );

    EXPECT_EQ( assembled.GetValue(), expected );
    EXPECT_EQ( assembled.GetValue().size(), 2u * Assets::kCloudModellingVoxelBytes );
}

TEST( CloudAtlas, AnEmptyListAndAWrongSizedBodyAreRefusedRatherThanPacked )
{
    // AN ATLAS OF NOTHING IS NOT AN EMPTY ATLAS. A volume of zero depth is not a thing a device will
    // create, and the caller's answer to "no hero clouds" is the FALLBACK image — the rake this
    // subsystem has already stood on, where an invalid descriptor set makes the compute dispatch vanish
    // with nothing in the log. So the assembler refuses and says so.
    const auto empty = Assets::AssembleCloudModellingAtlas( {} );
    EXPECT_FALSE( empty );

    const std::vector<unsigned char> truncated( 16u, 0u );
    const auto                       wrong = Assets::AssembleCloudModellingAtlas( { &truncated } );
    EXPECT_FALSE( wrong );
    EXPECT_NE( wrong.GetError().find( "16" ), std::string::npos ) << wrong.GetError();

    const std::vector<unsigned char>* nothing = nullptr;
    EXPECT_FALSE( Assets::AssembleCloudModellingAtlas( { nothing } ) );
}

TEST( CloudAtlas, TheShaderAndTheEngineAgreeOnWhereEverySlabBegins )
{
    // The two sides of the atlas's geometry: CloudAuthoredAtlasSlabBaseW in the dialect, which the march
    // calls, and Graphic::CloudAuthoredAtlasSlabBaseW in C++, which the renderer packs with. A divergence
    // here is a hero cloud reading a fraction of the wrong body — and NOT an error, because both numbers
    // are legal coordinates.
    for ( uint32_t count = 1u; count <= Graphic::kCloudModellingAtlasMaxSlabs; ++count )
    {
        for ( uint32_t slot = 0u; slot < count; ++slot )
        {
            EXPECT_FLOAT_EQ( CloudAuthoredAtlasSlabBaseW( static_cast<int>( slot ), static_cast<int>( count ) ),
                             Graphic::CloudAuthoredAtlasSlabBaseW( slot, count ) )
                 << "slot " << slot << " of " << count;
        }
    }
}

TEST( CloudAtlas, OneSlabIsBitForBitTheAddressingA0Shipped )
{
    // THE REGRESSION THE SIX-POINT PROTOCOL IS THE PICTURE OF. A scene with one hero cloud must render
    // exactly the frame it rendered before the atlas existed, and the reason it does is arithmetic rather
    // than luck: with one slab the base is 0 and the divisor is 1, and adding zero and dividing by one
    // are both exact in IEEE754. Asserted on the BITS, not with a tolerance, because a tolerance is what
    // a byte-identical claim cannot be built on.
    const float halfU = 0.5f / static_cast<float>( Assets::kCloudModellingVolumeWidth );
    const float halfV = 0.5f / static_cast<float>( Assets::kCloudModellingVolumeHeight );
    const float halfW = 0.5f / static_cast<float>( Assets::kCloudModellingVolumeDepth );

    for ( int i = 0; i <= 64; ++i )
    {
        const float     t = static_cast<float>( i ) / 64.0f;
        const glm::vec3 uvw( t, 1.0f - t, ( t * 7.0f ) - std::floor( t * 7.0f ) );

        const glm::vec3 mapped = CloudAuthoredAtlasUvw( uvw, 0.0f, 1 );

        // A0's own clamp, written out here so the two can be compared rather than described.
        const glm::vec3 a0( std::clamp( uvw.x, halfU, 1.0f - halfU ), std::clamp( uvw.y, halfV, 1.0f - halfV ),
                            std::clamp( uvw.z, halfW, 1.0f - halfW ) );

        EXPECT_EQ( std::bit_cast<uint32_t>( mapped.x ), std::bit_cast<uint32_t>( a0.x ) );
        EXPECT_EQ( std::bit_cast<uint32_t>( mapped.y ), std::bit_cast<uint32_t>( a0.y ) );
        EXPECT_EQ( std::bit_cast<uint32_t>( mapped.z ), std::bit_cast<uint32_t>( a0.z ) );
    }
}

TEST( CloudAtlas, NoCoordinateOfOneSlabCanReachItsNeighbour )
{
    // THE DEFECT THIS FORBIDS, and it is the one an atlas is exposed to and a single volume is not: the
    // trilinear filter takes TWO texels along the depth axis, and if the second of them belongs to the
    // next body then a cumulonimbus grows a slice of somebody else's arch along its face. Every sampler
    // in this engine is REPEAT, so there is no address mode to hide behind.
    //
    // Stated as the property rather than as a spot check: for EVERY coordinate the exact bounds test lets
    // through, BOTH depth taps are texels of this instance's own slab.
    for ( uint32_t count = 1u; count <= Graphic::kCloudModellingAtlasMaxSlabs; ++count )
    {
        const float atlasDepth = static_cast<float>( Assets::kCloudModellingVolumeDepth * count );

        for ( uint32_t slot = 0u; slot < count; ++slot )
        {
            const float base = Graphic::CloudAuthoredAtlasSlabBaseW( slot, count );

            for ( int i = 0; i <= 256; ++i )
            {
                const float     z = static_cast<float>( i ) / 256.0f;
                const glm::vec3 mapped =
                     CloudAuthoredAtlasUvw( glm::vec3( 0.5f, 0.5f, z ), base, static_cast<int>( count ) );

                // The texels a trilinear fetch blends between, exactly as the hardware finds them.
                const float coordinate = mapped.z * atlasDepth - 0.5f;
                const int   lower      = static_cast<int>( std::floor( coordinate ) );
                const float fraction   = coordinate - std::floor( coordinate );

                const int firstOfSlab = static_cast<int>( slot * Assets::kCloudModellingVolumeDepth );
                const int lastOfSlab  = firstOfSlab + static_cast<int>( Assets::kCloudModellingVolumeDepth ) - 1;

                ASSERT_GE( lower, firstOfSlab ) << "slot " << slot << " of " << count << " at z " << z;
                ASSERT_LE( lower, lastOfSlab ) << "slot " << slot << " of " << count << " at z " << z;

                // THE SECOND TAP AND THE ONE PLACE THIS IS NOT EXACT. In real arithmetic the clamp puts
                // z = 1 exactly on the slab's last texel centre, the filter's weight on the next texel is
                // exactly zero, and the neighbour is unreachable. In FLOAT, dividing by a slab count that
                // is not a power of two leaves the coordinate a few parts in a hundred thousand past that
                // centre — measured at 3e-5 on slot 1 of 3 — so the hardware would blend that much of the
                // NEXT BODY's outermost texel.
                //
                // It is harmless and the reason is the bake's, not the addressing's: Assets::
                // GenerateCloudModellingVolume refuses a volume whose body touches its own boundary, so
                // that texel is four zeroes and 3e-5 of nothing is nothing. `CloudAuthored.
                // TheBoundaryOfTheVolumeIsEmptyAndReadsAsItself` is the assertion that keeps it that way.
                //
                // So what is asserted here is the honest version: either the second tap is inside the
                // slab, or its WEIGHT is below a thousandth.
                if ( fraction >= 1e-3f )
                    ASSERT_LE( lower + 1, lastOfSlab ) << "slot " << slot << " of " << count << " at z " << z;
            }
        }
    }
}

TEST( CloudAtlas, EachInstanceReadsItsOwnBodyAndReadingTheNeighboursIsVisible )
{
    // THE CLAIM PHASE A2 IS FOR: several hero clouds in one sky, each of them ITS OWN sculpted body.
    //
    // The two bodies are the cumulonimbus and the arch, chosen because they disagree everywhere — what
    // this test needs from a second slab is that reading the wrong one CHANGES the answer, and two
    // similar clouds would hide exactly that. The second half of the test proves the first half can see
    // the defect it is written against, which is the house rule about a sabotage that changes nothing.
    const auto storm = Assets::CloudModellingSpecies::Cumulonimbus;
    const auto arch  = Assets::CloudModellingSpecies::Freeform;

    const CloudFieldParams params = DefaultParams();

    const glm::vec3 stormAt( -6.0f, 1.6f, 0.0f );
    const glm::vec3 archAt( 6.0f, 1.0f, 0.0f );

    std::vector<float> aloneStorm;
    std::vector<float> aloneArch;

    const auto sweep =
         [&]( const Graphic::CloudAuthoredInstanceGpu& instance, const glm::vec3& centre, std::vector<float>& out )
    {
        out.clear();
        for ( int i = -6; i <= 6; ++i )
        {
            for ( int k = -6; k <= 6; ++k )
            {
                const glm::vec3 p =
                     centre + glm::vec3( static_cast<float>( i ) * 0.25f, 0.0f, static_cast<float>( k ) * 0.25f );
                ClearInstanceList();
                AddInstance( instance );
                out.push_back( CloudSampleAuthoredField( params, p ).Field.Profile );
            }
        }
    };

    // What each body answers ON ITS OWN, one slab, which is A0's case and therefore the ground truth.
    ClearInstances();
    SetAtlas( { &CatalogueBody( storm ) } );
    sweep( MakeCatalogueInstance( storm, stormAt, 1.0f, 0u, 1u ), stormAt, aloneStorm );

    ClearInstances();
    SetAtlas( { &CatalogueBody( arch ) } );
    sweep( MakeCatalogueInstance( arch, archAt, 1.0f, 0u, 1u ), archAt, aloneArch );

    // Both bodies are actually there, so the comparison below is not two rows of zeroes agreeing.
    EXPECT_GT( *std::max_element( aloneStorm.begin(), aloneStorm.end() ), 0.2f );
    EXPECT_GT( *std::max_element( aloneArch.begin(), aloneArch.end() ), 0.2f );

    // ... and now BOTH of them, in one atlas, in one sky.
    const std::vector<const std::vector<unsigned char>*> both{ &CatalogueBody( storm ), &CatalogueBody( arch ) };

    const Graphic::CloudAuthoredInstanceGpu stormInstance = MakeCatalogueInstance( storm, stormAt, 1.0f, 0u, 2u );
    const Graphic::CloudAuthoredInstanceGpu archInstance  = MakeCatalogueInstance( arch, archAt, 1.0f, 1u, 2u );

    ClearInstances();
    SetAtlas( both );

    size_t index = 0;
    for ( int i = -6; i <= 6; ++i )
    {
        for ( int k = -6; k <= 6; ++k )
        {
            const glm::vec3 offset( static_cast<float>( i ) * 0.25f, 0.0f, static_cast<float>( k ) * 0.25f );

            ClearInstanceList();
            AddInstance( stormInstance );
            AddInstance( archInstance );

            ASSERT_NEAR( CloudSampleAuthoredField( params, stormAt + offset ).Field.Profile, aloneStorm[index],
                         1e-6f )
                 << "the storm at " << i << ", " << k;
            ASSERT_NEAR( CloudSampleAuthoredField( params, archAt + offset ).Field.Profile, aloneArch[index],
                         1e-6f )
                 << "the arch at " << i << ", " << k;
            ++index;
        }
    }

    // THE SABOTAGE, PERFORMED HERE RATHER THAN LEFT TO A REVIEWER: give the storm the arch's slab. If the
    // assertions above could not see that, they would not be measuring the slab at all.
    const Graphic::CloudAuthoredInstanceGpu swapped = MakeCatalogueInstance( storm, stormAt, 1.0f, 1u, 2u );

    size_t moved = 0;
    index        = 0;
    SetAtlas( both );
    for ( int i = -6; i <= 6; ++i )
    {
        for ( int k = -6; k <= 6; ++k )
        {
            const glm::vec3 offset( static_cast<float>( i ) * 0.25f, 0.0f, static_cast<float>( k ) * 0.25f );

            ClearInstanceList();
            AddInstance( swapped );

            if ( std::abs( CloudSampleAuthoredField( params, stormAt + offset ).Field.Profile -
                           aloneStorm[index] ) > 1e-4f )
                ++moved;
            ++index;
        }
    }

    EXPECT_GT( moved, 20u ) << "reading the neighbouring slab changed almost nothing, so this test cannot "
                               "see the defect it is written against";
}

TEST( CloudAtlas, TheUnionOverDifferentBodiesDoesNotDependOnTheOrder )
{
    // A0 asserted this over instances of ONE body, where the two orders read the same texels. Over an
    // atlas the instances read DIFFERENT texels, so the property has to be re-established rather than
    // inherited: max is still commutative, but only if each instance keeps its own slab when the list is
    // permuted.
    const auto storm = Assets::CloudModellingSpecies::Cumulonimbus;
    const auto arch  = Assets::CloudModellingSpecies::Freeform;
    const auto lens  = Assets::CloudModellingSpecies::Lenticular;

    const std::vector<const std::vector<unsigned char>*> three{ &CatalogueBody( storm ), &CatalogueBody( arch ),
                                                                &CatalogueBody( lens ) };

    // Overlapping on purpose: three bodies that never meet would make this test true for the wrong reason.
    const Graphic::CloudAuthoredInstanceGpu instances[3] = {
         MakeCatalogueInstance( storm, glm::vec3( 0.0f, 1.6f, 0.0f ), 1.0f, 0u, 3u ),
         MakeCatalogueInstance( arch, glm::vec3( 0.6f, 1.2f, 0.3f ), 1.0f, 1u, 3u ),
         MakeCatalogueInstance( lens, glm::vec3( -0.4f, 1.4f, -0.2f ), 1.0f, 2u, 3u ),
    };

    const CloudFieldParams params = DefaultParams();

    int order[3] = { 0, 1, 2 };

    std::vector<float> reference;
    size_t             nonZero = 0;

    ClearInstances();
    SetAtlas( three );

    do
    {
        std::vector<float> readings;
        for ( int i = -8; i <= 8; ++i )
        {
            for ( int k = -8; k <= 8; ++k )
            {
                const glm::vec3 p( static_cast<float>( i ) * 0.2f, 1.4f, static_cast<float>( k ) * 0.2f );

                ClearInstanceList();
                for ( int slot : order )
                    AddInstance( instances[slot] );

                const CloudAuthoredResult result = CloudSampleAuthoredField( params, p );
                readings.push_back( result.Field.Profile );
                readings.push_back( result.Field.DetailType );
                readings.push_back( result.Cutout );
            }
        }

        if ( reference.empty() )
        {
            reference = readings;
            nonZero   = static_cast<size_t>(
                 std::count_if( readings.begin(), readings.end(), []( float v ) { return v > 0.0f; } ) );
        }
        else
        {
            ASSERT_EQ( readings.size(), reference.size() );
            for ( size_t i = 0; i < readings.size(); ++i )
                ASSERT_FLOAT_EQ( readings[i], reference[i] ) << "reading " << i;
        }
    } while ( std::next_permutation( order, order + 3 ) );

    EXPECT_GT( nonZero, 40u ) << "the sweep found almost nothing, so permuting it proves almost nothing";
}

TEST( CloudAtlas, TheSignedStrengthCarriesBothTheStrengthAndTheCutout )
{
    // The float A2 freed to hold the slab. Strength and cutout were two numbers where the second was
    // suppress times the first; they are one signed number now, and this is the assertion that the two
    // readers recover exactly what the packer meant.
    for ( const bool suppress : { false, true } )
    {
        for ( const float strength : { 0.0f, 0.25f, 1.0f } )
        {
            const Graphic::CloudAuthoredInstanceGpu gpu =
                 MakeInstance( glm::vec3( 0.0f, 1.5f, 0.0f ), 1.0f, 0.0f, strength, suppress );

            CloudAuthoredInstance instance;
            std::memcpy( &instance, &gpu, sizeof( instance ) );

            EXPECT_FLOAT_EQ( CloudAuthoredStrength( instance ), strength ) << "suppress " << suppress;
            EXPECT_FLOAT_EQ( CloudAuthoredCutout( instance ), suppress ? strength : 0.0f );
        }
    }
}

TEST( CloudAtlas, OnlyAnEmptyPayloadIsBindableAgainstTheFallback )
{
    // THE RELATION BETWEEN THE PAYLOAD AND THE IMAGE. An instance packed for a three-slab atlas and
    // dispatched against a one-slab one reads a third of the wrong body — legally, silently, and looking
    // like a sculpting mistake. Worst of all is the FALLBACK, one texel across, which is what gets bound
    // when no atlas was built: against it the only coherent payload is an empty one.
    Graphic::CloudAuthoredPayload payload;

    EXPECT_TRUE( Graphic::CloudAuthoredPayloadIsBindable( payload, 0u ) );
    EXPECT_FALSE( Graphic::CloudAuthoredPayloadIsBindable( payload, 1u ) );

    // THE CASE THIS TEST DID NOT HAVE, and a sabotage found it: an instance list with the FALLBACK bound.
    // Deleting the clause that forbids it left every assertion above green, because every one of them had
    // a count of zero — the two conditions were never varied independently. It is the exact arrangement
    // the subsystem's oldest rake produces: no atlas was built, the one-texel fallback goes in to keep the
    // descriptor set valid, and an instance that still believes in a body reads it.
    payload.Count        = 1;
    payload.Instances[0] = MakeInstance( glm::vec3( 0.0f, 1.5f, 0.0f ), 1.0f, 0.0f, 1.0f, false, 0u, 1u );
    EXPECT_FALSE( Graphic::CloudAuthoredPayloadIsBindable( payload, 0u ) );
    payload = Graphic::CloudAuthoredPayload{};

    payload.SlabCount    = 2;
    payload.Count        = 2;
    payload.Instances[0] = MakeInstance( glm::vec3( 0.0f, 1.5f, 0.0f ), 1.0f, 0.0f, 1.0f, false, 0u, 2u );
    payload.Instances[1] = MakeInstance( glm::vec3( 4.0f, 1.5f, 0.0f ), 1.0f, 0.0f, 1.0f, false, 1u, 2u );

    EXPECT_TRUE( Graphic::CloudAuthoredPayloadIsBindable( payload, 2u ) );
    EXPECT_FALSE( Graphic::CloudAuthoredPayloadIsBindable( payload, 1u ) );
    EXPECT_FALSE( Graphic::CloudAuthoredPayloadIsBindable( payload, 3u ) );

    // An instance packed for a THREE-slab atlas among two-slab ones: the count agrees, the coordinate
    // does not, and it is the coordinate that decides which cloud is drawn.
    payload.Instances[1] = MakeInstance( glm::vec3( 4.0f, 1.5f, 0.0f ), 1.0f, 0.0f, 1.0f, false, 1u, 3u );
    EXPECT_FALSE( Graphic::CloudAuthoredPayloadIsBindable( payload, 2u ) );

    // ... and a count past the slots, which is the other way a payload can be incoherent.
    payload.Instances[1] = MakeInstance( glm::vec3( 4.0f, 1.5f, 0.0f ), 1.0f, 0.0f, 1.0f, false, 1u, 2u );
    payload.Count        = static_cast<int32_t>( Graphic::kCloudAuthoredSlots ) + 1;
    EXPECT_FALSE( Graphic::CloudAuthoredPayloadIsBindable( payload, 2u ) );
}

TEST( CloudAtlas, EightBodiesFitTheBudgetAndNineDoNot )
{
    // THE NUMBER BEHIND CLOUD_AUTHORED_SLOTS, as arithmetic rather than as a comment. Decision D-9 gives
    // the subsystem 64 MiB; A0 measured 20.67 MiB occupied at 1280x766, and the trace and history targets
    // grow with the frame — 8.42 MiB of them at that size becomes 17.8 MiB at 1920x1080, so 9.4 MiB more.
    constexpr double kBudgetMiB           = 64.0;
    constexpr double kOccupiedMiB         = 20.67;
    constexpr double kTargetGrowth1080MiB = 9.4;
    constexpr double kBodyMiB             = 4.0;

    const double free1080 = kBudgetMiB - kOccupiedMiB - kTargetGrowth1080MiB;

    EXPECT_GE( free1080, kBodyMiB * Graphic::kCloudModellingAtlasMaxSlabs );
    EXPECT_LT( free1080, kBodyMiB * ( Graphic::kCloudModellingAtlasMaxSlabs + 1u ) );

    // ... and the body really is 4.00 MiB, measured rather than quoted.
    EXPECT_EQ( static_cast<double>( Assets::kCloudModellingVoxelBytes ) / ( 1024.0 * 1024.0 ), kBodyMiB );
}

TEST( CloudSeam, TheSculptedBodyIsErodedByTheFirstSpeciesVolume )
{
    // A HERO CLOUD IS NOT A SPECIES. It has no `.decloudtype` and therefore names no noise volume, so when
    // phase NV gave every species a volume of its own the sculpted producer had to be given SOME answer —
    // and the answer chosen was the one it already had. Before NV the layer bound one volume, the first
    // filled slot's, which is species 0's; after NV a body still reads species 0's. So this test asserts
    // that a decision was made and stayed made, rather than that a number is pretty.
    //
    // IT IS ASSERTED BY WATCHING THE CALLBACK, not by reading the seam's source. CloudFieldReference's
    // opposite number records every slot the seam asks for; if the sculpted producer ever started
    // carrying an instance's own slot, or the union began leaking the procedural winner's into an
    // authored sample, this set would gain a second entry.
    NoiseSlotsAsked().clear();

    CloudFieldParams params = DefaultParams();

    // A body in the sky, and the ordinary erosion path over it — the same walk the union tests take.
    ClearInstances();
    AddInstance( MakeInstance( glm::vec3( 0.0f, 1.4f, 0.0f ) ) );

    int inside = 0;

    for ( int iz = 0; iz < 24; ++iz )
    {
        for ( int ix = 0; ix < 24; ++ix )
        {
            for ( int ih = 0; ih < 6; ++ih )
            {
                const float fraction = ( ih + 0.5f ) / 6.0f;
                const vec3  at( -1.0f + 2.0f * ( ix + 0.5f ) / 24.0f, fraction * 2.8f,
                                -1.0f + 2.0f * ( iz + 0.5f ) / 24.0f );

                const CloudFieldSample sample = SampleCloudField( params, fraction, at );
                if ( sample.Profile <= 0.0f )
                    continue;

                ++inside;
                (void)CloudSampleDensity( params, sample, at );
            }
        }
    }

    std::printf( "[CloudSeam] %d samples eroded; the seam asked for %zu distinct noise slot(s)\n", inside,
                 NoiseSlotsAsked().size() );

    ASSERT_GT( inside, 0 ) << "no sample reached the erosion, so no slot was ever asked for";
    ASSERT_EQ( NoiseSlotsAsked().size(), 1u ) << "the seam asked for more than one noise volume in a layer "
                                                 "that binds one";
    EXPECT_EQ( *NoiseSlotsAsked().begin(), 0 ) << "a sculpted body was eroded by a volume that is not the "
                                                  "first species' — which is a change of what A0 shipped";
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );

    // THE ATLAS IS BOUND BEFORE ANY TEST RUNS, and it is the same rule the renderer works by: a sampler
    // with no image behind it is not an unused descriptor, it is an invalid one. Here it is an empty
    // vector and the fetch walks off the end of it, which is the C++ shape of exactly that defect —
    // found by this suite crashing rather than failing, which is why it is set HERE and not inside the
    // fetch, where an empty atlas would have been quietly papered over.
    SetSingleBodyAtlas();

    return RUN_ALL_TESTS();
}
