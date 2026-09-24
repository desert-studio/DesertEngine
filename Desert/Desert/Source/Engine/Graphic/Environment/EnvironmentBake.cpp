#include "EnvironmentBake.hpp"

#include <Common/Content/DerivedDataCache.hpp>
#include <Common/Core/Logger.hpp>
#include <Common/Utilities/Crc32c.hpp>
#include <Common/Utilities/FileSystem.hpp>
#include <Common/Core/Constants.hpp>

#include <Engine/Assets/CookedTexturePath.hpp>
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

    namespace
    {
        /// The format every one of the three cubes is COMPUTED at (`ComputeImages::ProccessForImageCube`).
        /// Written down HERE as well as there because the container records it and the load has to refuse
        /// a file whose pixels are a different size from the image it is about to fill.
        constexpr Core::Formats::ImageFormat kCubeFormat = Core::Formats::ImageFormat::RGBA32F;

        /// AND THE FORMAT IT IS STORED AND RESIDENT AT, which is no longer the same thing.
        ///
        /// A compute pass writes RGBA32F because a storage image must have a format a shader can write,
        /// and no BC format can be one. Nothing writes the cube after the bake, so what is CACHED and
        /// what is resident afterwards need not be that format — and at sixteen bytes a texel it is by
        /// far the most expensive thing this engine keeps per environment. BC6H is one byte a texel.
        ///
        /// WHY BC6H AND NOT BC7 HERE. Radiance is not in [0,1]: the measured peak of this project's own
        /// panorama is above 1 and a real sky's sun is orders of magnitude above it. BC7 would clamp
        /// every value over 1 to white, which is not a quality loss, it is a different image.
        constexpr Core::Formats::ImageFormat kStoredCubeFormat = Core::Formats::ImageFormat::BC6H_UFLOAT;

        /// The bake's own signature is a hash over a byte image of everything it depends on. FNV-1a, the
        /// same primitive `IconBake` keys with — this is an identity, not an integrity check, and the
        /// integrity of the bytes is the container's own problem.
        struct SignatureImage
        {
            uint32_t Version;
            uint32_t Which;
            uint32_t FaceSize;
            uint32_t Mips;
        };

        uint64_t Fnv1a( const void* bytes, const size_t size )
        {
            constexpr uint64_t kOffset = 1469598103934665603ull;
            constexpr uint64_t kPrime  = 1099511628211ull;

            const auto* p = static_cast<const unsigned char*>( bytes );
            uint64_t    h = kOffset;
            for ( size_t i = 0; i < size; ++i )
            {
                h ^= p[i];
                h *= kPrime;
            }
            return h;
        }

    } // namespace

    uint64_t EnvironmentBakeSignature( const BakedEnvironmentCube which, const uint32_t faceSize,
                                       const uint32_t mips )
    {
        // THE SHAPE GOES IN. A face size or a mip count changed in `SkyRules.hpp` is
        // a different cube out of the same file and the same sky, and a cache that did not notice would
        // hand a 256-face prefilter to an image created at 512 — which is not a wrong picture, it is a
        // refused upload, on a path where "no environment" is already a legal state and says nothing.
        // THE LOOK DOES NOT: it is applied where the cubes are sampled (Environment/SkyLook.hpp), so a
        // rotation dragged through twenty values addresses the same three files twenty times.
        SignatureImage image{};
        image.Version  = kEnvironmentBakeVersion;
        image.Which    = static_cast<uint32_t>( which );
        image.FaceSize = faceSize;
        image.Mips     = mips;
        return Fnv1a( &image, sizeof( image ) );
    }

    std::filesystem::path EnvironmentBakePath( const uint64_t sourceSignature, const uint64_t bakeSignature )
    {
        // Payload = the panorama's own signature; setting = the cube's shape (which cube, size, mips).
        constexpr Common::DDC::Deriver kEnvironmentDeriver{ "EnvironmentCache", ".tex",
                                                            { 0x5e0b8d3712f4a6c9ULL, 0xe47a2c9150b3d68fULL } };
        return Common::DDC::PathFor( kEnvironmentDeriver, Common::DDC::MakeKey( kEnvironmentDeriver, sourceSignature,
                                                                                &bakeSignature,
                                                                                sizeof( bakeSignature ) ) );
    }

    Common::ResultStr<CookedPanorama> FindCookedPanorama( const std::filesystem::path& hdr )
    {
        CookedPanorama panorama;
        panorama.Path = Assets::CookedTexturePath( hdr, ".tex" );

        auto prefix =
             Common::Utils::FileSystem::ReadFileContentPrefix( panorama.Path, Ser::kTextureBinaryPrefixBytes );
        if ( !prefix.IsSuccess() )
        {
            return Common::MakeFormattedError<CookedPanorama>(
                 "'{}' has no cooked panorama at '{}' ({}). The source is cooked by the editor's texture "
                 "pass; the runtime reads only the cooked form.",
                 hdr.string(), panorama.Path.string(), prefix.GetError() );
        }

        // THE ONE-READ COMMON CASE, AND THE SECOND READ WHEN IT IS NOT. A metadata block longer than the
        // prefix window (a very long source key) is legal; the header says how long it is.
        const uint64_t needed = Ser::TextureBinaryMetadataBytes( prefix.GetValue() );
        if ( needed > prefix.GetValue().size() )
        {
            prefix = Common::Utils::FileSystem::ReadFileContentPrefix( panorama.Path,
                                                                       static_cast<std::size_t>( needed ) );
            if ( !prefix.IsSuccess() )
                return Common::MakeError<CookedPanorama>( prefix.GetError() );
        }

        const auto header = Ser::DecodeTextureHeader( prefix.GetValue(), panorama.Path.string() );
        if ( !header.IsSuccess() )
            return Common::MakeError<CookedPanorama>( header.GetError() );

        // A ZERO SIGNATURE IS NOT A SIGNATURE. It is what the cache key would be built from, and a key
        // built from "unknown" would let every panorama that lacks one share a single cache entry.
        if ( header.GetValue().SourceContentHash == 0 )
        {
            return Common::MakeFormattedError<CookedPanorama>(
                 "the cooked panorama '{}' records no source signature, so its bake cannot be keyed.",
                 panorama.Path.string() );
        }
        panorama.SourceSignature = header.GetValue().SourceContentHash;
        return Common::MakeSuccess( std::move( panorama ) );
    }

    Common::ResultStr<std::shared_ptr<ImageCube>>
    LoadBakedEnvironmentCube( const std::filesystem::path& path, const std::string_view tag,
                              const uint32_t faceSize, const uint32_t mips, const uint64_t sourceSignature,
                              const uint64_t bakeSignature )
    {
        // A MISS IS THE NORMAL FIRST RUN, NOT AN ERROR: the caller logs one line naming why it bakes.
        const auto raw = Common::Utils::FileSystem::ReadFileContentIfExists( path );
        if ( !raw.IsSuccess() )
            return Common::MakeError<std::shared_ptr<ImageCube>>( raw.GetError() );
        if ( !raw.GetValue().has_value() )
            return Common::MakeFormattedError<std::shared_ptr<ImageCube>>( "not in the cache yet: {}",
                                                                           path.string() );

        auto decoded = Ser::DecodeTextureBinary( raw.GetValue().value(), path.string() );
        if ( !decoded.IsSuccess() )
            return Common::MakeError<std::shared_ptr<ImageCube>>( decoded.GetError() );

        auto data = decoded.ExtractValue();

        // ── FOUR QUESTIONS, ASKED SEPARATELY SO THE ANSWER NAMES ITSELF ──────────────────────────
        //
        // The path is content-addressed, so a file that is here at all is PROBABLY the right one. The
        // checks below are what makes "probably" into "yes": the two signatures rule out a collision
        // and a hand-copied file, and the shape rules out a cache written before somebody changed a
        // face size in `SkyRules.hpp` and left the version alone.
        if ( data.SourceContentHash != sourceSignature )
        {
            return Common::MakeFormattedError<std::shared_ptr<ImageCube>>(
                 "'{}' was baked from a panorama whose signature is {:#018x} and this scene's is {:#018x}.",
                 path.string(), data.SourceContentHash, sourceSignature );
        }
        if ( data.EncoderHash != bakeSignature )
        {
            return Common::MakeFormattedError<std::shared_ptr<ImageCube>>(
                 "'{}' was baked with settings {:#018x} and this scene asks for {:#018x} (the bake's version or "
                 "shape changed).",
                 path.string(), data.EncoderHash, bakeSignature );
        }
        if ( data.Kind != Ser::TextureKind::Cube )
        {
            return Common::MakeFormattedError<std::shared_ptr<ImageCube>>(
                 "'{}' is kind {}, and an environment cube is a cube.", path.string(),
                 static_cast<uint32_t>( data.Kind ) );
        }
        if ( data.Width != faceSize || data.LevelCount() != mips ||
             ( data.Format != kStoredCubeFormat && data.Format != kCubeFormat ) )
        {
            return Common::MakeFormattedError<std::shared_ptr<ImageCube>>(
                 "'{}' holds a {}-texel face with {} levels in format {}, and this environment wants {} / {} "
                 "/ {}.",
                 path.string(), data.Width, data.LevelCount(), static_cast<uint32_t>( data.Format ), faceSize,
                 mips, static_cast<uint32_t>( kStoredCubeFormat ) );
        }

        // THE TABLE TRAVELS WITH THE PIXELS. `ImageCubeSpecification::Levels` is indexed exactly as the
        // container's is — `level * 6 + face` — so this is a copy of the offsets, not a re-derivation of
        // them. A re-derivation is where the padding would be lost.
        std::vector<Core::Formats::MipLevelSpan> spans;
        spans.reserve( data.Levels.size() );
        for ( const Ser::TextureLevel& level : data.Levels )
            spans.push_back( Core::Formats::MipLevelSpan{ level.ByteOffset, level.ByteSize } );

        // SAMPLE ONLY WHEN THE PIXELS ARE BLOCKS, AND STORAGE OTHERWISE — not a tidy-up, a hardware
        // rule. `VK_IMAGE_USAGE_STORAGE_BIT` requires a format a shader can write, and no BC format is
        // one (`storageImage` is absent from every BC entry of `vkGetPhysicalDeviceFormatProperties`).
        // A cube read off the cache is never written again — the three compute passes did not run, which
        // is the whole point of the cache — so the bit it would be refused for is also the bit it has no
        // use for. The uncompressed branch keeps its old properties exactly, so a file cooked before
        // this change loads exactly as it did.
        const Core::Formats::ImageProperties properties =
             Core::Formats::IsBlockCompressed( data.Format )
                  ? Core::Formats::Sample
                  : static_cast<Core::Formats::ImageProperties>( Core::Formats::Storage | Core::Formats::Sample );

        LOG_INFO( "[EnvironmentBake] '{}' loaded: {}-texel face, {} level(s), format {}, {:.2f} MiB resident.",
                  path.string(), faceSize, mips, static_cast<uint32_t>( data.Format ),
                  static_cast<double>( Core::Formats::CalculateCubeImageSize( faceSize, mips, data.Format ) ) /
                       ( 1024.0 * 1024.0 ) );

        const Core::Formats::ImageCubeSpecification spec = { .Tag        = std::string( tag ),
                                                             .FaceSize   = faceSize,
                                                             .Format     = data.Format,
                                                             .Mips       = mips,
                                                             .Data       = std::move( data.Pixels ),
                                                             .Properties = properties,
                                                             .Levels     = std::move( spans ) };

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
        data.Format            = kCubeFormat;

        // THE READBACK'S LAYOUT AND THE PLACER'S INPUT ARE THE SAME LAYOUT, and that is asserted rather
        // than assumed: `RT_ReadAllLevels` documents "tightly packed in table order" and
        // `TightlyPackedChainBytes` is the size that phrase means. A disagreement here is the middle
        // link dropping a property, so it is a refusal with both numbers and not a silent truncation.
        const uint64_t expected =
             Ser::TightlyPackedChainBytes( faceSize, faceSize, mips, Ser::kTextureCubeLayerCount, kCubeFormat );
        if ( readback.GetValue().size() != expected )
        {
            return Common::MakeFormattedError<bool>(
                 "the cube read back as {} bytes and {} levels x {} faces of a {}-texel face is {}.",
                 readback.GetValue().size(), mips, Ser::kTextureCubeLayerCount, faceSize, expected );
        }

        auto table = Ser::BuildLevelTable( faceSize, faceSize, mips, Ser::kTextureCubeLayerCount, kCubeFormat,
                                           readback.GetValue(), data.Pixels );
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
        auto blocks = Ser::BlockCompressChain( faceSize, faceSize, mips, Ser::kTextureCubeLayerCount, kCubeFormat,
                                               kStoredCubeFormat, data.Levels, data.Pixels );
        if ( !blocks.IsSuccess() )
        {
            LOG_WARN( "[EnvironmentBake] '{}' is cached uncompressed: {}", path.string(), blocks.GetError() );
        }
        else
        {
            std::vector<unsigned char> blockPixels;
            auto blockTable = Ser::BuildLevelTable( faceSize, faceSize, mips, Ser::kTextureCubeLayerCount,
                                                    kStoredCubeFormat, blocks.GetValue(), blockPixels );
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
                data.Format = kStoredCubeFormat;
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
