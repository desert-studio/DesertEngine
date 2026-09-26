#include "EnvironmentBake.hpp"

#include <Common/Content/DerivedDataCache.hpp>
#include <Common/Core/Logger.hpp>
#include <Common/Utilities/Crc32c.hpp>
#include <Common/Utilities/FileSystem.hpp>
#include <Common/Core/Constants.hpp>

#include <Engine/Assets/TextureSourceAsset.hpp>
#include <Engine/Assets/Serialization/TextureBinary.hpp>
#include <Engine/Core/Formats/BlockCompression.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanImage.hpp>

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace Desert::Graphic
{
    namespace Ser = Assets::Serialization;

    Common::ResultStr<std::shared_ptr<ImageCube>>
    CreateBakedEnvironmentCube( Core::Formats::ImageCubeSpecification spec, const std::filesystem::path& path )
    {
        auto cube = SP_CAST( ImageCube, ImageCube::Create( spec, nullptr ) );
        if ( !cube )
        {
            return Common::MakeFormattedError<std::shared_ptr<ImageCube>>(
                 "'{}' decoded but its GPU cube was not created.", path.string() );
        }
        return Common::MakeSuccess( std::move( cube ) );
    }

    Common::BoolResultStr WriteBakedEnvironmentCube( const std::filesystem::path& path, ImageCube& cube,
                                                     const std::string& sourceKey, const uint64_t sourceSignature,
                                                     const uint64_t bakeSignature )
    {
        auto* vulkanCube = dynamic_cast<API::Vulkan::VulkanImageCube*>( &cube );
        if ( !vulkanCube )
            return Common::MakeError<bool>( "the environment bake can only read back a Vulkan cube" );

        const uint32_t faceSize = cube.GetWidth();
        const uint32_t mips     = cube.GetMipmapLevels();

        auto readback = vulkanCube->RT_ReadAllLevels();
        if ( !readback.IsSuccess() )
            return Common::MakeError<bool>( readback.GetError() );

        Ser::TextureAssetData data;
        data.SourcePath        = sourceKey;
        data.SourceContentHash = sourceSignature;
        data.EncoderHash       = bakeSignature;
        data.Width             = faceSize;
        data.Height            = faceSize;
        data.LayerCount        = Ser::kTextureCubeLayerCount;
        data.Kind              = Ser::TextureKind::Cube;
        data.Format            = Assets::kEnvironmentComputeFormat;

        // THE READBACK'S LAYOUT AND THE PLACER'S INPUT ARE THE SAME LAYOUT, and that is asserted rather
        // than assumed: `RT_ReadAllLevels` documents "tightly packed in table order" and
        // `TightlyPackedChainBytes` is the size that phrase means. A disagreement here is the middle
        // link dropping a property, so it is a refusal with both numbers and not a silent truncation.
        const uint64_t expected = Ser::TightlyPackedChainBytes(
             faceSize, faceSize, mips, Ser::kTextureCubeLayerCount, Assets::kEnvironmentComputeFormat );
        if ( readback.GetValue().size() != expected )
        {
            return Common::MakeFormattedError<bool>(
                 "the cube read back as {} bytes and {} levels x {} faces of a {}-texel face is {}.",
                 readback.GetValue().size(), mips, Ser::kTextureCubeLayerCount, faceSize, expected );
        }

        auto table = Ser::BuildLevelTable( faceSize, faceSize, mips, Ser::kTextureCubeLayerCount,
                                           Assets::kEnvironmentComputeFormat, readback.GetValue(), data.Pixels );
        if ( !table.IsSuccess() )
            return Common::MakeError<bool>( table.GetError() );
        data.Levels = table.ExtractValue();

        // ── WHAT THE BLOCK FORMAT IS ABOUT TO LOSE, COUNTED BEFORE IT LOSES IT ───────────────────
        //
        // BC6H stores half-float endpoints, so everything above `kBC6HLargestValue` is clamped by the
        // encoder. On the procedural sky that never happens (no sun disc is baked); on a real HDRI it
        // does, and silently: rural_asphalt_road_2k.hdr's sun reaches 131072 and nine of its texels hold
        // 13.4 % of the panorama's luminance ABOVE the ceiling. So the chain is censused here, per level,
        // and a clamp that is about to happen is a line with its numbers — and a non-finite texel, which
        // the encoder would write as black, is a refused cache entry rather than a black sun.
        //
        // WHY A CLAMP AND NOT A DIFFERENT FORMAT. The convolved terms (irradiance, prefilter levels past
        // the mirror) are integrated from the RGBA32F panorama BEFORE this encode, so they keep the sun's
        // energy; what is clamped is the sharp image of the sun itself, which is white after exposure
        // either way. That is also where UE draws the line: an HDRI is captured into a half-float sky
        // cube, and a sun that must light the scene at full strength is a DirectionalLight, not texels.
        {
            Core::Formats::BC6HCeilingCensus whole;
            double                           worstLoss  = 0.0;
            uint32_t                         worstLevel = 0;
            for ( uint32_t mip = 0; mip < mips; ++mip )
            {
                Core::Formats::BC6HCeilingCensus level;
                for ( uint32_t face = 0; face < Ser::kTextureCubeLayerCount; ++face )
                {
                    const Ser::TextureLevel& image =
                         data.Levels[Ser::TextureLevelIndex( mip, face, Ser::kTextureCubeLayerCount )];
                    std::vector<float> texels( static_cast<size_t>( image.ByteSize / sizeof( float ) ) );
                    std::memcpy( texels.data(), data.Pixels.data() + image.ByteOffset, image.ByteSize );
                    const auto faceCensus = Core::Formats::CensusBC6HCeiling( texels.data(), texels.size() / 4u );
                    level.NonFiniteChannels += faceCensus.NonFiniteChannels;
                    level.ClampedTexels += faceCensus.ClampedTexels;
                    level.Peak = std::max( level.Peak, faceCensus.Peak );
                    level.Sum += faceCensus.Sum;
                    level.LostAboveCeiling += faceCensus.LostAboveCeiling;
                }
                if ( level.LostFraction() > worstLoss )
                {
                    worstLoss  = level.LostFraction();
                    worstLevel = mip;
                }
                whole.NonFiniteChannels += level.NonFiniteChannels;
                whole.ClampedTexels += level.ClampedTexels;
                whole.Peak = std::max( whole.Peak, level.Peak );
            }

            if ( whole.NonFiniteChannels > 0 )
            {
                return Common::MakeFormattedError<bool>(
                     "the baked cube holds {} non-finite channel value(s); BC6H would store them as black, "
                     "so this bake is not cached.",
                     whole.NonFiniteChannels );
            }
            if ( whole.ClampedTexels > 0 )
            {
                LOG_WARN( "[EnvironmentBake] '{}': {} texel(s) exceed the BC6H ceiling {} (peak {}); the cached "
                          "cube loses {:.2f} % of level {}'s energy to the clamp. The bake in memory is "
                          "unclamped RGBA32F; the NEXT load reads the clamped file.",
                          path.string(), whole.ClampedTexels, Core::Formats::kBC6HLargestValue, whole.Peak,
                          100.0 * worstLoss, worstLevel );
            }
        }

        // ── AND THEN THE SIXTEEN BYTES A TEXEL BECOME ONE ────────────────────────────────────────
        //
        // This is the step the whole T3 chain was ordered around: the chain is in the file (so there is
        // no `vkCmdBlitImage` to be refused by `blitDst = 0`), the container can express a layered
        // image, and the format table can express a block. A radiance cube at 1024 with its mips is
        // 128 MiB of RGBA32F and 8 MiB of BC6H.
        //
        // A FAILURE HERE IS NOT FATAL AND IS NOT SILENT. The uncompressed chain in hand is a correct
        // cache entry and the load accepts either format, so the fall-through writes a bigger file
        // rather than no file — but it says so, because "the cube is somehow 128 MiB again" is exactly
        // the kind of thing that is discovered six weeks later.
        auto blocks = Ser::BlockCompressChain( faceSize, faceSize, mips, Ser::kTextureCubeLayerCount,
                                               Assets::kEnvironmentComputeFormat, Assets::kEnvironmentStoredFormat,
                                               data.Levels, data.Pixels );
        if ( !blocks.IsSuccess() )
        {
            LOG_WARN( "[EnvironmentBake] '{}' is cached uncompressed: {}", path.string(), blocks.GetError() );
        }
        else
        {
            std::vector<unsigned char> blockPixels;
            auto                       blockTable =
                 Ser::BuildLevelTable( faceSize, faceSize, mips, Ser::kTextureCubeLayerCount,
                                       Assets::kEnvironmentStoredFormat, blocks.GetValue(), blockPixels );
            if ( !blockTable.IsSuccess() )
            {
                LOG_WARN( "[EnvironmentBake] '{}' is cached uncompressed: {}", path.string(),
                          blockTable.GetError() );
            }
            else
            {
                LOG_INFO( "[EnvironmentBake] '{}' encoded to BC6H: {:.2f} MiB instead of {:.2f} MiB.",
                          path.string(), static_cast<double>( blockPixels.size() ) / ( 1024.0 * 1024.0 ),
                          static_cast<double>( data.Pixels.size() ) / ( 1024.0 * 1024.0 ) );
                data.Format = Assets::kEnvironmentStoredFormat;
                data.Levels = blockTable.ExtractValue();
                data.Pixels = std::move( blockPixels );
            }
        }

        std::error_code ec;
        std::filesystem::create_directories( path.parent_path(), ec );

        const std::string bytes = Ser::EncodeTextureBinary( data );
        if ( bytes.empty() )
            return Common::MakeError<bool>( "the cooked container refused the shape this bake produced" );

        return Common::Utils::FileSystem::WriteContentToFileAtomic( path, bytes );
    }
} // namespace Desert::Graphic
