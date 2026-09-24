#pragma once

// WHERE a compiled SPIR-V artifact lives and HOW it is read back — one definition, three consumers:
//
//   * Core::ShaderCompiler stores every fresh compile here and asks here before compiling;
//   * the game packager (Editor/Packaging) copies every shipped shader's DDC entry into the
//     platform cook directory under the same relative layout, and that tree is packed under the
//     archive key "Cooked", so Content.dpak carries the artifacts;
//   * a packaged game reads them back OUT of that archive: the load is VFS-aware (loose DDC file
//     first, then the mounted pak), because a player's install has no loose cache at all.
//
// The load being VFS-aware is the entire point of this file existing. It used to be a raw
// std::ifstream inside ShaderCompiler.cpp, which meant the packager shipped a warm cache the
// runtime could not see — every startup of a packaged game recompiled every shader it already had.

#include <Common/Core/ResultStr.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

namespace Desert::Core
{
    // The DDC entry for `key`: <DDC root>/Buckets/ShaderCache/... (Common::DDC::PathFor), where the DDC root is
    // machine.json DerivedDataCachePath or, by default, <projectDir>/DerivedDataCache.
    std::filesystem::path SpirvCachePathForKey( uint64_t key );

    // Loose file first (dev override), then the mounted .dpak. nullopt on miss or a torn artifact
    // (size not a whole number of SPIR-V words) — a corrupt cache entry is simply recompiled.
    std::optional<std::vector<uint32_t>> TryLoadCachedSpirv( uint64_t key );

    // Whether the artifact actually landed on disk, and if not, the file system's reason. The runtime
    // compiler warns and carries on — a read-only install (inside an .app bundle) simply keeps no
    // cache, which is the documented contract, but a dev machine whose DDC silently stays cold pays
    // every compile on every start, so the failure is logged and counted. The PACKAGER may not carry on: a cook
    // that could not write is a cook that ships nothing under that key, and every player then pays the compile it
    // was supposed to have been spared. Silence there would reintroduce П2 one artifact at a time, so the return
    // value exists to be checked.
    Common::BoolResultStr StoreCachedSpirv( uint64_t key, const std::vector<uint32_t>& spirv );

    // What the shader cache did since the process started: loads served from the cache, fresh
    // compiles, and compiles whose store failed. The preloader prints them once startup has compiled
    // everything it will, so a cold cache (every start a full compile) is one line in the log.
    struct ShaderCacheCounts
    {
        uint64_t Hits          = 0;
        uint64_t Compiled      = 0;
        uint64_t StoreFailures = 0;
    };
    ShaderCacheCounts ReadShaderCacheCounts();
    void              CountShaderCacheHit();
    void              CountShaderCacheCompile( bool stored );

    // The bucket directory every SpirvCachePathForKey lives under (sharded by key prefix below it).
    std::filesystem::path ShaderCacheDir();
} // namespace Desert::Core
