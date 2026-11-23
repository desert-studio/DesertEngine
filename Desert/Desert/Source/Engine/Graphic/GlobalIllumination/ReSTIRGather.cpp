#include <Engine/Graphic/GlobalIllumination/ReSTIRGather.hpp>

#include <Engine/Graphic/GlobalIllumination/ScreenProbeGather.hpp>
#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/View/SceneViewState.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Runtime/Services/Shader/ShaderService.hpp>

#include <Common/Core/Logger.hpp>
#include <Common/Core/Math/Rounding.hpp>

#include <algorithm>
#include <format>
#include <span>
#include <string>
#include <string_view>

namespace
{
    // The slot names the passes bind, spelled once (RDGShaderParameters.hpp).
    struct ReSTIRGIShaderParameters
    {
        DESERT_SHADER_PARAMETER( StorageBuffer, ReSTIRParamsUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, GIFrameUB );
        DESERT_SHADER_PARAMETER( Sampler, PointClampSampler );
        DESERT_SHADER_PARAMETER( Sampler, LinearClampSampler );
        DESERT_SHADER_PARAMETER( Texture, GBufferDepthTexture );
        DESERT_SHADER_PARAMETER( Texture, GBufferBTexture );
    };
    // ReSTIRGIInitial.shader: the screen probes' cache-miss leg's reads (ScreenProbeTrace.shader).
    struct ReSTIRGIInitialShaderParameters
    {
        DESERT_SHADER_PARAMETER( StorageBuffer, PointLightsUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, SpotLightsUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, LightGridParamsUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, LightGridWorldCellsUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, RadianceCacheParamsUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, RadianceCacheIndirectionUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, RadianceCacheProbesUB );
        DESERT_SHADER_PARAMETER( Texture, EnvironmentSpecularTexture );
        DESERT_SHADER_PARAMETER( Texture, GBufferATexture );
        DESERT_SHADER_PARAMETER( Texture, GBufferEmissiveTexture );
        DESERT_SHADER_PARAMETER( Texture, GBufferShadingWordTexture );
        DESERT_SHADER_PARAMETER( Texture, RadianceCacheIrradianceTexture );
        DESERT_SHADER_PARAMETER( Texture, RadianceCacheDepthTexture );
        DESERT_SHADER_PARAMETER( Texture, RadianceCacheRadianceAtlasTexture );
        DESERT_SHADER_PARAMETER( StorageTexture, ReSTIRHitOut );
        DESERT_SHADER_PARAMETER( StorageTexture, ReSTIRRadianceOut );
        DESERT_SHADER_PARAMETER( StorageTexture, ReSTIRStateOut );
    };
    // The view's previous reservoirs and the velocity that finds them: the temporal pass reuses them, the initial
    // pass's validation rays re-trace them (both RESTIRPreviousTexel).
    struct ReSTIRGIHistoryShaderParameters
    {
        DESERT_SHADER_PARAMETER( Texture, Velocity );
        DESERT_SHADER_PARAMETER( Texture, PreviousHit );
        DESERT_SHADER_PARAMETER( Texture, PreviousRadiance );
        DESERT_SHADER_PARAMETER( Texture, PreviousState );
    };
    struct ReSTIRGITemporalShaderParameters
    {
        DESERT_SHADER_PARAMETER( Texture, InitialHit );
        DESERT_SHADER_PARAMETER( Texture, InitialRadiance );
        DESERT_SHADER_PARAMETER( Texture, InitialState );
    };
    struct ReSTIRGISpatialShaderParameters
    {
        DESERT_SHADER_PARAMETER( Texture, TemporalHit );
        DESERT_SHADER_PARAMETER( Texture, TemporalRadiance );
        DESERT_SHADER_PARAMETER( Texture, TemporalState );
    };
    struct ReSTIRGIResolveShaderParameters
    {
        DESERT_SHADER_PARAMETER( StorageBuffer, RadianceCacheParamsUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, RadianceCacheIndirectionUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, RadianceCacheProbesUB );
        DESERT_SHADER_PARAMETER( Texture, GBufferShadingWord );
        DESERT_SHADER_PARAMETER( Texture, SpatialHit );
        DESERT_SHADER_PARAMETER( Texture, SpatialRadiance );
        DESERT_SHADER_PARAMETER( Texture, SpatialState );
        DESERT_SHADER_PARAMETER( Texture, RadianceCacheIrradianceTexture );
        DESERT_SHADER_PARAMETER( Texture, RadianceCacheDepthTexture );
        DESERT_SHADER_PARAMETER( StorageTexture, ReSTIRDiffuseOut );
        DESERT_SHADER_PARAMETER( StorageTexture, ReSTIRSpecularOut );
        DESERT_SHADER_PARAMETER( StorageTexture, ReSTIRBackfaceOut );
    };
} // namespace

namespace Desert::Graphic::GlobalIllumination
{
    namespace
    {
        using Common::Math::DivideRoundUp;

        // By ReSTIRGather::Program: InitialNoHistory is the initial shader's other permutation.
        constexpr std::array<std::string_view, 5> kReSTIRShaderNames{ "ReSTIRGIInitial", "ReSTIRGITemporal",
                                                                      "ReSTIRGISpatial", "ReSTIRGIResolve",
                                                                      "ReSTIRGIInitial" };

        // The permutation of the programs that trace nothing (Temporal, Resolve): no define, cooked.
        Core::ShaderVariant PlainVariant()
        {
            return Core::ShaderVariant{ .ShippedByCook = true };
        }

        // The initial pass of a frame without the view's previous reservoirs: the tracer's permutation, no
        // validation rays (kReSTIRInitialNoHistoryDefine).
        Core::ShaderVariant NoHistoryVariant( const Pipeline& pipeline )
        {
            Core::ShaderVariant variant = TracerVariant( pipeline );
            variant.Defines.emplace_back( kReSTIRInitialNoHistoryDefine );
            return variant;
        }

        // The hardware entries of kTracerPermutations: the only tracer ReSTIRGatherRuns lets the gather run on.
        bool HardwarePermutation( const Pipeline& pipeline )
        {
            return pipeline.Tracer == Tracer::HardwareRayQuery;
        }
    } // namespace

    ReSTIRGather::ReSTIRGather()  = default;
    ReSTIRGather::~ReSTIRGather() = default;

    Common::BoolResultStr ReSTIRGather::Acquire( const Program program, const Core::ShaderVariant& variant )
    {
        static_assert( kReSTIRShaderNames.size() == ProgramCount );
        const std::string name( kReSTIRShaderNames[program] );
        auto acquired = Runtime::ResourceRegistry::GetShaderService()->AcquireVariant( name, variant );
        if ( !acquired )
            return Common::MakeFormattedError<bool>( "ReSTIR gather: shader {} was refused: {}", name,
                                                     acquired.GetError() );
        auto shader  = acquired.ExtractValue();
        auto compute = ComputePipeline::Create( { .Shader = shader, .DebugName = name } );
        if ( !compute )
            return Common::MakeFormattedError<bool>( "ReSTIR gather: {}: {}", name, compute.GetError() );
        m_Shaders[program]   = std::move( shader );
        m_Pipelines[program] = compute.GetValue();
        return BOOLSUCCESS;
    }

    Common::BoolResultStr ReSTIRGather::EnsurePrograms( const Pipeline& pipeline )
    {
        if ( !HardwarePermutation( pipeline ) )
            return Common::MakeFormattedError<bool>(
                 "ReSTIR gather: the {} tracer has no ray query from the pixel (ReSTIRGatherRuns: hardware only)",
                 TracerDefine( pipeline.Tracer ) );
        for ( const Program program : { Temporal, Resolve } )
            if ( !m_Pipelines[program] )
                if ( auto acquired = Acquire( program, PlainVariant() ); !acquired )
                    return acquired;
        if ( m_TracingProgramsValid && pipeline == m_TracingProgramsFor )
            return BOOLSUCCESS;
        m_TracingProgramsValid = false;
        // No ternary of two ShaderVariant prvalues as an argument (an MSVC hang on a ternary by-value argument).
        const Core::ShaderVariant tracing   = TracerVariant( pipeline );
        const Core::ShaderVariant noHistory = NoHistoryVariant( pipeline );
        for ( const Program program : { Initial, InitialNoHistory, Spatial } )
            if ( auto acquired = Acquire( program, program == InitialNoHistory ? noHistory : tracing ); !acquired )
                return acquired;
        m_TracingProgramsFor   = pipeline;
        m_TracingProgramsValid = true;
        return BOOLSUCCESS;
    }

    std::vector<Core::CookedProgramVariant> ReSTIRGather::ProgramVariants()
    {
        std::vector<Core::CookedProgramVariant> variants;
        for ( const Program program : { Temporal, Resolve } )
            variants.push_back( { std::string( kReSTIRShaderNames[program] ), PlainVariant() } );
        for ( const Pipeline& tracer : kTracerPermutations )
            if ( HardwarePermutation( tracer ) )
            {
                for ( const Program program : { Initial, Spatial } )
                    variants.push_back( { std::string( kReSTIRShaderNames[program] ), TracerVariant( tracer ) } );
                variants.push_back(
                     { std::string( kReSTIRShaderNames[InitialNoHistory] ), NoHistoryVariant( tracer ) } );
            }
        return variants;
    }

    Common::BoolResultStr ReSTIRGather::AddPasses( RDG::Builder& graph, const ScreenProbeGatherInputs& inputs,
                                                   const float rayClamp, const bool historyReadable,
                                                   ScreenProbeGatherHistory&  history,
                                                   const ReSTIRGatherOutputs& outputs )
    {
        // On the async queue, as the screen probes' trace legs (FPS-GI-ASYNC): every input and output is a graph
        // resource.
        const RDG::PassFlags kAsyncPass = RDG::PassFlags::Compute | RDG::PassFlags::AsyncCompute;
        if ( auto programs = EnsurePrograms( inputs.Tracer.Pipeline ); !programs )
            return programs;
        if ( !m_Logged )
        {
            LOG_INFO( "[GI] ReSTIR temporal=on spatial={} validate=on", kReSTIRSpatialSamples );
            m_Logged = true;
        }

        using History               = ScreenProbeGatherHistory;
        const glm::uvec2 reservoirs = ReSTIRReservoirExtent( inputs.Extent );
        const ViewExtent reservoirExt{ .Width = reservoirs.x, .Height = reservoirs.y };
        // The view's previous reservoirs: read only when the view's previous frame is a previous frame of this one
        // (ViewFrame::HistoryValid, independent of the denoiser) and they were written at this extent (RegisterPreviousReservoirs).
        const History::ReservoirRefs previous = history.RegisterPreviousReservoirs( graph, reservoirExt );
        const bool previousValid = historyReadable && std::ranges::all_of( previous, []( const RDG::TextureRef& t )
                                                                           { return t.IsValid(); } );
        // ONE ROUTE A FRAME (UE: temporal resampling skipped when !bIsHistoryValid): with them, Initial (validation
        // rays) -> Temporal (into the history) -> Spatial; without them, InitialNoHistory writes its one-sample
        // reservoirs straight into the history and Spatial reads them - no temporal pass, no texture standing in for
        // a history that is not there.
        if ( !previousValid && !m_LoggedNoHistory )
        {
            LOG_INFO( "[GI] ReSTIR: no reservoir history (first frame, reset or new extent) - temporal resampling "
                      "skipped, the initial reservoirs seed the history" );
            m_LoggedNoHistory = true;
        }

        ReSTIRParamsPayload params;
        params.Extent = glm::uvec4( reservoirs, kReSTIRSpatialSamples, 0u );
        params.Frame = glm::uvec4( ++m_FrameIndex, 0u, 0u, 0u );
        params.Tuning =
             glm::vec4( kReSTIRTemporalMaxM, kReSTIRSpatialRadius, kReSTIRDepthTolerance, kReSTIRNormalTolerance );
        params.Jitter   = glm::vec4( inputs.JitterNdc, inputs.PrevJitterNdc );
        params.RayClamp   = glm::vec4( rayClamp, 0.0f, 0.0f, 0.0f );
        params.Validation = glm::vec4( kReSTIRValidationLuminance, kReSTIRValidationDistance,
                                       kReSTIRValidationFloor * rayClamp, 0.0f );
        const RDG::BufferRef paramsBuffer =
             graph.CreateBuffer( RDG::BufferDesc{ sizeof( params ) }, "ReSTIR.Params" );
        graph.QueueBufferUpload( paramsBuffer, std::as_bytes( std::span( &params, 1 ) ) );

        const auto transient = [&]( const History::Reservoir reservoir, const char* name )
        { return graph.CreateTexture( History::ReservoirDesc( reservoir, reservoirExt ), name ); };
        // This frame's reservoirs, extracted into the view for the next frame: the temporal pass's output, or the
        // initial pass's when there is no history.
        const History::ReservoirRefs current = history.CreateCurrentReservoirs( graph, reservoirExt );
        History::ReservoirRefs       initial = current;
        if ( previousValid )
            initial = { transient( History::ReservoirHit, "ReSTIR.InitialHit" ),
                        transient( History::ReservoirRadiance, "ReSTIR.InitialRadiance" ),
                        transient( History::ReservoirState, "ReSTIR.InitialState" ) };
        const History::ReservoirRefs spatial{ transient( History::ReservoirHit, "ReSTIR.SpatialHit" ),
                                              transient( History::ReservoirRadiance, "ReSTIR.SpatialRadiance" ),
                                              transient( History::ReservoirState, "ReSTIR.SpatialState" ) };

        constexpr RDG::Access kW  = RDG::Access::StorageWrite;
        constexpr RDG::Access kR  = RDG::Access::StorageRead;
        constexpr RDG::Access kS  = RDG::Access::SampledCompute;
        const auto            all = RDG::SubresourceRange::All();
        using P                   = ReSTIRGIShaderParameters;
        // What every pass reads: its parameters, the GI frame, the G-buffer's depth and normal.
        const auto common = [&]( RDG::BindingBlockBuilder& block )
        {
            block.Storage( P::ReSTIRParamsUB, paramsBuffer, kR )
                 .Storage( P::GIFrameUB, inputs.GIFrame, kR )
                 .Sampler( P::PointClampSampler, RDG::SamplerDesc::PointClamp() )
                 .Texture( P::GBufferDepthTexture, inputs.GBufferDepth, kS, all );
        };
        // The G-buffer's normal: only the initial and the resolve shaders declare it (the temporal and the
        // spatial pass read the normal packed in State.w) - ValidatePassBindings refuses a slot a shader lacks.
        const auto gbufferNormal = [&]( RDG::BindingBlockBuilder& block )
        { block.Texture( P::GBufferBTexture, inputs.GBufferB, kS, all ); };
        // What the initial pass's validation rays and the temporal pass both read: the view's previous reservoirs
        // and the velocity. Bound only when previousValid (neither pass runs on them otherwise).
        const auto historyReads = [&]( RDG::BindingBlockBuilder& block )
        {
            using H = ReSTIRGIHistoryShaderParameters;
            block.Texture( H::Velocity, inputs.Velocity, kS, all )
                 .Texture( H::PreviousHit, previous[History::ReservoirHit], kS, all )
                 .Texture( H::PreviousRadiance, previous[History::ReservoirRadiance], kS, all )
                 .Texture( H::PreviousState, previous[History::ReservoirState], kS, all );
        };
        const glm::uvec2 reservoirGroups( DivideRoundUp( reservoirs.x, 8u ), DivideRoundUp( reservoirs.y, 8u ) );
        const glm::uvec2 pixelGroups( DivideRoundUp( inputs.Extent.x, 8u ), DivideRoundUp( inputs.Extent.y, 8u ) );
        const auto       dispatch = [&]( const Program program, const glm::uvec2 groups )
        {
            ComputePipeline* const pipeline = m_Pipelines[program].get();
            return [pipeline, groups]( RDG::PassContext& context ) -> Common::BoolResultStr
            {
                RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                return Renderer::DispatchCompute( bindings, *pipeline, groups.x, groups.y, 1u );
            };
        };
        const auto bindings = [&]( RDG::PassBuilder& builder, const Program program )
        {
            return builder.Bindings( m_Layouts[program].Get( m_Shaders[program] ),
                                     Renderer::GetPipelineRouteFill( *m_Pipelines[program] ) );
        };

        // The initial sample reads the radiance cache's probes (the hit's bounce, as the cache-miss leg).
        if ( inputs.FinishRadianceCache )
            if ( auto finished = inputs.FinishRadianceCache(); !finished )
                return finished;
        const bool    readsCards     = inputs.Tracer.Pipeline.Hits == HitLighting::SurfaceCache;
        const Program initialProgram = previousValid ? Initial : InitialNoHistory;
        const char*   initialName    = previousValid ? "ReSTIRGI: Initial" : "ReSTIRGI: Initial (no history)";
        if ( !previousValid )
            history.SetReservoirWriterPass( graph.GetPassCount() );
        graph.AddPass(
             initialName, kAsyncPass,
             [&]( RDG::PassBuilder& builder )
             {
                 using I    = ReSTIRGIInitialShaderParameters;
                 auto block = bindings( builder, initialProgram );
                 common( block );
                 block.Storage( I::PointLightsUB, inputs.PointLights, kR )
                      .Storage( I::SpotLightsUB, inputs.SpotLights, kR )
                      .BindlessRead( inputs.LightFunctionAtlas, kS )
                      .Storage( I::LightGridParamsUB, inputs.WorldGridParams, kR )
                      .Storage( I::LightGridWorldCellsUB, inputs.WorldGridCells, kR )
                      .Storage( I::RadianceCacheParamsUB, inputs.RadianceCache.Params, kR )
                      .Storage( I::RadianceCacheIndirectionUB, inputs.RadianceCache.Indirection, kR )
                      .Storage( I::RadianceCacheProbesUB, inputs.RadianceCache.Probes, kR )
                      .Texture( I::EnvironmentSpecularTexture, inputs.EnvSpecular, kS, all )
                      .Sampler( P::LinearClampSampler, RDG::SamplerDesc::LinearClamp() )
                      .Texture( I::GBufferATexture, inputs.GBufferA, kS, all )
                      .Texture( I::GBufferEmissiveTexture, inputs.GBufferEmissive, kS, all )
                      .Texture( I::GBufferShadingWordTexture, inputs.GBufferShadingWord, kS, all )
                      .Texture( I::RadianceCacheIrradianceTexture, inputs.RadianceCache.Irradiance, kS, all )
                      .Texture( I::RadianceCacheDepthTexture, inputs.RadianceCache.Depth, kS, all )
                      .Texture( I::RadianceCacheRadianceAtlasTexture, inputs.RadianceCache.Radiance, kS, all )
                      .Storage( I::ReSTIRHitOut, initial[History::ReservoirHit], kW )
                      .Storage( I::ReSTIRRadianceOut, initial[History::ReservoirRadiance], kW )
                      .Storage( I::ReSTIRStateOut, initial[History::ReservoirState], kW );
                 gbufferNormal( block );
                 if ( previousValid )
                     historyReads( block );
                 DeclareTracerReads( block, inputs.Tracer, kS, TracerPointClamp::Bind );
                 if ( readsCards )
                     DeclareSurfaceCacheReads( block, inputs.SurfaceCache );
             },
             dispatch( initialProgram, reservoirGroups ) );

        // Temporal: into the view's reservoir history (next frame's previous). Only with a history to reuse.
        if ( previousValid )
        {
            history.SetReservoirWriterPass( graph.GetPassCount() );
            graph.AddPass(
                 "ReSTIRGI: Temporal", kAsyncPass,
                 [&]( RDG::PassBuilder& builder )
                 {
                     using T    = ReSTIRGITemporalShaderParameters;
                     using I    = ReSTIRGIInitialShaderParameters;
                     auto block = bindings( builder, Temporal );
                     common( block );
                     historyReads( block );
                     block.Texture( T::InitialHit, initial[History::ReservoirHit], kS, all )
                          .Texture( T::InitialRadiance, initial[History::ReservoirRadiance], kS, all )
                          .Texture( T::InitialState, initial[History::ReservoirState], kS, all )
                          .Storage( I::ReSTIRHitOut, current[History::ReservoirHit], kW )
                          .Storage( I::ReSTIRRadianceOut, current[History::ReservoirRadiance], kW )
                          .Storage( I::ReSTIRStateOut, current[History::ReservoirState], kW );
                 },
                 dispatch( Temporal, reservoirGroups ) );
        }

        graph.AddPass(
             "ReSTIRGI: Spatial", kAsyncPass,
             [&]( RDG::PassBuilder& builder )
             {
                 using S    = ReSTIRGISpatialShaderParameters;
                 using I    = ReSTIRGIInitialShaderParameters;
                 auto block = bindings( builder, Spatial );
                 common( block );
                 block.Texture( S::TemporalHit, current[History::ReservoirHit], kS, all )
                      .Texture( S::TemporalRadiance, current[History::ReservoirRadiance], kS, all )
                      .Texture( S::TemporalState, current[History::ReservoirState], kS, all )
                      .Storage( I::ReSTIRHitOut, spatial[History::ReservoirHit], kW )
                      .Storage( I::ReSTIRRadianceOut, spatial[History::ReservoirRadiance], kW )
                      .Storage( I::ReSTIRStateOut, spatial[History::ReservoirState], kW );
                 // Visibility rays only: no hit is lit, nothing of the tracer is point-sampled.
                 DeclareTracerReads( block, inputs.Tracer, kS, TracerPointClamp::Skip );
             },
             dispatch( Spatial, reservoirGroups ) );

        graph.AddPass(
             "ReSTIRGI: Resolve", kAsyncPass,
             [&]( RDG::PassBuilder& builder )
             {
                 using R    = ReSTIRGIResolveShaderParameters;
                 auto block = bindings( builder, Resolve );
                 common( block );
                 gbufferNormal( block );
                 block.Storage( R::RadianceCacheParamsUB, inputs.RadianceCache.Params, kR )
                      .Storage( R::RadianceCacheIndirectionUB, inputs.RadianceCache.Indirection, kR )
                      .Storage( R::RadianceCacheProbesUB, inputs.RadianceCache.Probes, kR )
                      .Sampler( P::LinearClampSampler, RDG::SamplerDesc::LinearClamp() )
                      .Texture( R::GBufferShadingWord, inputs.GBufferShadingWord, kS, all )
                      .Texture( R::SpatialHit, spatial[History::ReservoirHit], kS, all )
                      .Texture( R::SpatialRadiance, spatial[History::ReservoirRadiance], kS, all )
                      .Texture( R::SpatialState, spatial[History::ReservoirState], kS, all )
                      .Texture( R::RadianceCacheIrradianceTexture, inputs.RadianceCache.Irradiance, kS, all )
                      .Texture( R::RadianceCacheDepthTexture, inputs.RadianceCache.Depth, kS, all )
                      .Storage( R::ReSTIRDiffuseOut, outputs.Diffuse, kW )
                      .Storage( R::ReSTIRSpecularOut, outputs.Specular, kW )
                      .Storage( R::ReSTIRBackfaceOut, outputs.Backface, kW );
             },
             dispatch( Resolve, pixelGroups ) );
        return BOOLSUCCESS;
    }
} // namespace Desert::Graphic::GlobalIllumination
