// THE FIRST ENVIRONMENT RUN DRAWS WHAT EVERY LATER RUN DRAWS (ENV-FIRST1).
//
// The bake computes its cubes in RGBA32F; the cache stores them as BC6H, which clamps every channel above
// `kBC6HLargestValue` and quantises the rest. The run that baked used to keep drawing its own RGBA32F cubes
// while every later run drew the BC6H file -- RM1 measured up to 8/255 on 33 % of the pixels between run 1
// and run 2. The writer now adopts what `CacheAndReloadBakedEnvironmentCube` hands back, so these tests pin
// the RELATION: the cube handed to the running environment is byte-for-byte the cube the next run's cache
// hit reads, and it carries the clamp the file carries. Device-free: the encode, the write and the read
// are all CPU.

#include <Engine/Graphic/Environment/EnvironmentBake.hpp>
#include <Engine/Assets/Serialization/EnvironmentStaging.hpp>
#include <Engine/Assets/Serialization/TextureBinary.hpp>
#include <Engine/Core/Formats/BlockCompression.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <variant>
#include <vector>

#include "../../TestSupport/scratch_dir.hpp"

namespace
{
    namespace Ser                       = Desert::Assets::Serialization;
    namespace Formats                   = Desert::Core::Formats;
    constexpr uint32_t kFace            = 8;
    constexpr uint32_t kMips            = 4; // 8, 4, 2, 1
    constexpr float    kAboveTheCeiling = 100000.0f;
    constexpr uint64_t kSourceSignature = 0x1234'5678'9abc'def0ull;
    constexpr uint64_t kBakeSignature   = 0x0fed'cba9'8765'4321ull;

    // A cube chain as the GPU readback delivers it: RGBA32F, tightly packed in table order. Every
    // seventh float is a sun texel far above what BC6H can store, the rest a plain sky radiance.
    std::vector<uint8_t> BakedChainWithASun()
    {
        const uint64_t     bytes = Ser::TightlyPackedChainBytes( kFace, kFace, kMips, Ser::kTextureCubeLayerCount,
                                                                 Desert::Assets::kEnvironmentComputeFormat );
        std::vector<float> texels( bytes / sizeof( float ), 1.5f );
        for ( size_t i = 0; i < texels.size(); i += 7 )
            texels[i] = kAboveTheCeiling;
        std::vector<uint8_t> chain( bytes );
        std::memcpy( chain.data(), texels.data(), bytes );
        return chain;
    }

    Desert::Graphic::EnvironmentCacheEntry EntryAt( const std::filesystem::path& path )
    {
        return { .Path            = path,
                 .SourceKey       = "Tests/EnvironmentFirstRun.hdr",
                 .SourceSignature = kSourceSignature,
                 .BakeSignature   = kBakeSignature,
                 .FaceSize        = kFace,
                 .Mips            = kMips,
                 .Tag             = std::string( Desert::Assets::kEnvRadianceTag ) };
    }

    const std::vector<unsigned char>& Bytes( const Formats::ImageCubeSpecification& spec )
    {
        return std::get<std::vector<unsigned char>>( spec.Data );
    }
} // namespace

// The relation itself: what this run adopts == what the next run's cache hit loads. Goes red the moment
// the writer hands the running environment anything but the file -- the in-memory RGBA32F chain included.
TEST( EnvironmentFirstRun, TheAdoptedCubeIsTheCubeTheNextRunLoads )
{
    Desert::TestSupport::ScratchDir dir( "desert-env-first-run" );
    const std::filesystem::path     path = dir.Path() / "radiance.tex";

    auto adopted = Desert::Graphic::CacheAndReloadBakedEnvironmentCube( EntryAt( path ), BakedChainWithASun() );
    ASSERT_TRUE( adopted.IsSuccess() ) << adopted.GetError();

    auto nextRun = Desert::Assets::ReadBakedEnvironmentCube( path, Desert::Assets::kEnvRadianceTag, kFace, kMips,
                                                             kSourceSignature, kBakeSignature );
    ASSERT_TRUE( nextRun.IsSuccess() ) << nextRun.GetError();

    const auto& run1 = adopted.GetValue();
    const auto& run2 = nextRun.GetValue();
    EXPECT_EQ( run1.Format, run2.Format );
    EXPECT_EQ( run1.FaceSize, run2.FaceSize );
    EXPECT_EQ( run1.Mips, run2.Mips );
    EXPECT_EQ( run1.Tag, run2.Tag );
    ASSERT_EQ( run1.Levels.size(), run2.Levels.size() );
    for ( size_t i = 0; i < run1.Levels.size(); ++i )
    {
        EXPECT_EQ( run1.Levels[i].ByteOffset, run2.Levels[i].ByteOffset ) << "span " << i;
        EXPECT_EQ( run1.Levels[i].ByteSize, run2.Levels[i].ByteSize ) << "span " << i;
    }
    EXPECT_TRUE( Bytes( run1 ) == Bytes( run2 ) ) << "the first run would draw different texels from the second";
}

// And it is the ENCODED cube, not the computed one: the format is the stored BC6H, and the sun decodes at
// the ceiling the file clamped it to -- not at the 100000 the bake computed and RGBA32F would have kept.
TEST( EnvironmentFirstRun, TheAdoptedCubeCarriesTheClampTheFileCarries )
{
    Desert::TestSupport::ScratchDir dir( "desert-env-first-run" );
    const std::filesystem::path     path = dir.Path() / "radiance.tex";

    auto adopted = Desert::Graphic::CacheAndReloadBakedEnvironmentCube( EntryAt( path ), BakedChainWithASun() );
    ASSERT_TRUE( adopted.IsSuccess() ) << adopted.GetError();
    const auto& cube = adopted.GetValue();
    ASSERT_EQ( cube.Format, Desert::Assets::kEnvironmentStoredFormat )
         << "the running environment would keep the uncompressed bake the next run never sees";

    // Level 0, face 0: spans are indexed `level * 6 + face`.
    ASSERT_FALSE( cube.Levels.empty() );
    const auto& span    = cube.Levels[0];
    const auto& blocks  = Bytes( cube );
    auto        decoded = Formats::BlockDecompressImage( kFace, kFace, cube.Format, Formats::ImageFormat::RGBA32F,
                                                         blocks.data() + span.ByteOffset, span.ByteSize );
    ASSERT_TRUE( decoded.IsSuccess() ) << decoded.GetError();
    const auto&        texelBytes = decoded.GetValue();
    std::vector<float> texels( texelBytes.size() / sizeof( float ) );
    std::memcpy( texels.data(), texelBytes.data(), texels.size() * sizeof( float ) );
    ASSERT_FALSE( texels.empty() );
    const float peak = *std::max_element( texels.begin(), texels.end() );
    EXPECT_LE( peak, Formats::kBC6HLargestValue );
    EXPECT_GT( peak, 1.5f ) << "the sun texels vanished instead of being clamped";
}
