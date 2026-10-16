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

#include <Common/Core/Math/Rounding.hpp>

#include <format>
#include <span>
#include <string>

namespace Desert::Graphic::GlobalIllumination
{
    namespace
    {
        using Common::Math::DivideRoundUp;

        constexpr std::array<std::string_view, 4> kScreenProbeShaderNames{
             "ScreenProbePlace", "ScreenProbeTrace", "ScreenProbeFilter", "ScreenProbeIntegrate" };

        constexpr std::array<std::string_view, ScreenProbeLayout::kLevels> kPlaceNames{
             "ScreenProbes: Place Uniform", "ScreenProbes: Place Adaptive 8", "ScreenProbes: Place Adaptive 4" };

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
            m_Shaders[program] = shaders->GetByName( std::string( kScreenProbeShaderNames[program] ) );
            if ( !m_Shaders[program] )
                return Common::MakeFormattedError<bool>( "screen probes: shader {} not found",
                                                         kScreenProbeShaderNames[program] );
            auto pipeline = ComputePipeline::Create(
                 { .Shader = m_Shaders[program], .DebugName = std::string( kScreenProbeShaderNames[program] ) } );
            if ( !pipeline )
                return Common::MakeFormattedError<bool>( "screen probes: {}: {}", kScreenProbeShaderNames[program],
                                                         pipeline.GetError() );
            m_Pipelines[program] = pipeline.GetValue();
        }
        return BOOLSUCCESS;
    }

    Common::BoolResultStr ScreenProbeGather::EnsureTraceProgram( const Pipeline& pipeline )
    {
        if ( m_Shaders[Trace] && m_Pipelines[Trace] && pipeline == m_TraceProgramFor )
            return BOOLSUCCESS;
        const std::string name( kScreenProbeShaderNames[Trace] );
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
        for ( uint32_t i = 0; i < 2; ++i )
        {
            m_Diffuse[i]  = CreateTarget( std::format( "ScreenProbes.Diffuse{}", i ), extent.x, extent.y,
                                          ViewTargetFormats::kGIFinalGather );
            m_Specular[i] = CreateTarget( std::format( "ScreenProbes.Specular{}", i ), extent.x, extent.y,
                                          ViewTargetFormats::kGIFinalGather );
        }
        if ( !m_Traced || !m_Filtered || !m_Diffuse[0] || !m_Diffuse[1] || !m_Specular[0] || !m_Specular[1] )
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
        // The software tracer's near field marches the mesh SDFs (Common/GITraceMeshSDF.glslh, declared by that
        // permutation only); the hardware tracer's TLAS is its own.
        const bool meshSDFs = TracerReadsGlobalDistanceField( inputs.Tracer.Pipeline );
        if ( meshSDFs && !inputs.MeshSDFs )
            return Common::MakeError<ScreenProbeGatherFrame>(
                 "screen probes: the software tracer's mesh SDFs were not read this frame" );
        if ( !inputs.HZB.IsValid() || !inputs.SceneColorHistory.IsValid() )
            return Common::MakeError<ScreenProbeGatherFrame>(
                 "screen probes: the screen trace's HZB or scene colour history is missing (bind the system black "
                 "texture when the view has none)" );
        const ScreenProbeLayout layout = MakeScreenProbeLayout( inputs.Extent );
        if ( const auto images = EnsureImages( inputs.Extent, layout ); !images )
            return Common::MakeError<ScreenProbeGatherFrame>( images.GetError() );

        const uint32_t previous               = m_Current;
        m_Current                             = 1u - m_Current;
        const RDG::TextureRef traced          = import( m_Traced, "ScreenProbes.Traced" );
        const RDG::TextureRef filtered        = import( m_Filtered, "ScreenProbes.Filtered" );
        const RDG::TextureRef diffuse         = import( m_Diffuse[m_Current], "ScreenProbes.Diffuse" );
        const RDG::TextureRef specular        = import( m_Specular[m_Current], "ScreenProbes.Specular" );
        const RDG::TextureRef diffuseHistory  = import( m_Diffuse[previous], "ScreenProbes.DiffuseHistory" );
        const RDG::TextureRef specularHistory = import( m_Specular[previous], "ScreenProbes.SpecularHistory" );

        ScreenProbeParamsPayload params;
        params.PrevViewProjection = inputs.PrevViewProjection;
        params.PrevCamera         = glm::vec4( inputs.PrevCameraPosition, 0.0f );
        params.Grid               = glm::uvec4( layout.Grid, layout.UniformProbes, layout.AdaptiveProbes );
        params.Atlas =
             glm::uvec4( ScreenProbeLayout::kAtlasProbesRow, ++m_FrameIndex, m_HistoryValid ? 1u : 0u, 0u );
        m_HistoryValid = true;
        const uint32_t traceFlags = ( inputs.SceneColorHistoryReadable ? ScreenProbeLayout::kTraceScreen : 0u ) |
                                    ( meshSDFs ? ScreenProbeLayout::kTraceMeshSDFs : 0u );
        params.Trace =
             glm::vec4( ScreenProbeLayout::kMeshSDFTraceDistance, ScreenProbeLayout::kMeshSDFEndDistanceFromCamera,
                        static_cast<float>( traceFlags ), 0.0f );

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
            block.Texture( "u_GBufferDepth", inputs.GBufferDepth, kS, all )
                 .Sampler( "u_PointClampSampler", RDG::SamplerDesc::PointClamp() )
                 .Texture( "u_GBufferB", inputs.GBufferB, kS, all );
        };

        // Place: level 0 (uniform), then the two adaptive levels, each reading the coarser ones.
        for ( uint32_t level = 0; level < ScreenProbeLayout::kLevels; ++level )
        {
            ComputePipeline* const pipeline = m_Pipelines[Place].get();
            const uint32_t         spacing  = ScreenProbeLayout::kDownsample >> level;
            const glm::uvec2       groups( DivideRoundUp( DivideRoundUp( inputs.Extent.x, spacing ), 8u ),
                                           DivideRoundUp( DivideRoundUp( inputs.Extent.y, spacing ), 8u ) );
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
                     block.Texture( "u_EnvSpecularTex", inputs.EnvSpecular, kS, all )
                          .Sampler( "u_LinearClampSampler", RDG::SamplerDesc::LinearClamp() )
                          .Texture( "u_GBufferDepth", inputs.GBufferDepth, kS, all )
                          .Sampler( "u_PointClampSampler", RDG::SamplerDesc::PointClamp() )
                          .Texture( "u_GBufferA", inputs.GBufferA, kS, all )
                          .Texture( "u_GBufferEmissive", inputs.GBufferEmissive, kS, all )
                          .Texture( "u_RadianceCacheIrradiance", inputs.RadianceCache.Irradiance, kS, all )
                          .Texture( "u_RadianceCacheDepth", inputs.RadianceCache.Depth, kS, all )
                          .Texture( "u_RadianceCacheRadianceAtlas", inputs.RadianceCache.Radiance, kS, all )
                          .Storage( "u_ScreenProbeTraced", traced, kW )
                          .Texture( "u_HZB", inputs.HZB, kS, all )
                          .Texture( "u_SceneColorHistory", inputs.SceneColorHistory, kS, all );
                     if ( meshSDFs )
                     {
                         const DistanceField::GlobalDistanceField::MeshSDFScene& mesh = *inputs.MeshSDFs;
                         // The brick atlas reads through the tracer's shared repeat state (GITraceMeshSDF.glslh).
                         block.Texture( "u_DFBrickAtlas", mesh.BrickAtlas, kS, all )
                              .Sampler( "u_LinearRepeatSampler", RDG::SamplerDesc::LinearRepeat() )
                              .Storage( "DFAssetData", mesh.AssetData, kR )
                              .Storage( "DFIndirection", mesh.Indirection, kR )
                              .Storage( "DFObjects", mesh.Objects, kR )
                              .Storage( "GIMeshSDFObjectRowsUB", mesh.ObjectRows, kR )
                              .Storage( "GIMeshSDFCulledUB", mesh.Culled, kR );
                     }
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
                          .Texture( "u_ScreenProbeTraced", traced, kS, all )
                          .Sampler( "u_PointClampSampler", RDG::SamplerDesc::PointClamp() )
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
            const glm::uvec2 groups( DivideRoundUp( inputs.Extent.x, 8u ), DivideRoundUp( inputs.Extent.y, 8u ) );
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
                          .Texture( "u_ScreenProbeFiltered", filtered, kS, all )
                          .Sampler( "u_LinearClampSampler", RDG::SamplerDesc::LinearClamp() )
                          .Texture( "u_ScreenProbeDiffuseHistory", diffuseHistory, kS, all )
                          .Texture( "u_ScreenProbeSpecularHistory", specularHistory, kS, all )
                          .Storage( "u_ScreenProbeDiffuseOut", diffuse, kW )
                          .Storage( "u_ScreenProbeSpecularOut", specular, kW );
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
