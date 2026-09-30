#include "ShaderMapBuild.hpp"

#include <Engine/Core/ShaderCompiler/ShaderCacheKey.hpp>
#include <Engine/Core/ShaderCompiler/ShaderCompiler.hpp>
#include <Engine/Core/ShaderCompiler/ShaderPreprocess/ShaderPreprocessor.hpp>
#include <Engine/Core/ShaderCompiler/ShaderSpirvCache.hpp>

#include <Common/Core/Core.hpp>
#include <Common/Core/JobSystem.hpp>

#include <algorithm>
#include <unordered_map>

#include <format>

namespace Desert::Core
{
    namespace
    {
        uint64_t MapKeyOf( const ShaderMapRequest& request )
        {
            const ScopedShaderPhase timer( ShaderPhase::ShaderMap );
            return ComputeShaderMapKey( request.Source, request.Path, request.PassName, SpirvDebugInfoThisBuild(),
                                        request.Variant );
        }

        Common::ResultStr<ShaderMap> BuildShaderMapUnderKey( const ShaderMapRequest& request,
                                                             const uint64_t          mapKey )
        {
            // THE WARM PATH: the shader map is keyed by the raw text + include files, so a hit is the whole
            // program — metadata and SPIR-V — without parsing the DShader or preprocessing a stage.
            ShaderMapLookup lookup;
            {
                const ScopedShaderPhase timer( ShaderPhase::ShaderMap );
                lookup = TryLoadShaderMap( mapKey );
            }
            if ( lookup.Map )
            {
                CountShaderMapHit();
                return Common::MakeSuccess( std::move( *lookup.Map ) );
            }
            CountShaderMapMiss();
            if ( !lookup.Rejected.empty() )
                LOG_WARN( "[ShaderMap] '{}': cached entry rejected, rebuilding it ({})", request.Name,
                          lookup.Rejected );

            ShaderMap                                             built;
            std::unordered_map<Formats::ShaderStage, std::string> stages;
            {
                const ScopedShaderPhase timer( ShaderPhase::Preprocess );
                // A source that does not parse is THIS shader's refusal, named by its file (the preprocessor
                // prefixes it) and by the program asked for — never a verify on a job-system worker.
                auto preprocessed = Preprocess::ShaderPreprocess::PreProcessPass( request.Source, request.Path,
                                                                                  request.PassName );
                if ( !preprocessed.IsSuccess() )
                    return Common::MakeError<ShaderMap>( std::format( "shader '{}'{}: {}", request.Name,
                                                                      request.PassName.empty()
                                                                           ? std::string()
                                                                           : std::format( " pass '{}'", request.PassName ),
                                                                      preprocessed.GetError() ) );
                auto pass  = preprocessed.ExtractValue();
                built.Meta = std::move( pass.Meta );
                stages     = std::move( pass.Stages );
            }
            if ( built.Meta.HasParams() || built.Meta.State.Topology.has_value() )
                LOG_INFO( "Shader '{}': parsed {} param(s) + render-state from shader metadata", request.Name,
                          built.Meta.Params.size() );

            for ( const auto& [stage, source] : stages )
            {
                auto spirvResult =
                     ShaderCompiler::CompileGLSLToSPIRV( stage, source, request.Path.string(), request.Variant );
                if ( !spirvResult.IsSuccess() )
                    return Common::MakeError<ShaderMap>( spirvResult.GetError() );
                built.Stages.push_back( { stage, spirvResult.GetValue() } );
            }
            std::sort( built.Stages.begin(), built.Stages.end(),
                       []( const ShaderMapStage& a, const ShaderMapStage& b )
                       { return static_cast<uint32_t>( a.Stage ) < static_cast<uint32_t>( b.Stage ); } );
            if ( const auto stored = StoreShaderMap( mapKey, built ); !stored )
                LOG_WARN( "[ShaderMap] '{}': could not store the shader map {:016x}: {}", request.Name, mapKey,
                          stored.GetError() );
            return Common::MakeSuccess( std::move( built ) );
        }
    } // namespace

    Common::ResultStr<ShaderMap> BuildShaderMap( const ShaderMapRequest& request )
    {
        return BuildShaderMapUnderKey( request, MapKeyOf( request ) );
    }

    std::vector<ShaderMapOutcome> BuildShaderMaps( const std::span<const ShaderMapRequest> requests )
    {
        auto& jobs = ::Common::JobSystem::Get();

        // ONE BUILD PER MAP KEY. Two requests under one key are one artifact (without debug info the key does
        // not name the file, so two files with the same text are one key): built by two workers at once they
        // would both write that key's one <file>.tmp. The first index builds, the others copy its answer.
        std::vector<uint64_t> keys( requests.size() );
        jobs.ParallelFor( requests.size(), [&]( const size_t i ) { keys[i] = MapKeyOf( requests[i] ); } );
        std::vector<size_t>                  builders; // indices that build, in request order
        std::vector<size_t>                  builderOf( requests.size() );
        std::unordered_map<uint64_t, size_t> firstIndexOfKey;
        for ( size_t i = 0; i < requests.size(); ++i )
        {
            const auto [first, inserted] = firstIndexOfKey.try_emplace( keys[i], i );
            builderOf[i]                 = first->second;
            if ( inserted )
                builders.push_back( i );
        }

        std::vector<ShaderMapOutcome> outcomes( requests.size() );
        // One program per claim: a program is milliseconds of shaderc, far above the cost of a claim, and
        // programs differ in cost by 10x, so small claims keep every worker busy to the end.
        jobs.ParallelFor( builders.size(),
                          [&]( const size_t b )
                          {
                              const size_t i     = builders[b];
                              auto         built = BuildShaderMapUnderKey( requests[i], keys[i] );
                              if ( built.IsSuccess() )
                                  outcomes[i].Map = built.GetValue();
                              else
                                  outcomes[i].Error = built.GetError();
                          } );
        for ( size_t i = 0; i < requests.size(); ++i )
            if ( builderOf[i] != i )
                outcomes[i] = outcomes[builderOf[i]];
        return outcomes;
    }
} // namespace Desert::Core
