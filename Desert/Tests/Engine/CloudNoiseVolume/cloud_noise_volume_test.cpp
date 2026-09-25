#include "CloudNoiseVolumeReference.hpp"

#include <Engine/Assets/CloudNoiseVolume.hpp>
#include <Engine/Assets/CloudNoiseVolumeGenerator.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <span>
#include <string>
#include <vector>

using namespace Desert::Assets;
namespace Ref = Desert::Tests::CloudNoiseVolumeRef;

namespace
{
    // The smallest legal volume. Everything asserted here is a property of the FORMAT and the LAYOUT, not
    // of the resolution, and 64^3 costs a fraction of a second where 128^3 costs eighty in an unoptimised
    // build. The one test that has to speak about the shipped size says so and uses it.
    CloudNoiseVolumeParams SmallParams()
    {
        CloudNoiseVolumeParams params;
        params.Resolution                = 64u;
        params.Seed                      = 4242u;
        params.CurlStrength              = 0.25f;
        params.WispyPeriodLowFrequency   = 1.0f;
        params.WispyPeriodHighFrequency  = 2.0f;
        params.BillowPeriodLowFrequency  = 3.0f;
        params.BillowPeriodHighFrequency = 4.0f;
        return params;
    }

    CloudNoiseVolumeData Generate( const CloudNoiseVolumeParams& params )
    {
        auto result = GenerateCloudNoiseVolume( params );
        EXPECT_TRUE( result ) << result.GetError();
        return result.ExtractValue();
    }
} // namespace

// ---------------------------------------------------------------------------------------------------
// The generator: a pure function, and the container's promise that its header is a recipe rests on it.
// ---------------------------------------------------------------------------------------------------

TEST( CloudNoiseGenerator, TheSameParametersProduceTheSameBytes )
{
    // If this ever stops holding, the header stops being a recipe and becomes a description of a file that
    // cannot be reproduced — which is the difference between an asset an artist can iterate on and a blob.
    const CloudNoiseVolumeData first  = Generate( SmallParams() );
    const CloudNoiseVolumeData second = Generate( SmallParams() );

    ASSERT_EQ( first.Voxels.size(), second.Voxels.size() );
    EXPECT_EQ( first.Voxels, second.Voxels );
}

TEST( CloudNoiseGenerator, TheOutputIsExactlyFourBytesPerVoxel )
{
    const CloudNoiseVolumeData volume = Generate( SmallParams() );
    EXPECT_EQ( volume.Voxels.size(), 64u * 64u * 64u * 4u );
    EXPECT_EQ( volume.VoxelCount(), 64u * 64u * 64u );
    EXPECT_EQ( volume.GeneratorVersion, kCloudNoiseGeneratorVersion );
}

TEST( CloudNoiseGenerator, AnInvalidParameterSetIsRefusedRatherThanClamped )
{
    // Clamping would produce a volume the artist did not ask for and a header that lies about how it was
    // made. Every message names the number that was wrong.
    CloudNoiseVolumeParams params = SmallParams();
    params.Resolution             = 100u; // not a power of two
    auto refused                  = GenerateCloudNoiseVolume( params );
    EXPECT_FALSE( refused );
    EXPECT_NE( refused.GetError().find( "100" ), std::string::npos ) << refused.GetError();

    params              = SmallParams();
    params.CurlStrength = 0.9f;
    refused             = GenerateCloudNoiseVolume( params );
    EXPECT_FALSE( refused );
    EXPECT_NE( refused.GetError().find( "Curl Strength" ), std::string::npos ) << refused.GetError();

    params                         = SmallParams();
    params.WispyPeriodLowFrequency = 2.5f; // not a whole number of lattice cells: does not tile
    refused                        = GenerateCloudNoiseVolume( params );
    EXPECT_FALSE( refused );
    EXPECT_NE( refused.GetError().find( "Wispy Period LF" ), std::string::npos ) << refused.GetError();

    params                           = SmallParams();
    params.BillowPeriodHighFrequency = 16.0f; // 64/16 = 4 voxels per cell: quantises onto the voxel grid
    refused                          = GenerateCloudNoiseVolume( params );
    EXPECT_FALSE( refused );
    EXPECT_NE( refused.GetError().find( "voxels per cell" ), std::string::npos ) << refused.GetError();
}

// ---------------------------------------------------------------------------------------------------
// THE RELATION: what the generator writes is what the shader reads.
//
// This is the test the contract asks for by name, and it is not a test of a function. Four things have to
// agree for a volume to render as the maths intended — the voxel-centre convention, the byte order of the
// payload, the 8-bit quantisation and the channel order — and each of them is individually correct in
// every implementation that ever got it wrong.
// ---------------------------------------------------------------------------------------------------

TEST( CloudNoiseVolumeRelation, EveryVoxelHoldsTheValueTheSharedNoiseTextProducesAtItsCentre )
{
    const CloudNoiseVolumeParams params = SmallParams();
    const CloudNoiseVolumeData   volume = Generate( params );

    const uint32_t n = params.Resolution;

    // The addressing a `texture()` fetch performs, written out: the shader samples at uvw, the sampler
    // maps uvw to voxel (uvw * n - 0.5) and, at a voxel centre, lands exactly on one texel. This walks
    // those centres and asks the SHARED TEXT what it should have found there.
    for ( uint32_t z = 0; z < n; z += 3 )
    {
        for ( uint32_t y = 0; y < n; y += 3 )
        {
            for ( uint32_t x = 0; x < n; x += 3 )
            {
                const Ref::vec3 uvw{ ( x + 0.5f ) / n, ( y + 0.5f ) / n, ( z + 0.5f ) / n };
                const Ref::vec4 expected = Ref::CloudNoiseVolumeChannels(
                     uvw, params.Seed, params.CurlStrength, params.WispyPeriodLowFrequency,
                     params.WispyPeriodHighFrequency, params.BillowPeriodLowFrequency,
                     params.BillowPeriodHighFrequency );

                // x fastest, then y, then z. This IS the layout vkCmdCopyBufferToImage assumes for a
                // whole-volume copy with no row padding; a transposed index would render as a sky whose
                // structure runs the wrong way and would look like a different noise, not like a bug.
                const size_t index = ( ( static_cast<size_t>( z ) * n + y ) * n + x ) * 4u;

                // Half a step of 1/255 is the exact tolerance of round-to-nearest quantisation. Asserting
                // anything tighter would be asserting that eight bits carry more than eight bits.
                constexpr float kQuantisationStep = 1.0f / 255.0f;

                EXPECT_NEAR( volume.Voxels[index + 0] / 255.0f, expected.x, kQuantisationStep * 0.5f + 1e-6f )
                     << "R at " << x << "," << y << "," << z;
                EXPECT_NEAR( volume.Voxels[index + 1] / 255.0f, expected.y, kQuantisationStep * 0.5f + 1e-6f )
                     << "G at " << x << "," << y << "," << z;
                EXPECT_NEAR( volume.Voxels[index + 2] / 255.0f, expected.z, kQuantisationStep * 0.5f + 1e-6f )
                     << "B at " << x << "," << y << "," << z;
                EXPECT_NEAR( volume.Voxels[index + 3] / 255.0f, expected.w, kQuantisationStep * 0.5f + 1e-6f )
                     << "A at " << x << "," << y << "," << z;
            }
        }
    }
}

TEST( CloudNoiseVolumeRelation, TheVolumeTILESTheWayTheSamplerAssumesItDoes )
{
    // The sampler wraps with REPEAT, so the shader reading uvw and uvw+1 must get the same texel — and it
    // will, whatever is in the volume. What has to be true for that to MEAN anything is that the last voxel
    // of an axis continues into the first, which is a property of the generator's periodicity and not of
    // the sampler. Measured as a step: neighbours across the wrap must be no further apart than typical
    // neighbours inside the volume, or there is a seam.
    const CloudNoiseVolumeParams params = SmallParams();
    const CloudNoiseVolumeData   volume = Generate( params );
    const uint32_t               n      = params.Resolution;

    auto voxel = [&]( uint32_t x, uint32_t y, uint32_t z, int channel )
    { return volume.Voxels[( ( static_cast<size_t>( z ) * n + y ) * n + x ) * 4u + channel] / 255.0f; };

    double interiorStep = 0.0;
    double wrapStep     = 0.0;
    int    interiorN    = 0;
    int    wrapN        = 0;

    for ( uint32_t z = 0; z < n; ++z )
        for ( uint32_t y = 0; y < n; ++y )
            for ( int channel = 0; channel < 4; ++channel )
            {
                for ( uint32_t x = 1; x < n; ++x )
                {
                    interiorStep += std::abs( voxel( x, y, z, channel ) - voxel( x - 1, y, z, channel ) );
                    ++interiorN;
                }
                wrapStep += std::abs( voxel( 0, y, z, channel ) - voxel( n - 1, y, z, channel ) );
                ++wrapN;
            }

    const double interiorMean = interiorStep / interiorN;
    const double wrapMean     = wrapStep / wrapN;
    std::printf( "[CloudNoiseVolume] mean |step| interior %.5f, across the wrap %.5f\n", interiorMean, wrapMean );

    // A field that did not tile would jump by the field's whole range at the wrap — several times the
    // typical neighbour step, and unmistakable. 1.5x leaves room for the wrap plane happening to fall
    // somewhere busy without leaving room for a seam.
    EXPECT_LT( wrapMean, interiorMean * 1.5 ) << "the volume does not continue across its own boundary";
}

// ---------------------------------------------------------------------------------------------------
// The container.
// ---------------------------------------------------------------------------------------------------

TEST( CloudNoiseContainer, ARoundTripReturnsEverythingItWasGiven )
{
    CloudNoiseVolumeData original = Generate( SmallParams() );
    original.Guid                 = Common::Content::AssetGuid::Generate();

    EXPECT_EQ( EncodeCloudNoisePayload( original ).size(), kCloudNoiseHeaderSize + original.Voxels.size() );
    const auto encoded = EncodeCloudNoiseVolume( original );
    ASSERT_TRUE( encoded ) << encoded.GetError();

    auto decoded = DecodeCloudNoiseVolume( encoded.GetValue() );
    ASSERT_TRUE( decoded ) << decoded.GetError();

    const CloudNoiseVolumeData& read = decoded.GetValue();
    EXPECT_EQ( read.Guid, original.Guid );
    EXPECT_EQ( read.Params.Resolution, original.Params.Resolution );
    EXPECT_EQ( read.Params.Seed, original.Params.Seed );
    EXPECT_FLOAT_EQ( read.Params.CurlStrength, original.Params.CurlStrength );
    EXPECT_FLOAT_EQ( read.Params.WispyPeriodLowFrequency, original.Params.WispyPeriodLowFrequency );
    EXPECT_FLOAT_EQ( read.Params.WispyPeriodHighFrequency, original.Params.WispyPeriodHighFrequency );
    EXPECT_FLOAT_EQ( read.Params.BillowPeriodLowFrequency, original.Params.BillowPeriodLowFrequency );
    EXPECT_FLOAT_EQ( read.Params.BillowPeriodHighFrequency, original.Params.BillowPeriodHighFrequency );
    EXPECT_EQ( read.GeneratorVersion, original.GeneratorVersion );
    EXPECT_EQ( read.Voxels, original.Voxels );
}

TEST( CloudNoiseContainer, TheEnvelopeStatesKindGuidAndTheContainerVersionUnderDcnv )
{
    // A volume never written has a null GUID; the encoder mints one, and a re-encode of what it decoded
    // keeps it - the property every reference to the volume leans on.
    const auto first = EncodeCloudNoiseVolume( Generate( SmallParams() ) );
    ASSERT_TRUE( first ) << first.GetError();

    namespace CC                        = Common::Content;
    const CC::SubsystemVersion kKnown[] = { { kCloudNoiseSubsystemTag, kCloudNoiseContainerVersion } };
    const auto                 header   = CC::ReadEnvelopeHeader( std::as_bytes( std::span( first.GetValue() ) ),
                                                                  CC::AssetHeaderReadContext{ kKnown } );
    ASSERT_TRUE( header ) << header.GetError();
    EXPECT_EQ( header.GetValue().Asset.Kind, CC::ContentKind::CloudNoiseVolume );
    EXPECT_FALSE( header.GetValue().Asset.Guid.IsNull() );
    ASSERT_EQ( header.GetValue().Asset.Subsystems.size(), 1u );
    EXPECT_EQ( header.GetValue().Asset.Subsystems[0].Tag, kCloudNoiseSubsystemTag );
    EXPECT_EQ( header.GetValue().Asset.Subsystems[0].Version, 3u );

    const auto decoded = DecodeCloudNoiseVolume( first.GetValue() );
    ASSERT_TRUE( decoded ) << decoded.GetError();
    EXPECT_EQ( decoded.GetValue().Guid, header.GetValue().Asset.Guid );
    const auto second = EncodeCloudNoiseVolume( decoded.GetValue() );
    ASSERT_TRUE( second ) << second.GetError();
    EXPECT_EQ( second.GetValue(), first.GetValue() ) << "a decode/encode round trip must be byte-identical";
}

TEST( CloudNoiseContainer, ABareContainerIsRefusedByNameAndNamesTheMigrator )
{
    // The version-2 shape: magic, container version 2, then the payload. It has no GUID to give the volume.
    std::vector<unsigned char> bare( kCloudNoiseMagic, kCloudNoiseMagic + sizeof( kCloudNoiseMagic ) );
    bare.insert( bare.end(), { 2u, 0u, 0u, 0u } );
    const std::vector<unsigned char> payload = EncodeCloudNoisePayload( Generate( SmallParams() ) );
    bare.insert( bare.end(), payload.begin(), payload.end() );

    auto decoded = DecodeCloudNoiseVolume( bare );
    ASSERT_FALSE( decoded );
    EXPECT_NE( decoded.GetError().find( "bare 'DCNV' container version 2" ), std::string::npos )
         << decoded.GetError();
    EXPECT_NE( decoded.GetError().find( "SceneMigrator" ), std::string::npos ) << decoded.GetError();
}

TEST( CloudNoiseContainer, ThePayloadCarriesTheDeckSChannelOrder )
{
    const std::vector<unsigned char> payload = EncodeCloudNoisePayload( Generate( SmallParams() ) );

    ASSERT_GE( payload.size(), kCloudNoiseHeaderSize );
    // The channel meanings live at payload bytes 12..27, one 32-bit word each, in the deck's order.
    for ( uint32_t channel = 0; channel < 4u; ++channel )
        EXPECT_EQ( payload[12 + channel * 4u], static_cast<unsigned char>( channel ) );

    EXPECT_STREQ( CloudNoiseChannelName( CloudNoiseChannel::CurlyAlligatorLowFrequency ),
                  "Curly-Alligator LF (wispy, coarse)" );
    EXPECT_STREQ( CloudNoiseChannelName( CloudNoiseChannel::AlligatorHighFrequency ),
                  "Alligator HF (billowy, fine)" );
}

TEST( CloudNoiseContainer, ATruncatedPayloadIsRefusedAndSaysSo )
{
    std::vector<unsigned char> payload = EncodeCloudNoisePayload( Generate( SmallParams() ) );

    // Cut in the middle of the voxels: the shape a half-finished write leaves behind.
    payload.resize( payload.size() - 4096u );

    auto decoded = DecodeCloudNoisePayload( payload );
    EXPECT_FALSE( decoded );
    EXPECT_NE( decoded.GetError().find( "truncated" ), std::string::npos ) << decoded.GetError();
}

TEST( CloudNoiseContainer, APayloadShorterThanItsOwnHeaderIsRefused )
{
    std::vector<unsigned char> payload = EncodeCloudNoisePayload( Generate( SmallParams() ) );
    payload.resize( kCloudNoiseHeaderSize - 1u );

    auto decoded = DecodeCloudNoisePayload( payload );
    EXPECT_FALSE( decoded );
    EXPECT_NE( decoded.GetError().find( "header" ), std::string::npos ) << decoded.GetError();
}

TEST( CloudNoiseContainer, ACORRUPTEDPayloadIsRefusedByItsChecksum )
{
    // The failure the length check cannot see, and the expensive one: a file whose middle was damaged
    // renders as clouds with a wrong edge rather than as an error, which is the least diagnosable thing
    // this subsystem can do.
    std::vector<unsigned char> payload = EncodeCloudNoisePayload( Generate( SmallParams() ) );

    const size_t middle = kCloudNoiseHeaderSize + payload.size() / 2u;
    payload[middle]     = static_cast<unsigned char>( payload[middle] ^ 0x01u ); // ONE bit

    auto decoded = DecodeCloudNoisePayload( payload );
    EXPECT_FALSE( decoded );
    EXPECT_NE( decoded.GetError().find( "checksum" ), std::string::npos ) << decoded.GetError();
}

TEST( CloudNoiseContainer, AFileThatIsNotAVolumeAtAllIsRefused )
{
    std::vector<unsigned char> notAVolume( kCloudNoiseHeaderSize + 64u, 0u );
    notAVolume[0] = 'P';
    notAVolume[1] = 'N';
    notAVolume[2] = 'G';

    auto decoded = DecodeCloudNoiseVolume( notAVolume );
    EXPECT_FALSE( decoded );
    EXPECT_NE( decoded.GetError().find( "not a cloud noise volume envelope" ), std::string::npos )
         << decoded.GetError();
}

TEST( CloudNoiseContainer, AFutureContainerVersionIsRefusedByNumberRatherThanMisread )
{
    // Written by hand at a version this build does not know; the envelope reader refuses the tag's version.
    namespace CC                             = Common::Content;
    const std::vector<unsigned char> payload = EncodeCloudNoisePayload( Generate( SmallParams() ) );
    CC::AssetEnvelope                envelope;
    envelope.Asset.Kind       = CC::ContentKind::CloudNoiseVolume;
    envelope.Asset.Guid       = CC::AssetGuid::Generate();
    envelope.Asset.Subsystems = { { kCloudNoiseSubsystemTag, kCloudNoiseContainerVersion + 7u } };
    const std::span<const std::byte> payloadBytes = std::as_bytes( std::span( payload ) );
    envelope.Sections.push_back( { CC::EnvelopeSection::Payload, CC::EnvelopeCodec::Stored,
                                   std::vector<std::byte>( payloadBytes.begin(), payloadBytes.end() ) } );
    const auto file = CC::WriteAssetEnvelope( envelope );
    ASSERT_TRUE( file ) << file.GetError();
    std::vector<unsigned char> bytes( file.GetValue().size() );
    std::memcpy( bytes.data(), file.GetValue().data(), file.GetValue().size() );

    auto decoded = DecodeCloudNoiseVolume( bytes );
    EXPECT_FALSE( decoded );
    EXPECT_NE( decoded.GetError().find( std::to_string( kCloudNoiseContainerVersion + 7u ) ), std::string::npos )
         << decoded.GetError();
}

TEST( CloudNoiseContainer, AHeaderWhoseResolutionDisagreesWithItsPayloadIsRefused )
{
    // Two statements of one fact, which is the class of defect this programme keeps meeting. Caught here,
    // once, rather than trusted into an out-of-bounds upload later.
    std::vector<unsigned char> payload = EncodeCloudNoisePayload( Generate( SmallParams() ) );
    payload[4]                         = 128u; // resolution says 128^3; the payload is 64^3

    auto decoded = DecodeCloudNoisePayload( payload );
    EXPECT_FALSE( decoded );
    EXPECT_NE( decoded.GetError().find( "payload bytes" ), std::string::npos ) << decoded.GetError();
}

TEST( CloudNoiseContainer, AnUnknownPixelFormatIsRefusedRatherThanReadAsBytes )
{
    std::vector<unsigned char> payload = EncodeCloudNoisePayload( Generate( SmallParams() ) );
    payload[8]                         = 3u; // some future half-float format

    auto decoded = DecodeCloudNoisePayload( payload );
    EXPECT_FALSE( decoded );
    EXPECT_NE( decoded.GetError().find( "format" ), std::string::npos ) << decoded.GetError();
}

TEST( CloudNoiseContainer, TheShippedSizeIsTheOneTheBudgetWasWrittenFor )
{
    // A test on the RELATION between the deck's number, the engine's format and the memory budget: 128^3
    // in RGBA8 is 8 MiB, and D-9 allows this subsystem 64. Stated here so a resolution changed on a whim
    // fails a test rather than a frame rate.
    CloudNoiseVolumeParams params;
    EXPECT_EQ( params.Resolution, 128u ) << "the default is the deck's size (p.96)";

    const uint64_t bytes = 128ull * 128ull * 128ull * 4ull;
    EXPECT_EQ( bytes, 8u * 1024u * 1024u );
    EXPECT_LT( bytes * 2u, 64ull * 1024ull * 1024ull ) << "two volumes at once must still fit the budget";

    EXPECT_TRUE( ValidateCloudNoiseVolumeParams( params ) ) << ValidateCloudNoiseVolumeParams( params ).GetError();
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
