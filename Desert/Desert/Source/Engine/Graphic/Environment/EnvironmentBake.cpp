#include "EnvironmentBake.hpp"

#include <Common/Content/DerivedDataCache.hpp>
#include <Common/Core/Logger.hpp>
#include <Common/Utilities/Crc32c.hpp>
#include <Common/Utilities/FileSystem.hpp>
#include <Common/Core/Constants.hpp>
#include <Common/Core/JobSystem.hpp>

#include <Engine/Assets/TextureSourceAsset.hpp>
#include <Engine/Assets/Serialization/TextureBinary.hpp>
#include <Engine/Core/Formats/BlockCompression.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanImage.hpp>

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <thread>

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

    Common::BoolResultStr EncodeBakedEnvironmentCube( const EnvironmentCacheEntry& entry,
                                                      const std::vector<uint8_t>&  levels )
    {
        const std::filesystem::path& path            = entry.Path;
        const std::string&           sourceKey       = entry.SourceKey;
        const uint64_t               sourceSignature = entry.SourceSignature;
        const uint64_t               bakeSignature   = entry.BakeSignature;
        const uint32_t               faceSize        = entry.FaceSize;
        const uint32_t               mips            = entry.Mips;

        using Clock        = std::chrono::steady_clock;
        const auto msSince = []( Clock::time_point from )
        { return std::chrono::duration<double, std::milli>( Clock::now() - from ).count(); };
        auto   stageStart = Clock::now();
        double tableMs = 0.0, censusMs = 0.0, blocksMs = 0.0, blockTableMs = 0.0, containerMs = 0.0;

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
        // than assumed: `RT_BeginReadAllLevels` documents "tightly packed in table order" and
        // `TightlyPackedChainBytes` is the size that phrase means. A disagreement here is the middle
        // link dropping a property, so it is a refusal with both numbers and not a silent truncation.
        const uint64_t expected = Ser::TightlyPackedChainBytes(
             faceSize, faceSize, mips, Ser::kTextureCubeLayerCount, Assets::kEnvironmentComputeFormat );
        if ( levels.size() != expected )
        {
            return Common::MakeFormattedError<bool>(
                 "the cube read back as {} bytes and {} levels x {} faces of a {}-texel face is {}.",
                 levels.size(), mips, Ser::kTextureCubeLayerCount, faceSize, expected );
        }

        auto table = Ser::BuildLevelTable( faceSize, faceSize, mips, Ser::kTextureCubeLayerCount,
                                           Assets::kEnvironmentComputeFormat, levels, data.Pixels );
        if ( !table.IsSuccess() )
            return Common::MakeError<bool>( table.GetError() );
        data.Levels = table.ExtractValue();
        tableMs     = msSince( stageStart );
        stageStart  = Clock::now();

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
            // EVERY FACE IS COUNTED IN PLACE, ON THE WORKERS, AND SUMMED IN THE OLD ORDER. Copying each
            // face out first and counting them one after another was 0.6 s of the radiance write (AL1-3b,
            // timed). The rows are 16-byte aligned RGBA32F inside `data.Pixels` (`kTextureLevelAlignment`),
            // so they are read where they lie; each face lands in its own slot and the fold below walks
            // the slots in (level, face) order, so the numbers the warning quotes do not depend on which
            // worker finished first.
            std::vector<Core::Formats::BC6HCeilingCensus> faces( data.Levels.size() );
            Common::JobSystem::Get().ParallelRanges(
                 faces.size(), 1u,
                 [&]( const std::size_t first, const std::size_t end )
                 {
                     for ( std::size_t row = first; row < end; ++row )
                     {
                         const Ser::TextureLevel& image = data.Levels[row];
                         faces[row]                     = Core::Formats::CensusBC6HCeiling(
                              reinterpret_cast<const float*>( data.Pixels.data() + image.ByteOffset ),
                              static_cast<std::size_t>( image.ByteSize / ( 4u * sizeof( float ) ) ) );
                     }
                 } );

            Core::Formats::BC6HCeilingCensus whole;
            double                           worstLoss  = 0.0;
            uint32_t                         worstLevel = 0;
            for ( uint32_t mip = 0; mip < mips; ++mip )
            {
                Core::Formats::BC6HCeilingCensus level;
                for ( uint32_t face = 0; face < Ser::kTextureCubeLayerCount; ++face )
                {
                    const auto& faceCensus =
                         faces[Ser::TextureLevelIndex( mip, face, Ser::kTextureCubeLayerCount )];
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
        censusMs   = msSince( stageStart );
        stageStart = Clock::now();

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
        blocksMs    = msSince( stageStart );
        stageStart  = Clock::now();
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
            blockTableMs = msSince( stageStart );
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

        stageStart              = Clock::now();
        const std::string bytes = Ser::EncodeTextureBinary( data );
        containerMs             = msSince( stageStart );
        if ( bytes.empty() )
            return Common::MakeError<bool>( "the cooked container refused the shape this bake produced" );

        stageStart         = Clock::now();
        const auto written = Common::Utils::FileSystem::WriteContentToFileAtomic( path, bytes );
        LOG_INFO( "[EnvironmentBake] '{}' encode stages: level table {:.1f} ms, BC6H census {:.1f} ms, block "
                  "encode {:.1f} ms, block table {:.1f} ms, container {:.1f} ms, write {:.1f} ms.",
                  path.string(), tableMs, censusMs, blocksMs, blockTableMs, containerMs, msSince( stageStart ) );
        return written;
    }
    EnvironmentCacheWriter& EnvironmentCacheWriter::Get()
    {
        static EnvironmentCacheWriter writer;
        return writer;
    }

    Common::BoolResultStr EnvironmentCacheWriter::Begin( ImageCube& cube, EnvironmentCacheEntry entry )
    {
        auto* vulkanCube = dynamic_cast<API::Vulkan::VulkanImageCube*>( &cube );
        if ( !vulkanCube )
            return Common::MakeError<bool>( "the environment bake can only read back a Vulkan cube" );
        entry.FaceSize = cube.GetWidth();
        entry.Mips     = cube.GetMipmapLevels();
        auto begun     = vulkanCube->RT_BeginReadAllLevels();
        if ( !begun.IsSuccess() )
            return Common::MakeError<bool>( begun.GetError() );
        m_InFlight.push_back( { std::move( entry ), begun.ExtractValue(), {}, std::chrono::steady_clock::now() } );
        return BOOLSUCCESS;
    }

    void EnvironmentCacheWriter::Pump()
    {
        for ( auto it = m_InFlight.begin(); it != m_InFlight.end(); )
        {
            Write& write = *it;
            if ( !write.Encoded.valid() )
            {
                if ( !write.Readback->IsComplete() )
                {
                    ++it;
                    continue;
                }
                // THE READBACK OBJECT STAYS HERE, the worker gets a raw pointer: it is created and destroyed on
                // the device thread (its destructor frees a command buffer from a pool), and ReadBytes alone
                // may run elsewhere. The entry is not erased until the future below is ready.
                const ImageReadback* readback = write.Readback.get();
                write.Encoded = Common::JobSystem::Get().Async(
                     [readback, entry = write.Entry]() -> Common::BoolResultStr
                     {
                         auto bytes = readback->ReadBytes();
                         if ( !bytes.IsSuccess() )
                             return Common::MakeError<bool>( bytes.GetError() );
                         return EncodeBakedEnvironmentCube( entry, bytes.GetValue() );
                     } );
                ++it;
                continue;
            }
            if ( write.Encoded.wait_for( std::chrono::seconds( 0 ) ) != std::future_status::ready )
            {
                ++it;
                continue;
            }
            Finish( write );
            it = m_InFlight.erase( it );
        }
    }

    void EnvironmentCacheWriter::Drain()
    {
        // Shutdown only: every write in flight is FINISHED rather than dropped, because a dropped one is a
        // cache entry the next run bakes again for 15 s. The readbacks are released here, on the device
        // thread, while there is still a device to release them to.
        for ( Write& write : m_InFlight )
        {
            if ( !write.Encoded.valid() )
            {
                const ImageReadback* readback = write.Readback.get();
                const auto&          entry    = write.Entry;
                write.Encoded = std::async( std::launch::deferred,
                                            [readback, &entry]() -> Common::BoolResultStr
                                            {
                                                while ( !readback->IsComplete() )
                                                    std::this_thread::yield();
                                                auto bytes = readback->ReadBytes();
                                                if ( !bytes.IsSuccess() )
                                                    return Common::MakeError<bool>( bytes.GetError() );
                                                return EncodeBakedEnvironmentCube( entry, bytes.GetValue() );
                                            } );
            }
            write.Encoded.wait();
            Finish( write );
        }
        m_InFlight.clear();
    }

    void EnvironmentCacheWriter::Finish( Write& write )
    {
        const Common::BoolResultStr written = write.Encoded.get();
        const double                ms =
             std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - write.StartedAt ).count();
        if ( !written.IsSuccess() )
        {
            LOG_ERROR( "[EnvironmentBake] '{}' was baked but not cached to '{}' ({:.1f} ms after the bake): {}",
                       write.Entry.SourceKey, write.Entry.Path.string(), ms, written.GetError() );
            return;
        }
        LOG_INFO( "[EnvironmentBake] '{}' cached to '{}' {:.1f} ms after the bake, off the main thread.",
                  write.Entry.SourceKey, write.Entry.Path.string(), ms );
    }
} // namespace Desert::Graphic
