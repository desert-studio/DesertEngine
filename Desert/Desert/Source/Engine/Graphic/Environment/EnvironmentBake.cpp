#include "EnvironmentBake.hpp"

#include <Common/Core/Logger.hpp>
#include <Common/Utilities/Crc32c.hpp>
#include <Common/Utilities/FileSystem.hpp>
#include <Common/Core/Constants.hpp>

#include <Engine/Assets/Serialization/TextureBinary.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanImage.hpp>

#include <spdlog/fmt/fmt.h>

#include <array>
#include <cstring>

namespace Desert::Graphic
{
    namespace Ser = Assets::Serialization;

    namespace
    {
        /// The format every one of the three cubes is created at (`ComputeImages::ProccessForImageCube`).
        /// Written down HERE as well as there because the container records it and the load has to refuse
        /// a file whose pixels are a different size from the image it is about to fill.
        constexpr Core::Formats::ImageFormat kCubeFormat = Core::Formats::ImageFormat::RGBA32F;

        /// The bake's own signature is a hash over a byte image of everything it depends on. FNV-1a, the
        /// same primitive `IconBake` keys with — this is an identity, not an integrity check, and the
        /// integrity of the bytes is the container's own problem.
        struct LookImage
        {
            uint32_t Version;
            uint32_t Which;
            float    RotationDegrees;
            float    Intensity;
            float    TintR, TintG, TintB;
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

    uint64_t EnvironmentBakeSignature( const SkyLook& look, const BakedEnvironmentCube which,
                                       const uint32_t faceSize, const uint32_t mips )
    {
        // THE SHAPE GOES IN AS WELL AS THE LOOK. A face size or a mip count changed in `SkyRules.hpp` is
        // a different cube out of the same file and the same sky, and a cache that did not notice would
        // hand a 256-face prefilter to an image created at 512 — which is not a wrong picture, it is a
        // refused upload, on a path where "no environment" is already a legal state and says nothing.
        LookImage image{};
        image.Version         = kEnvironmentBakeVersion;
        image.Which           = static_cast<uint32_t>( which );
        image.RotationDegrees = look.RotationDegrees;
        image.Intensity       = look.Intensity;
        image.TintR           = look.Tint.x;
        image.TintG           = look.Tint.y;
        image.TintB           = look.Tint.z;
        image.FaceSize        = faceSize;
        image.Mips            = mips;
        return Fnv1a( &image, sizeof( image ) );
    }

    std::filesystem::path EnvironmentBakePath( const uint64_t sourceSignature, const uint64_t bakeSignature )
    {
        const uint64_t parts[2] = { sourceSignature, bakeSignature };
        return Common::Constants::Path::COOKED_PATH / "EnvironmentCache" /
               fmt::format( "{:016x}.tex", Fnv1a( parts, sizeof( parts ) ) );
    }

    uint64_t EnvironmentSourceSignature( const std::filesystem::path& hdr )
    {
        const auto bytes = Common::Utils::FileSystem::ReadFileContent( hdr );
        if ( !bytes.IsSuccess() )
            return 0;
        return Ser::SourceSignature( bytes.GetValue().data(), bytes.GetValue().size() );
    }

    Common::ResultStr<std::shared_ptr<ImageCube>>
    LoadBakedEnvironmentCube( const std::filesystem::path& path, const std::string_view tag,
                              const uint32_t faceSize, const uint32_t mips, const uint64_t sourceSignature,
                              const uint64_t bakeSignature )
    {
        const auto raw = Common::Utils::FileSystem::ReadFileContent( path );
        if ( !raw.IsSuccess() )
            return Common::MakeError<std::shared_ptr<ImageCube>>( raw.GetError() );

        auto decoded = Ser::DecodeTextureBinary( raw.GetValue(), path.string() );
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
                 "'{}' was baked with settings {:#018x} and this scene asks for {:#018x} (the sky's look or "
                 "the bake's shape changed).",
                 path.string(), data.EncoderHash, bakeSignature );
        }
        if ( data.Kind != Ser::TextureKind::Cube )
        {
            return Common::MakeFormattedError<std::shared_ptr<ImageCube>>(
                 "'{}' is kind {}, and an environment cube is a cube.", path.string(),
                 static_cast<uint32_t>( data.Kind ) );
        }
        if ( data.Width != faceSize || data.LevelCount() != mips || data.Format != kCubeFormat )
        {
            return Common::MakeFormattedError<std::shared_ptr<ImageCube>>(
                 "'{}' holds a {}-texel face with {} levels in format {}, and this environment wants {} / {} "
                 "/ {}.",
                 path.string(), data.Width, data.LevelCount(), static_cast<uint32_t>( data.Format ), faceSize,
                 mips, static_cast<uint32_t>( kCubeFormat ) );
        }

        // THE TABLE TRAVELS WITH THE PIXELS. `ImageCubeSpecification::Levels` is indexed exactly as the
        // container's is — `level * 6 + face` — so this is a copy of the offsets, not a re-derivation of
        // them. A re-derivation is where the padding would be lost.
        std::vector<Core::Formats::MipLevelSpan> spans;
        spans.reserve( data.Levels.size() );
        for ( const Ser::TextureLevel& level : data.Levels )
            spans.push_back( Core::Formats::MipLevelSpan{ level.ByteOffset, level.ByteSize } );

        const Core::Formats::ImageCubeSpecification spec = { .Tag      = std::string( tag ),
                                                             .FaceSize = faceSize,
                                                             .Format   = kCubeFormat,
                                                             .Mips     = mips,
                                                             .Data     = std::move( data.Pixels ),
                                                             .Properties =
                                                                  Core::Formats::Storage | Core::Formats::Sample,
                                                             .Levels = std::move( spans ) };

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

        std::error_code ec;
        std::filesystem::create_directories( path.parent_path(), ec );

        const std::string bytes = Ser::EncodeTextureBinary( data );
        if ( bytes.empty() )
            return Common::MakeError<bool>( "the cooked container refused the shape this bake produced" );

        return Common::Utils::FileSystem::WriteContentToFileAtomic( path, bytes );
    }
} // namespace Desert::Graphic
