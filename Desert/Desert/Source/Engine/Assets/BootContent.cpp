#include "BootContent.hpp"

#include <Engine/Animation/AnimationLibrary.hpp>
#include <Engine/Assets/ContentRegistry.hpp>
#include <Engine/Assets/Shader/ShaderAsset.hpp>
#include <Engine/Assets/StringTableSource.hpp>
#include <Engine/Core/ShaderCompiler/ShaderSpirvCache.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <chrono>

namespace Desert::Assets
{
    std::size_t EngineShaderCount()
    {
        return ContentRegistry::FilesOfKind( Common::Content::ContentKind::Shader ).size();
    }

    void CompileEngineShaders( const std::shared_ptr<AssetManager>& manager, const ItemProgress& progress,
                               const StopRequested& stop )
    {
        if ( !manager )
        {
            LOG_ERROR( "[BootContent] no asset manager to create the engine shaders in; none compiled" );
            return;
        }

        // Timed as a phase: Register() compiles every stage of every pass, so this is the whole "shader
        // startup cost" in one number — against it, the per-miss lines ShaderCompiler prints say how much was
        // real compilation rather than cache reads.
        const auto                   start   = std::chrono::steady_clock::now();
        const Core::ShaderPhaseTimes before  = Core::ReadShaderPhaseTimes();
        double                       assetMs = 0.0;

        const auto  rows  = ContentRegistry::FilesOfKind( Common::Content::ContentKind::Shader );
        std::size_t count = 0;
        for ( const auto& path : rows )
        {
            if ( stop && stop() )
            {
                LOG_INFO( "[BootContent] engine shader compile stopped on request after {} of {} program(s)",
                          count, rows.size() );
                break;
            }
            ReportItem( progress, path.filename().string(), count, rows.size() );

            const auto created = std::chrono::steady_clock::now();
            auto       shader  = manager->CreateAsset<ShaderAsset>( path );
            assetMs +=
                 std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - created ).count();
            // NULL IS A REACHABLE ANSWER: CreateAsset returns nullptr when the parse fails (it logs why), and one
            // malformed `.shader` must not take the whole start down with it.
            if ( !shader )
            {
                LOG_ERROR( "[BootContent] engine shader '{}' is a row of the content registry and could not be "
                           "loaded (the parse error is logged above); every material that names it has no shader",
                           path.generic_string() );
                continue;
            }
            Runtime::ResourceRegistry::GetShaderService()->Register( shader );
            ++count;
        }

        const auto ms =
             std::chrono::duration_cast<std::chrono::milliseconds>( std::chrono::steady_clock::now() - start )
                  .count();
        LOG_INFO( "[BootContent] {} engine shader program(s) ready in {} ms", count, ms );
        // The split of that total. "other" is Register()'s own bookkeeping outside every timed phase; the phases
        // are summed over the programs registered HERE, so the line adds up to the total above.
        Core::ShaderPhaseTimes during  = Core::ReadShaderPhaseTimes();
        double                 timedMs = 0.0;
        for ( size_t i = 0; i < during.Nanoseconds.size(); ++i )
        {
            during.Nanoseconds[i] -= before.Nanoseconds[i];
            during.Calls[i] -= before.Calls[i];
            timedMs += during.Milliseconds( static_cast<Core::ShaderPhase>( i ) );
        }
        LOG_INFO( "[BootContent] shader phases: asset load {:.1f} ms, {}, other {:.1f} ms", assetMs,
                  Core::FormatShaderPhaseTimes( during ), static_cast<double>( ms ) - assetMs - timedMs );
        // Cumulative for the process, which at this point is startup: a cold cache shows as hits 0.
        const Core::ShaderCacheCounts cache = Core::ReadShaderCacheCounts();
        LOG_INFO( "[ShaderCache] shader map {} hit(s) / {} miss(es); SPIR-V {} hit(s), {} compiled, {} store "
                  "failure(s) in {}",
                  cache.MapHits, cache.MapMisses, cache.Hits, cache.Compiled, cache.StoreFailures,
                  Core::ShaderCacheDir().string() );
    }

    void IndexAnimationClips( AssetManager& manager, Animation::AnimationLibrary& library )
    {
        // The registry's own clip count is what PopulateLibrary is given: it is how "this project has no clips"
        // is told apart from "the clips never arrived" (see PopulateLibrary).
        const std::size_t clipRows = ContentRegistry::Rows( Common::Content::ContentKind::Animation ).size();
        if ( const auto populated = Animation::PopulateLibrary( manager, library, clipRows ); !populated )
            LOG_ERROR( "[AnimationLibrary] {}", populated.GetError() );
    }

    void RequestStringTables( const std::weak_ptr<AssetManager>& manager )
    {
        if ( const auto begun = BeginStringTables( manager ); !begun )
            LOG_ERROR( "[Localization] {}", begun.GetError() );
    }
} // namespace Desert::Assets
