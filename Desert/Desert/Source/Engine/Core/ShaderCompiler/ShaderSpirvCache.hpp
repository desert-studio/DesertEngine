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

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
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
        uint64_t MapHits       = 0; // programs built from a shader map: no parse, no preprocessing
        uint64_t MapMisses     = 0; // programs parsed and compiled (their map is written afterwards)
    };
    ShaderCacheCounts ReadShaderCacheCounts();
    void              CountShaderCacheHit();
    void              CountShaderCacheCompile( bool stored );
    void              CountShaderMapHit();
    void              CountShaderMapMiss();

    // WHERE shader startup time goes, per phase, summed over the process (workers included). The preloader's
    // one total ("78 programs in 3231 ms") was read as "compilation" for weeks; a warm SPIR-V cache took it
    // only from 3.8 s to 3.2 s, so the rest had to be named before anything else was changed. Pipelines are
    // counted here too although the preloader builds none: they are the other half of "shaders at startup",
    // built by the first frames, and the half a warm VkPipelineCache is supposed to remove.
    enum class ShaderPhase : uint8_t
    {
        ShaderMap,      // the raw-text key + the shader map read (the warm path's whole front end)
        Preprocess,     // pass metadata + include expansion of the program text
        CacheKey,       // ComputeShaderCacheKeyForProfile (hashes the source and every include)
        SpirvLoad,      // a cache hit's read
        Compile,        // a cache miss: shaderc + the store
        ShaderModule,   // vkCreateShaderModule
        Reflect,        // SPIR-V reflection + descriptor-set layouts
        PipelineCreate, // vkCreateGraphicsPipelines / vkCreateComputePipelines
        Count
    };
    struct ShaderPhaseTimes
    {
        std::array<uint64_t, static_cast<size_t>( ShaderPhase::Count )> Nanoseconds{};
        std::array<uint64_t, static_cast<size_t>( ShaderPhase::Count )> Calls{};

        double Milliseconds( const ShaderPhase phase ) const
        {
            return static_cast<double>( Nanoseconds[static_cast<size_t>( phase )] ) / 1.0e6;
        }
    };
    ShaderPhaseTimes ReadShaderPhaseTimes();
    void             AddShaderPhaseTime( ShaderPhase phase, std::chrono::nanoseconds elapsed );
    // "preprocess 412.3 ms/156, cache key ..., pipelines 0.0 ms/0" — one log line's worth.
    std::string FormatShaderPhaseTimes( const ShaderPhaseTimes& times );

    // Times its own scope into one phase.
    class ScopedShaderPhase
    {
    public:
        explicit ScopedShaderPhase( const ShaderPhase phase )
             : m_Phase( phase ), m_Start( std::chrono::steady_clock::now() )
        {
        }
        ~ScopedShaderPhase()
        {
            AddShaderPhaseTime( m_Phase, std::chrono::steady_clock::now() - m_Start );
        }
        ScopedShaderPhase( const ScopedShaderPhase& )            = delete;
        ScopedShaderPhase& operator=( const ScopedShaderPhase& ) = delete;

    private:
        ShaderPhase                           m_Phase;
        std::chrono::steady_clock::time_point m_Start;
    };

    // The bucket directory every SpirvCachePathForKey lives under (sharded by key prefix below it).
    std::filesystem::path ShaderCacheDir();
} // namespace Desert::Core
