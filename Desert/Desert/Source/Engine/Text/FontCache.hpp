#pragma once

// WHERE a baked font atlas lives on disk and HOW it is read back — one definition, three consumers,
// same seam as Core/ShaderCompiler/ShaderSpirvCache:
//
//   * Runtime::FontService asks here before paying the CPU-bound SDF bake, and stores fresh bakes;
//   * the game packager cooks every shipped .ttf into this exact location, so the census tree
//     { COOKED_PATH, "Cooked" } carries the atlases into Content.dpak;
//   * a packaged game reads them back OUT of the mounted archive: the load is VFS-aware, because a
//     player's install has no loose Cooked/ tree.
//
// Key and path used to be private to FontService.cpp, and the load a raw std::ifstream — so even a
// pak that shipped the atlases had them rebaked at every startup.

#include <Engine/Text/FontBaker.hpp>

#include <cstdint>
#include <filesystem>
#include <vector>

namespace Desert::Text
{
    // Content-addressed cache key: the TTF bytes + EVERY input the bake actually has — the size, the
    // gutter, the atlas width, the distance range — plus the extra glyph set (FNV-1a, same scheme as the
    // SPIR-V shader cache). `extraCodepoints` must be sorted+unique so the same glyph set always yields
    // the same key regardless of request order.
    //
    // The FORMAT VERSION IS NOT IN HERE, and that is the point. Folding it into the key made a version
    // bump rename every atlas instead of rejecting it: the old files stayed on disk forever, a shipped
    // .dpak's atlases became unreachable, and the only symptom was a slower start nobody could attribute.
    // The parameters above are in here because they genuinely change the picture under one name; the
    // version is in the FILE, where TryLoadBakedFont can refuse it out loud.
    uint64_t FontCacheKey( const std::vector<uint8_t>& ttf, float pixelHeight,
                           const std::vector<uint32_t>& extraCodepoints );

    // Cooked/FontCache/<key as 16 hex digits>.dfont under the CURRENT project's cooked tree.
    std::filesystem::path FontCachePath( uint64_t key );

    // The ONE bake the cache key describes: BakeFontMSDF with the engine's bake parameters, every one of
    // which FontCacheKey hashes. FontService and the game packager both bake through here — a bake with
    // different parameters under the same key would be a cache that lies.
    BakedFont BakeFontForCache( const std::vector<uint8_t>& ttf, float pixelHeight,
                                const std::vector<uint32_t>& extraCodepoints );

    // Loose file first (dev override), then the mounted .dpak. false on miss or a corrupt entry — a
    // corrupt cache is simply re-baked, never fatal.
    bool TryLoadBakedFont( const std::filesystem::path& path, BakedFont& out );

    // Whether the atlas actually landed on disk — best-effort for the runtime (a read-only install
    // keeps no cache), load-bearing for the packager: an unwritten cook ships nothing under that key
    // and the player pays the bake. See ShaderSpirvCache::StoreCachedSpirv for the same contract.
    bool StoreBakedFont( const std::filesystem::path& path, const BakedFont& font );
} // namespace Desert::Text
