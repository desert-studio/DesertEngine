#pragma once

// THE CPU HALF OF THE .HDR ENVIRONMENT CACHE (AL1-3). The design note — what the bake is a function of,
// why the cache name is its key, why it is a `.tex` — is at the top of `Graphic/Environment/EnvironmentBake.hpp`,
// which keeps the two device calls (upload, readback+write). Everything here is file reads and decodes:
// `SkyboxAsset::LoadFromFile` calls `StageEnvironment` on an `AsyncAssetLoader` worker, and a suite that
// compiles the asset layer without the renderer links it.

#include <Common/Core/ResultStr.hpp>
#include <Engine/Core/Formats/ImageFormat.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace Desert::Assets
{
    // The IBL cube chain that every baked environment produces. These are the sizes SceneEnvironment
    // actually asks for; they live here so the cost report and the bake cannot disagree. Every size names
    // a FACE (ImageCubeSpecification::FaceSize) — no call site multiplies by the 4x3 cross unwrap.
    inline constexpr uint32_t kSkyEnvCubeFaceSize       = 1024;
    inline constexpr uint32_t kSkyEnvIrradianceFaceSize = 32;
    // The radiance cube is the SHARP environment: the skybox pass draws its mip 0 and the prefilter
    // convolves it. IT CARRIES ITS WHOLE CHAIN, because both convolutions read it (or the panorama) by
    // mipmap-filtered importance sampling, and a filtered sample with no lower level to read is a point
    // sample. Measured 2026-09-23 on SKY_HDR_PolyHaven (rural_asphalt_road_2k.hdr, sun peak 131072;
    // camera 0,220,-900 look 0,-0.08,1; 90 frames; repeat floor 0 px): with ONE level the satin sphere
    // reflected the sun as a spray of square splats and the matte sphere's high-pass luminance std was
    // 2.71; with the chain the splats are gone and it is 1.06. Cost: the radiance cube is 96 -> 128 MiB
    // of RGBA32F on the bake that computes it, 6.0 -> 8.0 MiB of BC6H on every load that reads the
    // cache, and on the procedural path nothing resident (that path frees its radiance cube after the
    // prefilter). The bake got FASTER, as the 2026-09-06 note predicted: .hdr convolution 1876 ->
    // 1325..1635 ms, procedural rebake 1024x512 sky-only ~440 -> ~395 ms, 512x256 ~360 -> ~318 ms.
    // The procedural frame (Clouds_Protocol + the same three spheres) moved by at most 2/255 on the
    // matte and satin spheres and 22/255 at one chrome pixel. (The refusal this replaces rested on
    // "the only live producer is the procedural bake"; an .hdr with a real sun made that false.)
    inline constexpr uint32_t kSkyEnvRadianceMips = Core::Formats::MipChainLength( kSkyEnvCubeFaceSize );
    // The prefiltered specular face. 256 is the MEASURED choice, re-measured 2026-09-23 on real content
    // (same protocol as above, radiance chain on): 1024 moved SKY_HDR_PolyHaven by at most 64/255 --
    // at the rim of the sun's highlight on the chrome sphere, mean 0.07/255 over the frame, no
    // structure visible side by side -- and the procedural sphere scene by at most 31/255, while
    // costing 8 -> 128 MiB of RGBA32F (0.5 -> 8 MiB cached), 1.5x the procedural rebake (~318 ->
    // ~487 ms at 512x256) and ~2x the .hdr cache write. The face only matters where a mirror covers
    // more screen per degree than 256/90 texels do (~2.8 per degree); revisit for a close-up mirror,
    // not for a brighter sky. (The first measurement, 2026-09-06 on the synthetic content, found 13/255.)
    inline constexpr uint32_t kSkyEnvPrefilterFaceSize = 256;
    // Derived from the face, never authored: a hand-typed pair is how 11 mips got requested on a 256
    // face — an invalid vkCreateImage (VUID-...-00958) away from VK_ERROR_DEVICE_LOST.
    inline constexpr uint32_t kSkyEnvPrefilterMips = Core::Formats::MipChainLength( kSkyEnvPrefilterFaceSize );
    static_assert( kSkyEnvRadianceMips <= Core::Formats::MipChainLength( kSkyEnvCubeFaceSize ),
                   "radiance mip count exceeds what its own face supports" );
    /// The format every one of the three cubes is COMPUTED at (`ComputeImages::ProccessForImageCube`).
    /// Written down HERE as well as there because the container records it and the load has to refuse
    /// a file whose pixels are a different size from the image it is about to fill.
    inline constexpr Core::Formats::ImageFormat kEnvironmentComputeFormat = Core::Formats::ImageFormat::RGBA32F;

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
    inline constexpr Core::Formats::ImageFormat kEnvironmentStoredFormat = Core::Formats::ImageFormat::BC6H_UFLOAT;

    /// Which of the three cubes a cached file holds. It is part of the key, so the three cannot collide,
    /// and part of the header check, so a file that somehow arrived under the wrong name is refused
    /// rather than bound.
    enum class BakedEnvironmentCube : uint32_t
    {
        Radiance    = 0,
        Irradiance  = 1,
        Prefiltered = 2,
    };

    /// Bumped when anything about the BAKE changes that the numbers below cannot see — a shader edit, a
    /// different convolution, a different STORAGE FORMAT. It is part of `EncoderHash`, so bumping it
    /// re-bakes every environment without anybody deleting a directory.
    ///
    ///   1  RGBA32F levels, six faces, written by T3.1's container.
    ///   2  the levels are encoded to BC6H on the way out (`kEnvironmentStoredFormat`). A v1 file is still
    ///      LOADED — the load accepts either format and an uncompressed cube is a correct cube — but it
    ///      is no longer FOUND, because the signature it was filed under has moved. That is the right
    ///      shape here and not a waste: the v1 file costs sixteen times its successor in resident
    ///      memory, and going on using it is the defect this version exists to prevent.
    ///   3  the radiance cube carries its whole mip chain and both convolutions read it filtered
    ///      (mipmap-filtered importance sampling in PrefilterEnvMap AND DiffuseIrradiance). A v2 file's
    ///      irradiance and prefilter were integrated from level 0 alone and show the sun as splats.
    ///   4  the cubes no longer carry the authored `SkyLook`: they are the panorama as it is, at unit
    ///      gain, and rotation/intensity/tint are applied where the cubes are SAMPLED. The look left the
    ///      signature with the same change, so one `.hdr` is one set of three files whatever its sliders
    ///      say; the bump makes that a stated miss rather than a hash that merely happened to move.
    inline constexpr uint32_t kEnvironmentBakeVersion = 4;

    /// Everything except the source file that the baked pixels depend on. See the header note.
    [[nodiscard]] uint64_t EnvironmentBakeSignature( BakedEnvironmentCube which, uint32_t faceSize,
                                                     uint32_t mips );

    /// Where this (source, cube) lands. Deterministic and content-addressed: the same three
    /// inputs name the same file on every machine, which is what lets a cooked environment be committed.
    [[nodiscard]] std::filesystem::path EnvironmentBakePath( uint64_t sourceSignature, uint64_t bakeSignature );

    /// An `.hdr` skybox as the RUNTIME knows it: the cooked panorama `TextureImporter` wrote for it, and
    /// the source signature that cook recorded.
    struct CookedPanorama
    {
        std::filesystem::path Path;
        uint64_t              SourceSignature = 0;
    };

    /// THE PANORAMA IS A COOKED TEXTURE, AND THE RUNTIME DOES NOT DECODE ITS SOURCE. This used to be two
    /// things: `EnvironmentSourceSignature` hashed the `.hdr`'s bytes, and `EnvironmentManager::Create`
    /// decoded the same file through stb into a `Texture2D` — BEFORE asking the cache, so a hit paid the
    /// decode for nothing and a miss kept an image decoder in the draw layer. Both answers are in the
    /// cooked panorama's header, which is one prefix read.
    ///
    /// A refusal is a sentence naming the path it looked at: "no cooked panorama" means the editor's
    /// texture pass has not run over this source, and that is the remedy the log has to be able to say.
    [[nodiscard]] Common::ResultStr<CookedPanorama> FindCookedPanorama( const std::filesystem::path& hdr );

    /// A baked cube off the disk, or a REFUSAL NAMING WHY. Every reason is a sentence: no file, a file
    /// from another source, a file from another bake version, a cube of the wrong shape. The caller's answer to
    /// all of them is the same — bake — but the log has to be able to say which one happened, because
    /// "the cache never hits" and "the cache is never written" look identical from the frame rate.
    ///
    /// SPLIT IN TWO SO THE DISK HALF LEAVES THE FRAME (AL1-3). `Read` is the file read, the container decode
    /// and every refusal — no device call, so it runs on an `AsyncAssetLoader` worker; `Create` is the one
    /// GPU upload and runs on the thread that owns the device. A cache hit is the two in sequence and
    /// nothing else: no panorama read, no compute pass.
    [[nodiscard]] Common::ResultStr<Core::Formats::ImageCubeSpecification>
    ReadBakedEnvironmentCube( const std::filesystem::path& path, std::string_view tag, uint32_t faceSize,
                              uint32_t mips, uint64_t sourceSignature, uint64_t bakeSignature );

    /// EVERYTHING ABOUT AN `.hdr` ENVIRONMENT THAT CAN BE KNOWN WITHOUT THE DEVICE (AL1-3). Built on an
    /// `AsyncAssetLoader` worker by `SkyboxAsset::LoadFromFile`, consumed on the main thread by
    /// `EnvironmentManager::Create`: a cache hit arrives with its three cubes decoded and only the upload
    /// is left; a miss arrives with the paths and signatures the bake will write under, and the reason.
    struct StagedEnvironment
    {
        std::filesystem::path Skybox;
        /// Non-empty: there is no cooked panorama, so there is no environment at all — the sentence says why.
        std::string           Error;
        CookedPanorama        Panorama;
        uint64_t              RadianceBake = 0, IrradianceBake = 0, PrefilterBake = 0;
        std::filesystem::path RadiancePath, IrradiancePath, PrefilterPath;
        /// Radiance, irradiance, prefiltered — present only when ALL THREE hit.
        std::optional<std::array<Core::Formats::ImageCubeSpecification, 3>> Cached;
        std::string                                                         MissReason;
        double                                                              StageMs = 0.0;
    };

    /// Worker-safe: file reads and decodes only, no device call.
    [[nodiscard]] StagedEnvironment StageEnvironment( const std::filesystem::path& skybox );
} // namespace Desert::Assets
