#pragma once

// THE .HDR ENVIRONMENT, BAKED — why an IBL chain is now a file and not a recipe.
//
// WHAT IT REPLACED, MEASURED ON THIS TREE. An `.hdr` skybox reached the screen through three compute
// passes every time a scene naming it was opened: `PanoramaToCubemap` into a 1024 face, `DiffuseIrradiance`
// into a 32 face and `PrefilterEnvMap` into a 256 face with a nine-level GGX chain. Nothing about that
// depends on anything but the FILE, which is on disk — so the
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
//   * `EncoderHash` — everything else the output depends on: the three face sizes, the two mip counts, and this
//   file's own version. It is the
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
#include <Engine/Assets/Serialization/EnvironmentStaging.hpp>
#include <Engine/Graphic/Image.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace Desert::Graphic
{
    // The CPU half of the cache (keys, paths, the read, the staging) is `Assets/Serialization/EnvironmentStaging`:
    // it runs on an `AsyncAssetLoader` worker, and the asset layer does not link the device (AL1-3).

    [[nodiscard]] Common::ResultStr<std::shared_ptr<ImageCube>>
    CreateBakedEnvironmentCube( Core::Formats::ImageCubeSpecification spec, const std::filesystem::path& path );

    /// Read @p cube back off the device and write it as a cooked container. @p sourceKey is the
    /// `.hdr`'s stable project key, recorded as provenance exactly as a cooked texture records its PNG.
    [[nodiscard]] Common::BoolResultStr WriteBakedEnvironmentCube( const std::filesystem::path& path,
                                                                   ImageCube& cube, const std::string& sourceKey,
                                                                   uint64_t sourceSignature,
                                                                   uint64_t bakeSignature );
} // namespace Desert::Graphic
