#include "ShaderMapBuild.hpp"

#include <Engine/Core/ShaderCompiler/ShaderCacheKey.hpp>
#include <Engine/Core/ShaderCompiler/ShaderCompiler.hpp>
#include <Engine/Core/ShaderCompiler/ShaderPreprocess/ShaderPreprocessor.hpp>
#include <Engine/Core/ShaderCompiler/ShaderSpirvCache.hpp>

#include <Common/Core/Core.hpp>
#include <Common/Core/JobSystem.hpp>

#include <algorithm>

namespace Desert::Core
{
    Common::ResultStr<ShaderMap> BuildShaderMap( const ShaderMapRequest& request )
    {
        // THE WARM PATH: the shader map is keyed by the raw text + include files, so a hit is the whole
        // program — metadata and SPIR-V — without parsing the DShader or preprocessing a stage.
        uint64_t        mapKey = 0;
        ShaderMapLookup lookup;
        {
            const ScopedShaderPhase timer( ShaderPhase::ShaderMap );
            mapKey = ComputeShaderMapKey( request.Source, request.Path, request.PassName,
                                          SpirvDebugInfoThisBuild(), request.Variant );
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
            auto                    preprocessed =
                 Preprocess::ShaderPreprocess::PreProcessPass( request.Source, request.Path, request.PassName );
            built.Meta = std::move( preprocessed.Meta );
            stages     = std::move( preprocessed.Stages );
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
            built.Stages.push_back( { stage, std::move( spirvResult.GetValue() ) } );
        }
        std::sort( built.Stages.begin(), built.Stages.end(), []( const ShaderMapStage& a, const ShaderMapStage& b )
                   { return static_cast<uint32_t>( a.Stage ) < static_cast<uint32_t>( b.Stage ); } );
        if ( const auto stored = StoreShaderMap( mapKey, built ); !stored )
            LOG_WARN( "[ShaderMap] '{}': could not store the shader map {:016x}: {}", request.Name, mapKey,
                      stored.GetError() );
        return Common::MakeSuccess( std::move( built ) );
    }

    std::vector<ShaderMapOutcome> BuildShaderMaps( const std::span<const ShaderMapRequest> requests )
    {
        std::vector<ShaderMapOutcome> outcomes( requests.size() );
        // One program per claim: a program is milliseconds of shaderc, far above the cost of a claim, and
        // programs differ in cost by 10x, so small claims keep every worker busy to the end.
        ::Common::JobSystem::Get().ParallelFor( requests.size(),
                                                [&]( const size_t i )
                                                {
                                                    auto built = BuildShaderMap( requests[i] );
                                                    if ( built.IsSuccess() )
                                                        outcomes[i].Map = std::move( built.GetValue() );
                                                    else
                                                        outcomes[i].Error = built.GetError();
                                                } );
        return outcomes;
    }
} // namespace Desert::Core
