#include <Engine/Assets/Serialization/EnvironmentStaging.hpp>

#include <Common/Content/DerivedDataCache.hpp>
#include <Common/Core/Logger.hpp>
#include <Common/Utilities/FileSystem.hpp>

#include <Engine/Assets/TextureSourceAsset.hpp>
#include <Engine/Assets/Serialization/TextureBinary.hpp>

#include <spdlog/fmt/fmt.h>

#include <chrono>
#include <vector>

namespace Desert::Assets
{
    namespace Ser = Serialization;

    namespace
    {
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
        constexpr Common::DDC::Deriver kEnvironmentDeriver{
             "EnvironmentCache", ".tex", { 0x5e0b8d3712f4a6c9ULL, 0xe47a2c9150b3d68fULL } };
        return Common::DDC::PathFor( kEnvironmentDeriver,
                                     Common::DDC::MakeKey( kEnvironmentDeriver, sourceSignature, &bakeSignature,
                                                           sizeof( bakeSignature ) ) );
    }

    Common::ResultStr<CookedPanorama> FindCookedPanorama( const std::filesystem::path& hdr )
    {
        // THE ASSET IS WHERE THE PANORAMA IS FOUND (AF3c). Its key (IMPT, or PAYL cooked) has the source hash,
        // which is both the bake's key and the input of the panorama's own DDC entry; one prefix read.
        const auto key = ReadTextureAssetKey( hdr );
        if ( !key.IsSuccess() )
            return Common::MakeFormattedError<CookedPanorama>( "the panorama asset cannot be read: {}",
                                                               key.GetError() );
        // A ZERO SIGNATURE IS NOT A SIGNATURE. It is what the cache key would be built from, and a key
        // built from "unknown" would let every panorama that lacks one share a single cache entry.
        if ( key.GetValue().SourceHash == 0 )
            return Common::MakeFormattedError<CookedPanorama>(
                 "the panorama asset '{}' records no source hash, so its bake cannot be keyed.", hdr.string() );
        CookedPanorama panorama;
        panorama.Path            = hdr;
        panorama.SourceSignature = key.GetValue().SourceHash;
        return Common::MakeSuccess( std::move( panorama ) );
    }

    Common::ResultStr<Core::Formats::ImageCubeSpecification>
    ReadBakedEnvironmentCube( const std::filesystem::path& path, const std::string_view tag,
                              const uint32_t faceSize, const uint32_t mips, const uint64_t sourceSignature,
                              const uint64_t bakeSignature )
    {
        using Spec = Core::Formats::ImageCubeSpecification;
        // A MISS IS THE NORMAL FIRST RUN, NOT AN ERROR: the caller logs one line naming why it bakes.
        const auto raw = Common::Utils::FileSystem::ReadFileContentIfExists( path );
        if ( !raw.IsSuccess() )
            return Common::MakeError<Spec>( raw.GetError() );
        const auto& bytes = raw.GetValue();
        if ( !bytes.has_value() )
            return Common::MakeFormattedError<Spec>( "not in the cache yet: {}", path.string() );

        auto decoded = Ser::DecodeTextureBinary( *bytes, path.string() );
        if ( !decoded.IsSuccess() )
            return Common::MakeError<Spec>( decoded.GetError() );

        auto data = decoded.ExtractValue();

        // ── FOUR QUESTIONS, ASKED SEPARATELY SO THE ANSWER NAMES ITSELF ──────────────────────────
        //
        // The path is content-addressed, so a file that is here at all is PROBABLY the right one. The
        // checks below are what makes "probably" into "yes": the two signatures rule out a collision
        // and a hand-copied file, and the shape rules out a cache written before somebody changed a
        // face size in `SkyRules.hpp` and left the version alone.
        if ( data.SourceContentHash != sourceSignature )
        {
            return Common::MakeFormattedError<Spec>(
                 "'{}' was baked from a panorama whose signature is {:#018x} and this scene's is {:#018x}.",
                 path.string(), data.SourceContentHash, sourceSignature );
        }
        if ( data.EncoderHash != bakeSignature )
        {
            return Common::MakeFormattedError<Spec>(
                 "'{}' was baked with settings {:#018x} and this scene asks for {:#018x} (the bake's version or "
                 "shape changed).",
                 path.string(), data.EncoderHash, bakeSignature );
        }
        if ( data.Kind != Ser::TextureKind::Cube )
        {
            return Common::MakeFormattedError<Spec>( "'{}' is kind {}, and an environment cube is a cube.",
                                                     path.string(), static_cast<uint32_t>( data.Kind ) );
        }
        if ( data.Width != faceSize || data.LevelCount() != mips ||
             ( data.Format != kEnvironmentStoredFormat && data.Format != kEnvironmentComputeFormat ) )
        {
            return Common::MakeFormattedError<Spec>(
                 "'{}' holds a {}-texel face with {} levels in format {}, and this environment wants {} / {} "
                 "/ {}.",
                 path.string(), data.Width, data.LevelCount(), static_cast<uint32_t>( data.Format ), faceSize,
                 mips, static_cast<uint32_t>( kEnvironmentStoredFormat ) );
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

        Core::Formats::ImageCubeSpecification spec = { .Tag        = std::string( tag ),
                                                       .FaceSize   = faceSize,
                                                       .Format     = data.Format,
                                                       .Mips       = mips,
                                                       .Data       = std::move( data.Pixels ),
                                                       .Properties = properties,
                                                       .Levels     = std::move( spans ) };

        return Common::MakeSuccess( std::move( spec ) );
    }

    StagedEnvironment StageEnvironment( const std::filesystem::path& skybox )
    {
        const auto        startedAt = std::chrono::steady_clock::now();
        StagedEnvironment staged;
        staged.Skybox = skybox;

        // THE ASSET'S HEADER DECIDES WHAT THIS SKYBOX IS, NOT ITS FILE NAME: `FindCookedPanorama` reads
        // the container's own kind and key and names the reason when it refuses, so it is the only gate.
        auto cooked = FindCookedPanorama( skybox );
        if ( !cooked )
        {
            staged.Error = cooked.GetError();
            return staged;
        }
        staged.Panorama       = cooked.ExtractValue();
        const uint64_t source = staged.Panorama.SourceSignature;

        staged.RadianceBake =
             EnvironmentBakeSignature( BakedEnvironmentCube::Radiance, kSkyEnvCubeFaceSize, kSkyEnvRadianceMips );
        staged.IrradianceBake =
             EnvironmentBakeSignature( BakedEnvironmentCube::Irradiance, kSkyEnvIrradianceFaceSize, 1u );
        staged.PrefilterBake  = EnvironmentBakeSignature( BakedEnvironmentCube::Prefiltered,
                                                          kSkyEnvPrefilterFaceSize, kSkyEnvPrefilterMips );
        staged.RadiancePath   = EnvironmentBakePath( source, staged.RadianceBake );
        staged.IrradiancePath = EnvironmentBakePath( source, staged.IrradianceBake );
        staged.PrefilterPath  = EnvironmentBakePath( source, staged.PrefilterBake );

        auto radiance   = ReadBakedEnvironmentCube( staged.RadiancePath, "EnvRadiance", kSkyEnvCubeFaceSize,
                                                    kSkyEnvRadianceMips, source, staged.RadianceBake );
        auto irradiance = ReadBakedEnvironmentCube( staged.IrradiancePath, "EnvDiffuseIrradiance",
                                                    kSkyEnvIrradianceFaceSize, 1u, source, staged.IrradianceBake );
        auto prefilter =
             ReadBakedEnvironmentCube( staged.PrefilterPath, "EnvPrefiltered", kSkyEnvPrefilterFaceSize,
                                       kSkyEnvPrefilterMips, source, staged.PrefilterBake );
        if ( radiance.IsSuccess() && irradiance.IsSuccess() && prefilter.IsSuccess() )
        {
            staged.Cached.emplace( std::array<Core::Formats::ImageCubeSpecification, 3>{
                 radiance.ExtractValue(), irradiance.ExtractValue(), prefilter.ExtractValue() } );
        }
        else
        {
            staged.MissReason =
                 ( radiance.IsSuccess() ? std::string( "radiance ready" ) : radiance.GetError() ) + " / " +
                 ( irradiance.IsSuccess() ? std::string( "irradiance ready" ) : irradiance.GetError() ) + " / " +
                 ( prefilter.IsSuccess() ? std::string( "prefilter ready" ) : prefilter.GetError() );
        }
        staged.StageMs =
             std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - startedAt ).count();
        return staged;
    }
} // namespace Desert::Assets
