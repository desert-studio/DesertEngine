#include "BootContent.hpp"

#include <Engine/Animation/AnimationLibrary.hpp>
#include <Engine/Assets/ContentRegistry.hpp>
#include <Engine/Assets/Shader/ShaderAsset.hpp>
#include <Engine/Assets/StringTableSource.hpp>
#include <Engine/Core/ShaderCompiler/DShader/DShaderParser.hpp>
#include <Engine/Core/ShaderCompiler/ShaderMapBuild.hpp>
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

        // Timed as a phase: Register() builds every stage of every pass, so this is the whole "shader startup
        // cost" in one number.
        const auto start   = std::chrono::steady_clock::now();
        double     assetMs = 0.0;

        // 1. THE ASSETS, on this thread and in registry order: AssetManager is not for workers.
        const auto rows = ContentRegistry::FilesOfKind( Common::Content::ContentKind::Shader );
        std::vector<std::shared_ptr<ShaderAsset>> shaders;
        shaders.reserve( rows.size() );
        for ( const auto& path : rows )
        {
            if ( stop && stop() )
            {
                LOG_INFO( "[BootContent] engine shader load stopped on request after {} of {} program(s)",
                          shaders.size(), rows.size() );
                break;
            }
            ReportItem( progress, path.filename().string(), shaders.size(), rows.size() );

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
            shaders.push_back( std::move( shader ) );
        }

        // 2. THE CPU HALF ON THE JOB SYSTEM (SHC1). Preprocess + compile of every program, then of every named
        // pass those programs declare, each answer stored in the shader map cache. Register() below then
        // reads the map instead of compiling, on this thread and in registry order, so the VkShaderModules,
        // the registration order and the SPIR-V are what the one-at-a-time start produced. A medium is left
        // out: it is source compiled into other programs, never a program of its own. A map the workers
        // could not store is simply built again by Register() — slower, not different.
        const Core::ShaderPhaseTimes        beforeWorkers = Core::ReadShaderPhaseTimes();
        const auto                          workersStart  = std::chrono::steady_clock::now();
        std::vector<Core::ShaderMapRequest> programs;
        for ( const auto& shader : shaders )
        {
            const std::string& source = shader->GetShaderContent();
            if ( Core::Preprocess::DShaderParser::MayDeclareMedium( source ) )
                continue;
            const auto& path = shader->GetMetadata().Filepath;
            programs.push_back( { source, path, {}, {}, path.stem().string() } );
        }
        const auto                          programMaps = Core::BuildShaderMaps( programs );
        std::vector<Core::ShaderMapRequest> passes;
        for ( size_t i = 0; i < programs.size(); ++i )
            for ( const auto& pass : programMaps[i].Map.Meta.PassNames )
                passes.push_back( { programs[i].Source, programs[i].Path, pass, {}, programs[i].Name } );
        Core::BuildShaderMaps( passes );
        const double workersMs =
             std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - workersStart ).count();
        Core::ShaderPhaseTimes onWorkers = Core::ReadShaderPhaseTimes();
        for ( size_t i = 0; i < onWorkers.Nanoseconds.size(); ++i )
        {
            onWorkers.Nanoseconds[i] -= beforeWorkers.Nanoseconds[i];
            onWorkers.Calls[i] -= beforeWorkers.Calls[i];
        }
        // Summed over the workers, so the phases may add up to more than the wall time: that excess is the
        // parallelism.
        LOG_INFO( "[BootContent] shader maps of {} program(s) + {} pass(es) built on the job system in {:.1f} ms "
                  "wall; summed over the workers: {}",
                  programs.size(), passes.size(), workersMs, Core::FormatShaderPhaseTimes( onWorkers ) );

        // 3. THE DEVICE HALF, in registry order.
        const Core::ShaderPhaseTimes before        = Core::ReadShaderPhaseTimes();
        const auto                   registerStart = std::chrono::steady_clock::now();
        std::size_t                  count         = 0;
        for ( const auto& shader : shaders )
        {
            Runtime::ResourceRegistry::GetShaderService()->Register( shader );
            ++count;
        }
        const double registerMs =
             std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - registerStart ).count();

        const auto ms =
             std::chrono::duration_cast<std::chrono::milliseconds>( std::chrono::steady_clock::now() - start )
                  .count();
        LOG_INFO( "[BootContent] {} engine shader program(s) ready in {} ms", count, ms );
        // The split of the registration. "other" is Register()'s own bookkeeping outside every timed phase; the
        // phases are summed over the programs registered HERE, so asset load + job system + register add up to
        // the total above.
        Core::ShaderPhaseTimes during  = Core::ReadShaderPhaseTimes();
        double                 timedMs = 0.0;
        for ( size_t i = 0; i < during.Nanoseconds.size(); ++i )
        {
            during.Nanoseconds[i] -= before.Nanoseconds[i];
            during.Calls[i] -= before.Calls[i];
            timedMs += during.Milliseconds( static_cast<Core::ShaderPhase>( i ) );
        }
        LOG_INFO( "[BootContent] shader phases: asset load {:.1f} ms, job system {:.1f} ms, register: {}, other "
                  "{:.1f} ms",
                  assetMs, workersMs, Core::FormatShaderPhaseTimes( during ), registerMs - timedMs );
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
