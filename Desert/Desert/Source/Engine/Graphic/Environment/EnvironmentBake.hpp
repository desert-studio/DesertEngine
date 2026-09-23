#pragma once

// THE .HDR ENVIRONMENT, BAKED — why an IBL chain is now a file and not a recipe.
//
// WHAT IT REPLACED, MEASURED ON THIS TREE. An `.hdr` skybox reached the screen through three compute
// passes every time a scene naming it was opened: `PanoramaToCubemap` into a 1024 face, `DiffuseIrradiance`
// into a 32 face and `PrefilterEnvMap` into a 256 face with a nine-level GGX chain. Nothing about that
// depends on anything but the FILE and the sky's authored look, and both of those are on disk — so the
// work was the same work, repeated, at every load. What it cost is in `SkyRules.hpp` beside the sizes.
//
// ── THE BOUNDARY, AND IT IS THE WHOLE DESIGN ────────────────────────────────────────────────────────
//
// THE SKY HAS TWO PATHS AND ONLY ONE OF THEM MAY BE BAKED. `EnvironmentManager::Create` reads a FILE;
// `EnvironmentManager::CreateProcedural` reads the atmosphere's parameters, which carry the sun's
// direction and therefore the time of day. Baking the second would freeze one instant of the day cycle
// into a cache and hand it back for every other instant — a defect with no symptom at the moment it is
// written and no way back once content depends on it. The formulation to keep: the path that gets baked
// is the one whose input is a file, not a time.
//
// WHAT THE BAKE IS A FUNCTION OF, exactly, and it is TWO numbers rather than one:
//
//   * `SourceContentHash` — the `.hdr` file's own signature (`Serialization::SourceSignature`: CRC-32C
//     of its bytes with its length in the high half). An artist who edits the panorama gets a re-bake
//     because the BYTES changed, which is the same question `TextureImporter` asks about a `.png` and
//     for the same reason — git sets mtimes to checkout time, so a timestamp answers "fresh" for ever.
//     IT IS READ OUT OF THE COOKED PANORAMA'S HEADER, NOT COMPUTED FROM THE `.hdr`: the cook recorded
//     exactly this number when it read those bytes, so the runtime needs neither the source file nor a
//     decoder for it — see `FindCookedPanorama` below.
//   * `EncoderHash` — everything else the output depends on: the authored `SkyLook` (rotation, tint,
//     intensity), the three face sizes, the two mip counts, and this file's own version. It is the
//     container's v3 provenance column, and the reason that column stopped being refused: only the
//     caller knows what it asked for, so only the caller can judge the answer. See the note at the
//     check in `TextureBinary.cpp`.
//
// A CACHE WHOSE NAME IS ITS KEY. The file is `Cooked/EnvironmentCache/{key:016x}.tex`, the same shape
// `IconBake` and `FontCache` use, and for the same reason: a bake keyed on content needs no invalidation
// pass and no stale-name bookkeeping, because a changed input simply addresses a different file. The two
// numbers above are still written INTO the container and compared on load — a hash collision must not be
// able to hand back somebody else's sky, and the comparison is one integer against a header this loader
// has already had to read.
//
// IT IS A `.tex`, NOT A NEW FORMAT. The cooked texture container gained faces in v3 for exactly this,
// which means the environment cache inherits everything that was already argued there: per-level codecs,
// the levels stored smallest first, the truncation refusal, and `-text` in `.gitattributes` so a Windows
// checkout cannot translate a byte inside a radiance value.

#include <Common/Core/Core.hpp>
#include <Common/Core/ResultStr.hpp>
#include <Engine/Graphic/Environment/SkyLook.hpp>
#include <Engine/Graphic/Image.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace Desert::Graphic
{
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
    ///   2  the levels are encoded to BC6H on the way out (`kStoredCubeFormat`). A v1 file is still
    ///      LOADED — the load accepts either format and an uncompressed cube is a correct cube — but it
    ///      is no longer FOUND, because the signature it was filed under has moved. That is the right
    ///      shape here and not a waste: the v1 file costs sixteen times its successor in resident
    ///      memory, and going on using it is the defect this version exists to prevent.
    ///   3  the radiance cube carries its whole mip chain and both convolutions read it filtered
    ///      (mipmap-filtered importance sampling in PrefilterEnvMap AND DiffuseIrradiance). A v2 file's
    ///      irradiance and prefilter were integrated from level 0 alone and show the sun as splats.
    inline constexpr uint32_t kEnvironmentBakeVersion = 3;

    /// Everything except the source file that the baked pixels depend on. See the header note.
    [[nodiscard]] uint64_t EnvironmentBakeSignature( const SkyLook& look, BakedEnvironmentCube which,
                                                     uint32_t faceSize, uint32_t mips );

    /// Where this (source, look, cube) lands. Deterministic and content-addressed: the same three
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
    /// from another source, a file from another look, a cube of the wrong shape. The caller's answer to
    /// all of them is the same — bake — but the log has to be able to say which one happened, because
    /// "the cache never hits" and "the cache is never written" look identical from the frame rate.
    [[nodiscard]] Common::ResultStr<std::shared_ptr<ImageCube>>
    LoadBakedEnvironmentCube( const std::filesystem::path& path, std::string_view tag, uint32_t faceSize,
                              uint32_t mips, uint64_t sourceSignature, uint64_t bakeSignature );

    /// Read @p cube back off the device and write it as a cooked container. @p sourceKey is the
    /// `.hdr`'s stable project key, recorded as provenance exactly as a cooked texture records its PNG.
    [[nodiscard]] Common::BoolResultStr WriteBakedEnvironmentCube( const std::filesystem::path& path,
                                                                   ImageCube& cube, const std::string& sourceKey,
                                                                   uint64_t sourceSignature,
                                                                   uint64_t bakeSignature );
} // namespace Desert::Graphic
