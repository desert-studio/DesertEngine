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
#include <chrono>
#include <future>
#include <list>
#include <vector>
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

    /// What one cached cube is: where it goes and the provenance it records. @p SourceKey is the `.hdr`'s
    /// stable project key, recorded exactly as a cooked texture records its PNG.
    struct EnvironmentCacheEntry
    {
        std::filesystem::path Path;
        std::string           SourceKey;
        uint64_t              SourceSignature = 0;
        uint64_t              BakeSignature   = 0;
        uint32_t              FaceSize        = 0;
        uint32_t              Mips            = 0;
    };

    /// The CPU half of the write: census, BC6H encode, container, atomic file write. Touches no device, so
    /// it runs on a worker. @p levels is the cube's chain in its own format, tightly packed in table order.
    [[nodiscard]] Common::BoolResultStr EncodeBakedEnvironmentCube( const EnvironmentCacheEntry& entry,
                                                                    const std::vector<uint8_t>&  levels );

    /// THE WRITE, OFF THE MAIN THREAD (AL1-3). Measured on SKY_HdrOrientation: 14 668 ms of the cold open
    /// were the blocking readback plus the BC6H encode plus the file write, all on the main thread, for a
    /// file that only the NEXT run reads. Now the readback is submitted without a wait, the fence is polled
    /// once a tick by `Pump`, and the encode and write run on a JobSystem worker; the environment is in
    /// use from the GPU cubes the whole time. Main-thread only, except the job it starts.
    class EnvironmentCacheWriter
    {
    public:
        static EnvironmentCacheWriter& Get();

        /// Submits the readback of @p cube; the bytes become @p entry's file some ticks later.
        [[nodiscard]] Common::BoolResultStr Begin( ImageCube& cube, EnvironmentCacheEntry entry );
        /// Once a tick, beside `AsyncAssetLoader::Pump`: starts the encode of a landed readback, retires a
        /// finished write with one line naming the file and its latency.
        void Pump();
        /// Host teardown, while the device is alive: finishes every write in flight and releases the readbacks.
        void Drain();
        [[nodiscard]] size_t InFlight() const { return m_InFlight.size(); }

    private:
        struct Write
        {
            EnvironmentCacheEntry                    Entry;
            std::shared_ptr<ImageReadback>           Readback;
            std::future<Common::BoolResultStr>       Encoded;
            std::chrono::steady_clock::time_point    StartedAt;
        };
        static void Finish( Write& write );

        std::list<Write> m_InFlight;
    };
} // namespace Desert::Graphic
