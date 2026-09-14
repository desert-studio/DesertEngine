#pragma once

// The CPU half of an icon import — SVG bytes -> per-colour-run SDF layer bitmaps — plus its on-disk
// cache. Split out of Runtime::IconService for the same reason Text/FontBaker + Text/FontCache exist
// beside FontService: the bake is pure CPU (parse + rasterize, no GPU types), it is the expensive
// part of the import, and it used to be redone from the .svg at EVERY startup because nothing cached
// it — so the game packager had nothing it could ship, either.
//
// One definition, three consumers, same seam as ShaderSpirvCache/FontCache:
//
//   * Runtime::IconService asks the cache before parsing/rasterizing, and stores fresh bakes;
//   * the game packager cooks every shipped .svg into this exact location, so the census tree
//     { COOKED_PATH, "Cooked" } carries the bakes into Content.dpak;
//   * a packaged game reads them back OUT of the mounted archive (VFS-aware load).
//
// The GPU half — packing layers into the atlas page, uploading, re-addressing UVs — stays in
// IconService: an atlas is a property of the running set of icons, not of one file.

#include <cstdint>
#include <filesystem>
#include <vector>

namespace Desert::Vector
{
    // Bake resolution of the inner box, plus the gutter the distance field spreads over. 64 is the
    // same ballpark as the font atlas (48) — an icon is drawn far larger than a glyph, and the SDF
    // reconstructs the edge analytically, so this is about gradient quality, not pixel resolution.
    inline constexpr uint32_t kIconSize    = 64;
    inline constexpr int      kIconPadding = 6;
    inline constexpr uint32_t kIconCellDim = kIconSize + 2u * static_cast<uint32_t>( kIconPadding );

    // Bumped when anything about HOW an icon is baked changes (kIconSize/kIconPadding, the distance
    // encoding, the SVG parser's curve tolerance, the colour-run collapse) — every cached bake becomes a
    // miss. 2: the field now spans Core::Formats::kSdfAtlasDistanceRangeTexels texels rather than
    // kIconPadding, so that it and the glyph atlas mean the same thing to the one shader that reads both.
    //
    // NOTE, and it is the same defect the font cache was just cured of: this version is folded into
    // IconCacheKey, so bumping it makes every existing .dicon UNREACHABLE rather than refused. Old files
    // stay on disk forever and a packaged game's icon bakes go quietly unused. Fixing it means moving the
    // version out of the key and into the file, as Engine/Text/FontBaker now does; that is a change to the
    // icon cache, which this task does not own.
    inline constexpr uint32_t kBakedIconCacheVersion = 2;

    // One SDF bitmap per COLOUR RUN of the source SVG (consecutive shapes sharing a fill collapse
    // into one layer; document order preserved, so overlapping paths still stack back-to-front).
    struct BakedIconLayer
    {
        std::vector<uint8_t> Sdf; // kIconCellDim * kIconCellDim texels
        uint32_t             RGBA = 0xFFFFFFFFu;
    };

    struct BakedIcon
    {
        float                       Aspect = 1.0f; // viewBox width / height
        std::vector<BakedIconLayer> Layers;

        bool Valid() const
        {
            return !Layers.empty();
        }
    };

    // Parse + rasterize, no cache involved. Invalid result when the SVG has no shapes this importer
    // understands (the caller owns the logging — it knows the path).
    BakedIcon BakeIconSdf( const uint8_t* svg, size_t size );

    // Content-addressed cache key: version tag + the SVG bytes (FNV-1a, same scheme as the SPIR-V
    // and font caches). The bake has no other inputs — its parameters live in the version above.
    uint64_t IconCacheKey( const std::vector<uint8_t>& svg );

    // Cooked/IconCache/<key as 16 hex digits>.dicon under the CURRENT project's cooked tree.
    std::filesystem::path IconCachePath( uint64_t key );

    // Binary (de)serialization for the cache file. Deserialize returns an INVALID icon on any
    // truncation / bad magic / version mismatch — a corrupt cache entry is simply re-baked.
    std::vector<uint8_t> SerializeBakedIcon( const BakedIcon& icon );
    bool                 DeserializeBakedIcon( const uint8_t* data, size_t size, BakedIcon& out );

    // Loose file first (dev override), then the mounted .dpak. false on miss/corruption.
    bool TryLoadBakedIcon( const std::filesystem::path& path, BakedIcon& out );

    // Whether the bake actually landed on disk — best-effort for the runtime (a read-only install
    // keeps no cache), load-bearing for the packager: an unwritten cook ships nothing under that key
    // and the player pays the bake. See ShaderSpirvCache::StoreCachedSpirv for the same contract.
    bool StoreBakedIcon( const std::filesystem::path& path, const BakedIcon& icon );
} // namespace Desert::Vector
