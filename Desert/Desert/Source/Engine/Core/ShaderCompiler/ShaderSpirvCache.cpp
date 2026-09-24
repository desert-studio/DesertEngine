#include "ShaderSpirvCache.hpp"

#include <Common/Content/DerivedDataCache.hpp>

#include <atomic>
#include <cstring>
#include <format>

namespace Desert::Core
{
    namespace
    {
        // The payload hash handed in is ComputeShaderCacheKeyForProfile's, which already folds the
        // source, every include, the stage, the variant and the compile profile; the DDC key wraps it
        // so a change to the COMPILER's output for identical inputs is one GUID edit here.
        constexpr Common::DDC::Deriver kSpirvDeriver{
             "ShaderCache", ".spv", { 0x6a1f3c0e9b2d4e71ULL, 0x8c5b0a3f17e2d964ULL } };

        // Shaders compile on job-system workers during preload, hence atomics.
        std::atomic<uint64_t> s_Hits{ 0 };
        std::atomic<uint64_t> s_Compiled{ 0 };
        std::atomic<uint64_t> s_StoreFailures{ 0 };
    } // namespace

    std::filesystem::path SpirvCachePathForKey( uint64_t key )
    {
        return Common::DDC::PathFor( kSpirvDeriver, Common::DDC::MakeKey( kSpirvDeriver, key, nullptr, 0 ) );
    }

    std::optional<std::vector<uint32_t>> TryLoadCachedSpirv( uint64_t key )
    {
        // A whole number of 32-bit words or it is not SPIR-V. The DDC reads the loose entry first (this
        // run's compiles, the dev override) and then the mounted archive, where a packaged game's live.
        const auto bytes =
             Common::DDC::Get( kSpirvDeriver, Common::DDC::MakeKey( kSpirvDeriver, key, nullptr, 0 ) );
        if ( !bytes || bytes->empty() || ( bytes->size() % sizeof( uint32_t ) ) != 0 )
            return std::nullopt;
        std::vector<uint32_t> words( bytes->size() / sizeof( uint32_t ) );
        std::memcpy( words.data(), bytes->data(), bytes->size() );
        return words;
    }

    Common::BoolResultStr StoreCachedSpirv( uint64_t key, const std::vector<uint32_t>& spirv )
    {
        // Write-then-rename (И2) inside DDC::Put: a torn .spv under a key that says it is valid would load
        // next run — the word-count check catches a wrong LENGTH but not a whole number of wrong words.
        const std::string_view bytes( reinterpret_cast<const char*>( spirv.data() ),
                                      spirv.size() * sizeof( uint32_t ) );
        return Common::DDC::Put( kSpirvDeriver, Common::DDC::MakeKey( kSpirvDeriver, key, nullptr, 0 ), bytes );
    }

    ShaderCacheCounts ReadShaderCacheCounts()
    {
        return { s_Hits.load(), s_Compiled.load(), s_StoreFailures.load() };
    }

    void CountShaderCacheHit()
    {
        s_Hits.fetch_add( 1 );
    }

    void CountShaderCacheCompile( const bool stored )
    {
        s_Compiled.fetch_add( 1 );
        if ( !stored )
            s_StoreFailures.fetch_add( 1 );
    }

    std::filesystem::path ShaderCacheDir()
    {
        return Common::DDC::BucketDir( kSpirvDeriver.Bucket );
    }
} // namespace Desert::Core
