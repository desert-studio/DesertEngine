// Ported from UE 5.5 Engine/Source/Runtime/Renderer/Private/Lumen/LumenScreenProbeGather.cpp (the pass sequence:
// uniform placement, adaptive placement per level, trace, filter + irradiance, integrate, temporal), adapted as
// described in ScreenProbeGather.hpp.
#include <Engine/Graphic/GlobalIllumination/ScreenProbeGather.hpp>

#include <Engine/Graphic/Image.hpp>
#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/ViewTargetFormats.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <format>
#include <span>
#include <string>

namespace Desert::Graphic::GlobalIllumination
{
    namespace
    {
        constexpr std::array<std::string_view, 5> kShaderNames{ "ScreenProbePlace", "ScreenProbeShortRangeAO",
                                                                "ScreenProbeTrace", "ScreenProbeFilter",
                                                                "ScreenProbeIntegrate" };

        constexpr std::array<std::string_view, ScreenProbeLayout::kLevels> kPlaceNames{
             "ScreenProbes: Place Uniform", "ScreenProbes: Place Adaptive 8", "ScreenProbes: Place Adaptive 4" };

        uint32_t DivideUp( const uint32_t value, const uint32_t by )
        {
            return ( value + by - 1u ) / by;
        }

        std::shared_ptr<Image2D>
        CreateTarget( const std::string_view tag, const uint32_t width, const uint32_t height,
                      const Core::Formats::ImageFormat format = Core::Formats::ImageFormat::RGBA16F )
        {
            const Core::Formats::Image2DSpecification spec{
                 .Tag        = std::string( tag ),
                 .Width      = width,
                 .Height     = height,
                 .Format     = format,
                 .Mips       = 1,
                 .Samples    = 1,
                 .Data       = Core::Formats::EmptyPixelData{},
                 .Usage      = Core::Formats::Image2DUsage::Image2D,
                 .Properties = Core::Formats::Storage | Core::Formats::Sample,
            };
            return Image2D::Create( spec );
        }
    } // namespace

    ScreenProbeGather::ScreenProbeGather()  = default;
    ScreenProbeGather::~ScreenProbeGather() = default;

    Common::BoolResultStr ScreenProbeGather::Initialize()
    {
        const auto shaders = Runtime::ResourceRegistry::GetShaderService();
        for ( uint32_t program = 0; program < ProgramCount; ++program )
        {
            if ( program == Trace )
                continue; // a tracer permutation, EnsureTraceProgram
            m_Shaders[program] = shaders->GetByName( std::string( kShaderNames[program] ) );
            if ( !m_Shaders[program] )
                return Common::MakeFormattedError<bool>( "screen probes: shader {} not found",
                                                         kShaderNames[program] );
            auto pipeline = ComputePipeline::Create(
                 { .Shader = m_Shaders[program], .DebugName = std::string( kShaderNames[program] ) } );
            if ( !pipeline )
                return Common::MakeFormattedError<bool>( "screen probes: {}: {}", kShaderNames[program],
                                                         pipeline.GetError() );
            m_Pipelines[program] = pipeline.GetValue();
        }
        return BOOLSUCCESS;
    }

    Common::BoolResultStr ScreenProbeGather::EnsureTraceProgram( const Pipeline& pipeline )
    {
        if ( m_Shaders[Trace] && m_Pipelines[Trace] && pipeline == m_TraceProgramFor )
            return BOOLSUCCESS;
        const std::string name( kShaderNames[Trace] );
        auto              shader =
             Runtime::ResourceRegistry::GetShaderService()->AcquireVariant( name, TracerVariant( pipeline ) );
        if ( !shader )
            return Common::MakeFormattedError<bool>( "screen probes: shader {} ({}, {}) did not compile", name,
                                                     TracerDefine( pipeline.Tracer ),
                                                     HitLightingDefine( pipeline.Hits ) );
        auto compute = ComputePipeline::Create(
             { .Shader = shader, .DebugName = std::format( "{} ({})", name, TracerDefine( pipeline.Tracer ) ) } );
        if ( !compute )
            return Common::MakeFormattedError<bool>( "screen probes: {}: {}", name, compute.GetError() );
        m_Shaders[Trace]   = std::move( shader );
        m_Pipelines[Trace] = compute.GetValue();
        m_TraceProgramFor  = pipeline;
        return BOOLSUCCESS;
    }

    Common::BoolResultStr ScreenProbeGather::EnsureImages( const glm::uvec2         extent,
                                                           const ScreenProbeLayout& layout )
    {
        if ( m_Traced && extent == m_Extent && layout.AtlasRows == m_AtlasRows )
            return BOOLSUCCESS;
        constexpr uint32_t row = ScreenProbeLayout::kAtlasProbesRow;
        constexpr uint32_t res = ScreenProbeLayout::kTracedTexels;
        m_Traced               = CreateTarget( "ScreenProbes.Traced", row * res, layout.AtlasRows * res );
        m_Filtered = CreateTarget( "ScreenProbes.Filtered", row * ( res + 2u ), layout.AtlasRows * ( res + 2u ) );
        m_ShortRangeAO         = CreateTarget( "ScreenProbes.ShortRangeAO", extent.x, extent.y );
        for ( uint32_t i = 0; i < 2; ++i )
        {
            m_Diffuse[i]  = CreateTarget( std::format( "ScreenProbes.Diffuse{}", i ), extent.x, extent.y,
                                          ViewTargetFormats::kGIFinalGather );
            m_Specular[i] = CreateTarget( std::format( "ScreenProbes.Specular{}", i ), extent.x, extent.y,
                                          ViewTargetFormats::kGIFinalGather );
        }
        if ( !m_Traced || !m_Filtered || !m_ShortRangeAO || !m_Diffuse[0] || !m_Diffuse[1] || !m_Specular[0] ||
             !m_Specular[1] )
            return Common::MakeError<bool>( "screen probes: an image could not be created" );
        m_Extent       = extent;
        m_AtlasRows    = layout.AtlasRows;
        m_HistoryValid = false;
        return BOOLSUCCESS;
    }

    Common::ResultStr<ScreenProbeGatherFrame>
    ScreenProbeGather::AddPasses( RDG::Builder& graph, const WorldRadianceCache::ImportImage& import,
                                  const ScreenProbeGatherInputs& inputs )
    {
        if ( inputs.Extent.x == 0 || inputs.Extent.y == 0 || inputs.Extent.x >= 4096 || inputs.Extent.y >= 4096 )
            return Common::MakeFormattedError<ScreenProbeGatherFrame>(
                 "screen probes: the G-buffer extent {}x{} is outside 1..4095 (a probe packs its pixel in 24 "
                 "bits)",
                 inputs.Extent.x, inputs.Extent.y );
        if ( !inputs.GIFrame.IsValid() || !inputs.RadianceCache.Radiance.IsValid() )
            return Common::MakeError<ScreenProbeGatherFrame>(
                 "screen probes: the GI frame record or the radiance cache of this frame is missing" );
        if ( const std::string_view missing = TracerFrameMissing( inputs.Tracer ); !missing.empty() )
            return Common::MakeFormattedError<ScreenProbeGatherFrame>( "screen probes: {}", missing );
        // A hit reads the cards first (GIHitRadiance) unless the hardware tracer lights every hit in place.
        const bool readsCards = inputs.Tracer.Pipeline.Hits == HitLighting::SurfaceCache;
        if ( const std::string_view missing = SurfaceCacheFrameMissing( inputs.SurfaceCache );
             readsCards && !missing.empty() )
            return Common::MakeFormattedError<ScreenProbeGatherFrame>( "screen probes: {}", missing );
        if ( const auto traced = EnsureTraceProgram( inputs.Tracer.Pipeline ); !traced )
            return Common::MakeError<ScreenProbeGatherFrame>( traced.GetError() );
        const ScreenProbeLayout layout = MakeScreenProbeLayout( inputs.Extent, inputs.AdaptiveProbePercent );
        if ( const auto images = EnsureImages( inputs.Extent, layout ); !images )
            return Common::MakeError<ScreenProbeGatherFrame>( images.GetError() );

        const uint32_t previous               = m_Current;
        m_Current                             = 1u - m_Current;
        const RDG::TextureRef traced          = import( m_Traced, "ScreenProbes.Traced" );
        const RDG::TextureRef filtered        = import( m_Filtered, "ScreenProbes.Filtered" );
        const RDG::TextureRef shortRangeAO    = import( m_ShortRangeAO, "ScreenProbes.ShortRangeAO" );
        const RDG::TextureRef diffuse         = import( m_Diffuse[m_Current], "ScreenProbes.Diffuse" );
        const RDG::TextureRef specular        = import( m_Specular[m_Current], "ScreenProbes.Specular" );
        const RDG::TextureRef diffuseHistory  = import( m_Diffuse[previous], "ScreenProbes.DiffuseHistory" );
        const RDG::TextureRef specularHistory = import( m_Specular[previous], "ScreenProbes.SpecularHistory" );

        ScreenProbeParamsPayload params;
        params.PrevViewProjection = inputs.PrevViewProjection;
        params.PrevCamera         = glm::vec4( inputs.PrevCameraPosition, 0.0f );
        params.Grid               = glm::uvec4( layout.Grid, layout.UniformProbes, layout.AdaptiveProbes );
        const glm::uvec2 jitter   = ScreenProbeJitter( ++m_FrameIndex );
        params.Atlas   = glm::uvec4( ScreenProbeLayout::kAtlasProbesRow, m_FrameIndex, m_HistoryValid ? 1u : 0u,
                                     jitter.x | ( jitter.y << 16u ) );
        m_HistoryValid = true;

        const RDG::BufferRef paramsBuffer =
             graph.CreateBuffer( RDG::BufferDesc{ sizeof( params ) }, "ScreenProbes.Params" );
        graph.QueueBufferUpload( paramsBuffer, std::as_bytes( std::span( &params, 1 ) ) );
        const RDG::BufferRef index =
             graph.CreateBuffer( RDG::BufferDesc{ layout.IndexEntries * 4ull }, "ScreenProbes.Index" );
        const RDG::BufferRef probes = graph.CreateBuffer(
             RDG::BufferDesc{ layout.Probes() * static_cast<uint64_t>( sizeof( ScreenProbePayload ) ) },
             "ScreenProbes.Probes" );
        const RDG::BufferRef sh = graph.CreateBuffer(
             RDG::BufferDesc{ layout.Probes() * ScreenProbeLayout::kSHCoefficients * 16ull }, "ScreenProbes.SH" );

        constexpr RDG::Access kW   = RDG::Access::StorageWrite;
        constexpr RDG::Access kR   = RDG::Access::StorageRead;
        constexpr RDG::Access kS   = RDG::Access::SampledCompute;
        const auto            all  = RDG::SubresourceRange::All();
        const auto            gbuf = [&]( RDG::BindingBlockBuilder& block )
        {
            block.Sampled( "u_GBufferDepth", inputs.GBufferDepth, kS, all, RDG::SamplerDesc::PointClamp() )
                 .Sampled( "u_GBufferB", inputs.GBufferB, kS, all, RDG::SamplerDesc::PointClamp() );
        };

        // Place: level 0 (uniform, and the tiles' adaptive counts reset), then the two adaptive levels, each
        // reading the probes the coarser ones placed.
        for ( uint32_t level = 0; level < ScreenProbeLayout::kLevels; ++level )
        {
            ComputePipeline* const pipeline = m_Pipelines[Place].get();
            const uint32_t         spacing  = ScreenProbeLayout::kDownsample >> level;
            const glm::uvec2       groups( DivideUp( DivideUp( inputs.Extent.x, spacing ), 8u ),
                                           DivideUp( DivideUp( inputs.Extent.y, spacing ), 8u ) );
            graph.AddPass(
                 kPlaceNames[level], RDG::PassFlags::Compute,
                 [&]( RDG::PassBuilder& builder )
                 {
                     auto block = builder.Bindings( m_Layouts[Place].Get( m_Shaders[Place] ),
                                                    Renderer::GetPipelineRouteFill( *pipeline ) );
                     block.Storage( "ScreenProbeParamsUB", paramsBuffer, kR )
                          .Storage( "ScreenProbeIndexUB", index, kW )
                          .Storage( "ScreenProbesUB", probes, kW )
                          .Storage( "GIFrameUB", inputs.GIFrame, kR )
                          .PushConstantBytes( static_cast<uint32_t>( sizeof( uint32_t ) ) );
                     gbuf( block );
                 },
                 [pipeline, level, groups]( RDG::PassContext& context ) -> Common::BoolResultStr
                 {
                     RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                     bindings.PushConstants( &level, static_cast<uint32_t>( sizeof( level ) ) );
                     return Renderer::DispatchCompute( bindings, *pipeline, groups.x, groups.y, 1u );
                 } );
        }

        // Short-range AO: the bent normal and AO of every pixel, from the scene depth (UE
        // ScreenSpaceShortRangeAOCS).
        {
            ComputePipeline* const pipeline = m_Pipelines[ShortRangeAO].get();
            const glm::uvec2       groups( DivideUp( inputs.Extent.x, 8u ), DivideUp( inputs.Extent.y, 8u ) );
            graph.AddPass(
                 "ScreenProbes: ShortRangeAO", RDG::PassFlags::Compute,
                 [&]( RDG::PassBuilder& builder )
                 {
                     auto block = builder.Bindings( m_Layouts[ShortRangeAO].Get( m_Shaders[ShortRangeAO] ),
                                                    Renderer::GetPipelineRouteFill( *pipeline ) );
                     block.Storage( "ScreenProbeParamsUB", paramsBuffer, kR )
                          .Storage( "GIFrameUB", inputs.GIFrame, kR )
                          .Storage( "u_ShortRangeAOOut", shortRangeAO, kW );
                     gbuf( block );
                 },
                 [pipeline, groups]( RDG::PassContext& context ) -> Common::BoolResultStr
                 {
                     RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                     return Renderer::DispatchCompute( bindings, *pipeline, groups.x, groups.y, 1u );
                 } );
        }

        const uint32_t probeGroups = layout.Probes();
        {
            ComputePipeline* const pipeline = m_Pipelines[Trace].get();
            graph.AddPass(
                 "ScreenProbes: Trace", RDG::PassFlags::Compute,
                 [&]( RDG::PassBuilder& builder )
                 {
                     auto block = builder.Bindings( m_Layouts[Trace].Get( m_Shaders[Trace] ),
                                                    Renderer::GetPipelineRouteFill( *pipeline ) );
                     block.Storage( "ScreenProbeParamsUB", paramsBuffer, kR )
                          .Storage( "ScreenProbeIndexUB", index, kR )
                          .Storage( "ScreenProbesUB", probes, kR )
                          .Storage( "GIFrameUB", inputs.GIFrame, kR )
                          .Storage( "PointLightsUB", inputs.PointLights, kR )
                          .Storage( "SpotLightsUB", inputs.SpotLights, kR )
                          .Storage( "LightGridParamsUB", inputs.WorldGridParams, kR )
                          .Storage( "LightGridWorldCellsUB", inputs.WorldGridCells, kR )
                          .Storage( "RadianceCacheParamsUB", inputs.RadianceCache.Params, kR )
                          .Storage( "RadianceCacheIndirectionUB", inputs.RadianceCache.Indirection, kR )
                          .Storage( "RadianceCacheProbesUB", inputs.RadianceCache.Probes, kR );
                     DeclareTracerReads( block, inputs.Tracer );
                     if ( readsCards )
                         DeclareSurfaceCacheReads( block, inputs.SurfaceCache );
                     block.Sampled( "u_EnvSpecularTex", inputs.EnvSpecular, kS, all,
                                    RDG::SamplerDesc::LinearClamp() )
                          .Sampled( "u_GBufferDepth", inputs.GBufferDepth, kS, all,
                                    RDG::SamplerDesc::PointClamp() )
                          .Sampled( "u_GBufferA", inputs.GBufferA, kS, all, RDG::SamplerDesc::PointClamp() )
                          .Sampled( "u_GBufferEmissive", inputs.GBufferEmissive, kS, all,
                                    RDG::SamplerDesc::PointClamp() )
                          .Sampled( "u_RadianceCacheIrradiance", inputs.RadianceCache.Irradiance, kS, all,
                                    RDG::SamplerDesc::LinearClamp() )
                          .Sampled( "u_RadianceCacheDepth", inputs.RadianceCache.Depth, kS, all,
                                    RDG::SamplerDesc::LinearClamp() )
                          .Sampled( "u_RadianceCacheRadianceAtlas", inputs.RadianceCache.Radiance, kS, all,
                                    RDG::SamplerDesc::LinearClamp() )
                          .Storage( "u_ScreenProbeTraced", traced, kW );
                 },
                 [pipeline, probeGroups]( RDG::PassContext& context ) -> Common::BoolResultStr
                 {
                     RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                     return Renderer::DispatchCompute( bindings, *pipeline, probeGroups, 1u, 1u );
                 } );
        }
        {
            ComputePipeline* const pipeline = m_Pipelines[Filter].get();
            graph.AddPass(
                 "ScreenProbes: Filter", RDG::PassFlags::Compute,
                 [&]( RDG::PassBuilder& builder )
                 {
                     auto block = builder.Bindings( m_Layouts[Filter].Get( m_Shaders[Filter] ),
                                                    Renderer::GetPipelineRouteFill( *pipeline ) );
                     block.Storage( "ScreenProbeParamsUB", paramsBuffer, kR )
                          .Storage( "ScreenProbeIndexUB", index, kR )
                          .Storage( "ScreenProbesUB", probes, kR )
                          .Storage( "GIFrameUB", inputs.GIFrame, kR )
                          .Sampled( "u_ScreenProbeTraced", traced, kS, all, RDG::SamplerDesc::PointClamp() )
                          .Storage( "u_ScreenProbeFiltered", filtered, kW )
                          .Storage( "ScreenProbeSHUB", sh, kW );
                 },
                 [pipeline, probeGroups]( RDG::PassContext& context ) -> Common::BoolResultStr
                 {
                     RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                     return Renderer::DispatchCompute( bindings, *pipeline, probeGroups, 1u, 1u );
                 } );
        }
        {
            ComputePipeline* const pipeline = m_Pipelines[Integrate].get();
            const glm::uvec2       groups( DivideUp( inputs.Extent.x, 8u ), DivideUp( inputs.Extent.y, 8u ) );
            graph.AddPass(
                 "ScreenProbes: Integrate", RDG::PassFlags::Compute,
                 [&]( RDG::PassBuilder& builder )
                 {
                     auto block = builder.Bindings( m_Layouts[Integrate].Get( m_Shaders[Integrate] ),
                                                    Renderer::GetPipelineRouteFill( *pipeline ) );
                     block.Storage( "ScreenProbeParamsUB", paramsBuffer, kR )
                          .Storage( "ScreenProbeIndexUB", index, kR )
                          .Storage( "ScreenProbesUB", probes, kR )
                          .Storage( "GIFrameUB", inputs.GIFrame, kR )
                          .Storage( "ScreenProbeSHUB", sh, kR )
                          .Sampled( "u_ScreenProbeFiltered", filtered, kS, all, RDG::SamplerDesc::LinearClamp() )
                          .Sampled( "u_ScreenProbeDiffuseHistory", diffuseHistory, kS, all,
                                    RDG::SamplerDesc::LinearClamp() )
                          .Sampled( "u_ScreenProbeSpecularHistory", specularHistory, kS, all,
                                    RDG::SamplerDesc::LinearClamp() )
                          .Storage( "u_ScreenProbeDiffuseOut", diffuse, kW )
                          .Storage( "u_ScreenProbeSpecularOut", specular, kW )
                          // A pixel no screen probe holds reads the world radiance cache in place.
                          .Storage( "RadianceCacheParamsUB", inputs.RadianceCache.Params, kR )
                          .Storage( "RadianceCacheIndirectionUB", inputs.RadianceCache.Indirection, kR )
                          .Storage( "RadianceCacheProbesUB", inputs.RadianceCache.Probes, kR )
                          .Sampled( "u_RadianceCacheIrradiance", inputs.RadianceCache.Irradiance, kS, all,
                                    RDG::SamplerDesc::LinearClamp() )
                          .Sampled( "u_RadianceCacheDepth", inputs.RadianceCache.Depth, kS, all,
                                    RDG::SamplerDesc::LinearClamp() )
                          .Sampled( "u_ShortRangeAO", shortRangeAO, kS, all, RDG::SamplerDesc::PointClamp() );
                     gbuf( block );
                 },
                 [pipeline, groups]( RDG::PassContext& context ) -> Common::BoolResultStr
                 {
                     RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                     return Renderer::DispatchCompute( bindings, *pipeline, groups.x, groups.y, 1u );
                 } );
        }

        return Common::MakeSuccess( ScreenProbeGatherFrame{ .Diffuse = diffuse, .Specular = specular } );
    }
} // namespace Desert::Graphic::GlobalIllumination
