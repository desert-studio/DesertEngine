// Ported from UE 5.5 Engine/Source/Runtime/Renderer/Private/Lumen/LumenScreenProbeGather.cpp (the pass sequence:
// uniform placement, adaptive placement per level, trace, filter + irradiance, integrate, temporal) and UE 5.8
// LumenScreenProbeFiltering.cpp FilterScreenProbes (the spatial filter's passes and its disocclusion input),
// adapted as described in ScreenProbeGather.hpp.
#include <Engine/Graphic/GlobalIllumination/ScreenProbeGather.hpp>

#include <Engine/Graphic/Image.hpp>
#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/View/SceneViewState.hpp>
#include <Engine/Graphic/ViewTargetFormats.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <Common/Core/Math/Rounding.hpp>

#include <algorithm>
#include <format>
#include <limits>
#include <span>
#include <string>

namespace Desert::Graphic::GlobalIllumination
{
    namespace DFShared
    {
#include <Common/DistanceFieldShared.glslh>
    } // namespace DFShared

    namespace
    {
        using Common::Math::DivideRoundUp;

        constexpr std::array<std::string_view, 11> kScreenProbeShaderNames{
             "ScreenProbePlace",         "ScreenProbeShortRangeAO", "ScreenProbeTraceScreen",
             "ScreenProbeCompactTraces", "ScreenProbeTrace",        "ScreenProbeTrace",
             "ScreenProbeDisocclusion",  "ScreenProbeSpatialFilter", "ScreenProbeFilter",
             "ScreenProbeIntegrate",     "ScreenProbeTemporal" };

        // UE r.Lumen.ScreenProbeGather.SpatialFilterNumPasses.
        constexpr uint32_t kSpatialFilterPasses = 3;

        // The defines of the world legs' two permutations of ScreenProbeTrace (besides TracerVariant's).
        constexpr std::string_view kTraceNearFieldDefine = "SP_TRACE_NEAR_FIELD";
        constexpr std::string_view kTraceFarFieldDefine  = "SP_TRACE_FAR_FIELD";

        // ScreenProbeCompactArgsUB: x = groups, y = z = 1 (a dispatch's three counts; w unused); the allocator: 0.
        constexpr std::array<uint32_t, 4> kCompactArgsReset{ 0u, 1u, 1u, 0u };
        constexpr uint32_t                kCompactAllocatorReset = 0u;

        // The near field's end along a ray (ScreenProbeCompactTraces' u_MaxDistance for the near-field list): the
        // software tracer's mesh SDFs trace up to DF_MESH_SDF_TRACE_DISTANCE (GITraceNearField); a ray the screen
        // took past it has nothing left there.
        [[nodiscard]] float NearFieldEnd( const Tracer tracer )
        {
            return tracer == Tracer::SoftwareDistanceField ? DFShared::DF_MESH_SDF_TRACE_DISTANCE
                                                           : std::numeric_limits<float>::max();
        }

        struct CompactPush
        {
            float    MaxDistance;
            uint32_t FarField;
        };

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
            if ( program == TraceNearField || program == TraceFarField )
                continue; // tracer permutations, EnsureTraceProgram
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
        if ( m_Pipelines[TraceNearField] && m_Pipelines[TraceFarField] && pipeline == m_TraceProgramFor )
            return BOOLSUCCESS;
        for ( const auto& [program, leg] : { std::pair{ TraceNearField, kTraceNearFieldDefine },
                                             std::pair{ TraceFarField, kTraceFarFieldDefine } } )
        {
            const std::string   name( kScreenProbeShaderNames[program] );
            Core::ShaderVariant variant = TracerVariant( pipeline );
            variant.Defines.emplace_back( leg );
            auto shader = Runtime::ResourceRegistry::GetShaderService()->AcquireVariant( name, variant );
            if ( !shader )
                return Common::MakeFormattedError<bool>( "screen probes: shader {} ({}, {}, {}) did not compile",
                                                         name, TracerDefine( pipeline.Tracer ),
                                                         HitLightingDefine( pipeline.Hits ), leg );
            auto compute = ComputePipeline::Create(
                 { .Shader    = shader,
                   .DebugName = std::format( "{} ({}, {})", name, TracerDefine( pipeline.Tracer ), leg ) } );
            if ( !compute )
                return Common::MakeFormattedError<bool>( "screen probes: {} ({}): {}", name, leg,
                                                         compute.GetError() );
            m_Shaders[program]   = std::move( shader );
            m_Pipelines[program] = compute.GetValue();
        }
        m_TraceProgramFor = pipeline;
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
        if ( !m_Traced || !m_Filtered || !m_ShortRangeAO )
            return Common::MakeError<bool>( "screen probes: an image could not be created" );
        m_Extent    = extent;
        m_AtlasRows = layout.AtlasRows;
        return BOOLSUCCESS;
    }

    Common::ResultStr<ScreenProbeGatherFrame>
    ScreenProbeGather::AddPasses( RDG::Builder& graph, const WorldRadianceCache::ImportImage& import,
                                  const ScreenProbeGatherInputs& inputs, ScreenProbeGatherHistory& history )
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
        if ( !inputs.HZB.IsValid() || !inputs.SceneColorHistory.IsValid() )
            return Common::MakeError<ScreenProbeGatherFrame>(
                 "screen probes: the screen trace's HZB or scene colour history is missing (bind the system black "
                 "texture when the view has none)" );
        if ( !inputs.Velocity.IsValid() || !inputs.GBufferShadingWord.IsValid() || !inputs.Black.IsValid() )
            return Common::MakeError<ScreenProbeGatherFrame>(
                 "screen probes: the view's velocity, the G-buffer's shading word or the system black texture is "
                 "missing (the temporal filter reprojects by the velocity, tells foliage by the shading word and "
                 "binds black for a history the view does not hold)" );
        const ScreenProbeLayout layout = MakeScreenProbeLayout( inputs.Extent, inputs.AdaptiveProbePercent );
        if ( const auto images = EnsureImages( inputs.Extent, layout ); !images )
            return Common::MakeError<ScreenProbeGatherFrame>( images.GetError() );

        const RDG::TextureRef traced       = import( m_Traced, "ScreenProbes.Traced" );
        const RDG::TextureRef filtered     = import( m_Filtered, "ScreenProbes.Filtered" );
        const RDG::TextureRef shortRangeAO = import( m_ShortRangeAO, "ScreenProbes.ShortRangeAO" );
        // The integrate's output: this frame's lighting, which the temporal filter blends with the view's history
        // (or the gather's result as it is when the denoiser is skipped).
        using History = ScreenProbeGatherHistory;
        const ViewExtent extent{ .Width = inputs.Extent.x, .Height = inputs.Extent.y };
        const auto       integrated = [&]( const History::Texture texture, const char* name )
        { return graph.CreateTexture( History::Desc( texture, extent ), name ); };
        const RDG::TextureRef diffuse  = integrated( History::DiffuseIndirect, "ScreenProbes.Diffuse" );
        const RDG::TextureRef specular = integrated( History::RoughSpecularIndirect, "ScreenProbes.Specular" );
        const RDG::TextureRef backface = integrated( History::BackfaceDiffuseIndirect, "ScreenProbes.Backface" );

        // The view's history set (UE FScreenProbeGatherTemporalState), read only when the view's previous frame is a
        // previous frame of this one: not after a cut, a teleport or a resize (ViewFrame::HistoryValid).
        History::Refs previous{};
        if ( inputs.Denoise )
            previous = history.RegisterPrevious( graph, extent );
        const bool historyReadable =
             inputs.Denoise && inputs.ViewHistoryValid &&
             std::ranges::all_of( previous, []( const RDG::TextureRef& texture ) { return texture.IsValid(); } );
        const auto historyOrBlack = [&]( const History::Texture texture )
        { return historyReadable ? previous[texture] : inputs.Black; };

        ScreenProbeParamsPayload params;
        // The temporal filter's previous world position of a pixel (its NDC.xyz minus the velocity).
        params.PrevInvViewProjection = glm::inverse( inputs.PrevViewProjection );

        params.PrevViewProjection = inputs.PrevViewProjection;
        params.PrevCamera         = glm::vec4( inputs.PrevCameraPosition, 0.0f );
        params.Jitter             = glm::vec4( inputs.JitterNdc, inputs.PrevJitterNdc );
        params.Grid               = glm::uvec4( layout.Grid, layout.UniformProbes, layout.AdaptiveProbes );
        const glm::uvec2 jitter   = ScreenProbeJitter( ++m_FrameIndex );
        const uint32_t   flags    = ( historyReadable ? kScreenProbeHistoryValid : 0u ) |
                               ( inputs.ShortRangeAO ? kScreenProbeShortRangeAO : 0u ) |
                               ( inputs.SceneColorHistoryReadable ? kScreenProbeScreenTrace : 0u );
        params.Atlas =
             glm::uvec4( ScreenProbeLayout::kAtlasProbesRow, m_FrameIndex, flags, jitter.x | ( jitter.y << 16u ) );

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
        const RDG::BufferRef backfaceIrradiance =
             graph.CreateBuffer( RDG::BufferDesc{ layout.Probes() * 16ull }, "ScreenProbes.BackfaceIrradiance" );
        // The fast update's chain (UE ScreenProbeWorldSpeed -> the trace hit's moving bit -> ScreenProbeMoving ->
        // LightIsMoving): each probe's world speed (the placement), and the share of its rays that hit something
        // moving relative to it (the filter, from the trace hit buffer).
        const RDG::BufferRef speed = graph.CreateBuffer(
             RDG::BufferDesc{ layout.Probes() * static_cast<uint64_t>( sizeof( float ) ) }, "ScreenProbes.Speed" );
        const RDG::BufferRef moving = graph.CreateBuffer(
             RDG::BufferDesc{ layout.Probes() * static_cast<uint64_t>( sizeof( float ) ) }, "ScreenProbes.Moving" );

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
                          .Storage( "ScreenProbeSpeedUB", speed, kW )
                          .Sampled( "Velocity", inputs.Velocity, kS, all, RDG::SamplerDesc::PointClamp() )
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
        // ScreenSpaceShortRangeAOCS). Off by scalability, the integrate reads the shading normal unoccluded (its
        // flag) and the composite applies SSAO to the gather instead.
        if ( inputs.ShortRangeAO )
        {
            ComputePipeline* const pipeline = m_Pipelines[ShortRangeAO].get();
            const glm::uvec2 groups( DivideRoundUp( inputs.Extent.x, 8u ), DivideRoundUp( inputs.Extent.y, 8u ) );
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

        // TRACE (UE LumenScreenProbeTracing.cpp TraceScreenProbes): the screen; the rays it left open compacted;
        // the tracer's near field over them; every ray still without a hit compacted; the far field, the radiance
        // cache and the sky over those. Each ray's state between the legs: the trace hit buffer
        // (Common/ScreenProbeTracing.glslh).
        const uint32_t       probeGroups = layout.Probes();
        constexpr uint32_t   kTexels     = ScreenProbeLayout::kTracedTexels * ScreenProbeLayout::kTracedTexels;
        const uint64_t       rayBytes    = static_cast<uint64_t>( probeGroups ) * kTexels * sizeof( uint32_t );
        const RDG::BufferRef traceHit = graph.CreateBuffer( RDG::BufferDesc{ rayBytes }, "ScreenProbes.TraceHit" );
        {
            ComputePipeline* const pipeline = m_Pipelines[TraceScreen].get();
            graph.AddPass(
                 "ScreenProbes: Trace Screen", RDG::PassFlags::Compute,
                 [&]( RDG::PassBuilder& builder )
                 {
                     auto block = builder.Bindings( m_Layouts[TraceScreen].Get( m_Shaders[TraceScreen] ),
                                                    Renderer::GetPipelineRouteFill( *pipeline ) );
                     block.Storage( "ScreenProbeParamsUB", paramsBuffer, kR )
                          .Storage( "ScreenProbeIndexUB", index, kR )
                          .Storage( "ScreenProbesUB", probes, kR )
                          .Storage( "GIFrameUB", inputs.GIFrame, kR )
                          .Storage( "RadianceCacheParamsUB", inputs.RadianceCache.Params, kR )
                          .Storage( "ScreenProbeTraceHitUB", traceHit, kW )
                          .Storage( "u_ScreenProbeTraced", traced, kW )
                          .Sampled( "u_HZB", inputs.HZB, kS, all, RDG::SamplerDesc::PointClamp() )
                          .Sampled( "u_SceneColorHistory", inputs.SceneColorHistory, kS, all,
                                    RDG::SamplerDesc::LinearClamp() )
                          .Storage( "ScreenProbeSpeedUB", speed, kR )
                          .Sampled( "Velocity", inputs.Velocity, kS, all, RDG::SamplerDesc::PointClamp() );
                 },
                 [pipeline, probeGroups]( RDG::PassContext& context ) -> Common::BoolResultStr
                 {
                     RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                     return Renderer::DispatchCompute( bindings, *pipeline, probeGroups, 1u, 1u );
                 } );
        }

        // One compaction: the open rays' list and its dispatch arguments (ScreenProbeCompactTraces.shader).
        struct Compacted
        {
            RDG::BufferRef Args;      // the world leg's dispatch (IndirectArgs)
            RDG::BufferRef Allocator; // the rays listed
            RDG::BufferRef Texels;
        };
        const auto compact = [&]( const std::string_view name, const CompactPush push ) -> Compacted
        {
            const Compacted list{ graph.CreateBuffer( RDG::BufferDesc{ sizeof( kCompactArgsReset ) },
                                                      std::format( "ScreenProbes.CompactArgs ({})", name ) ),
                                  graph.CreateBuffer( RDG::BufferDesc{ sizeof( kCompactAllocatorReset ) },
                                                      std::format( "ScreenProbes.CompactAllocator ({})", name ) ),
                                  graph.CreateBuffer( RDG::BufferDesc{ rayBytes },
                                                      std::format( "ScreenProbes.CompactTexels ({})", name ) ) };
            graph.QueueBufferUpload( list.Args, std::as_bytes( std::span( kCompactArgsReset ) ) );
            graph.QueueBufferUpload( list.Allocator, std::as_bytes( std::span( &kCompactAllocatorReset, 1 ) ) );
            ComputePipeline* const pipeline = m_Pipelines[CompactTraces].get();
            graph.AddPass(
                 std::format( "ScreenProbes: Compact Traces ({})", name ), RDG::PassFlags::Compute,
                 [&]( RDG::PassBuilder& builder )
                 {
                     auto block = builder.Bindings( m_Layouts[CompactTraces].Get( m_Shaders[CompactTraces] ),
                                                    Renderer::GetPipelineRouteFill( *pipeline ) );
                     block.Storage( "ScreenProbeParamsUB", paramsBuffer, kR )
                          .Storage( "ScreenProbeIndexUB", index, kR )
                          .Storage( "ScreenProbesUB", probes, kR )
                          .Storage( "ScreenProbeTraceHitUB", traceHit, kR )
                          .Storage( "ScreenProbeCompactArgsUB", list.Args, kW )
                          .Storage( "ScreenProbeCompactAllocatorUB", list.Allocator, kW )
                          .Storage( "ScreenProbeCompactTexelsUB", list.Texels, kW )
                          .PushConstantBytes( static_cast<uint32_t>( sizeof( CompactPush ) ) );
                 },
                 [pipeline, probeGroups, push]( RDG::PassContext& context ) -> Common::BoolResultStr
                 {
                     RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                     bindings.PushConstants( &push, static_cast<uint32_t>( sizeof( push ) ) );
                     return Renderer::DispatchCompute( bindings, *pipeline, probeGroups, 1u, 1u );
                 } );
            return list;
        };

        // One world leg over a compacted list, one thread per ray (ScreenProbeTrace.shader's permutation).
        const auto traceWorld = [&]( const Program program, const std::string_view name, const Compacted list )
        {
            ComputePipeline* const pipeline = m_Pipelines[program].get();
            graph.AddPass(
                 name, RDG::PassFlags::Compute,
                 [&]( RDG::PassBuilder& builder )
                 {
                     auto block = builder.Bindings( m_Layouts[program].Get( m_Shaders[program] ),
                                                    Renderer::GetPipelineRouteFill( *pipeline ) );
                     builder.Read( list.Args, RDG::Access::IndirectArgs );
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
                          .Storage( "RadianceCacheProbesUB", inputs.RadianceCache.Probes, kR )
                          .Storage( "ScreenProbeTraceHitUB", traceHit, kW )
                          .Storage( "ScreenProbeCompactAllocatorUB", list.Allocator, kR )
                          .Storage( "ScreenProbeCompactTexelsUB", list.Texels, kR )
                          .Storage( "ScreenProbeSpeedUB", speed, kR );
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
                 [pipeline, args = list.Args]( RDG::PassContext& context ) -> Common::BoolResultStr
                 {
                     RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                     return Renderer::DispatchComputeIndirect( bindings, *pipeline, args, 0u );
                 } );
        };

        traceWorld( TraceNearField, "ScreenProbes: Trace Near Field",
                    compact( "near field", CompactPush{ NearFieldEnd( inputs.Tracer.Pipeline.Tracer ), 0u } ) );
        traceWorld( TraceFarField, "ScreenProbes: Trace Far Field",
                    compact( "far field", CompactPush{ std::numeric_limits<float>::max(), 1u } ) );

        // SPATIAL FILTER (UE FilterScreenProbes, SpatialFilterNumPasses): the disocclusion of every probe's tile,
        // then three passes across neighbouring probes, each reading the last one's radiance.
        RDG::TextureRef radiance = traced;
        if ( inputs.Denoise )
        {
            const RDG::BufferRef disocclusion = graph.CreateBuffer(
                 RDG::BufferDesc{ layout.Probes() * static_cast<uint64_t>( sizeof( float ) ) },
                 "ScreenProbes.Disocclusion" );
            {
                ComputePipeline* const pipeline = m_Pipelines[Disocclusion].get();
                graph.AddPass(
                     "ScreenProbes: Disocclusion", RDG::PassFlags::Compute,
                     [&]( RDG::PassBuilder& builder )
                     {
                         auto block = builder.Bindings( m_Layouts[Disocclusion].Get( m_Shaders[Disocclusion] ),
                                                        Renderer::GetPipelineRouteFill( *pipeline ) );
                         block.Storage( "ScreenProbeParamsUB", paramsBuffer, kR )
                              .Storage( "ScreenProbeIndexUB", index, kR )
                              .Storage( "ScreenProbesUB", probes, kR )
                              .Storage( "GIFrameUB", inputs.GIFrame, kR )
                              .Sampled( "Velocity", inputs.Velocity, kS, all, RDG::SamplerDesc::PointClamp() )
                              .Sampled( "GBufferShadingWord", inputs.GBufferShadingWord, kS, all,
                                        RDG::SamplerDesc::PointClamp() )
                              .Sampled( "DiffuseIndirectHistory", historyOrBlack( History::DiffuseIndirect ), kS,
                                        all, RDG::SamplerDesc::PointClamp() )
                              .Sampled( "HistoryGuide", historyOrBlack( History::Guide ), kS, all,
                                        RDG::SamplerDesc::PointClamp() )
                              .Storage( "ScreenProbeDisocclusionUB", disocclusion, kW );
                         gbuf( block );
                     },
                     [pipeline, probeGroups]( RDG::PassContext& context ) -> Common::BoolResultStr
                     {
                         RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                         return Renderer::DispatchCompute( bindings, *pipeline, probeGroups, 1u, 1u );
                     } );
            }
            constexpr uint32_t     res = ScreenProbeLayout::kTracedTexels;
            const RDG::TextureDesc atlasDesc{
                 .Size   = { .Width = ScreenProbeLayout::kAtlasProbesRow * res, .Height = layout.AtlasRows * res },
                 .Format = Core::Formats::ImageFormat::RGBA16F };
            for ( uint32_t pass = 0; pass < kSpatialFilterPasses; ++pass )
            {
                ComputePipeline* const pipeline = m_Pipelines[SpatialFilter].get();
                const RDG::TextureRef  input    = radiance;
                const RDG::TextureRef  output   = graph.CreateTexture( atlasDesc, "ScreenProbes.SpatialFiltered" );
                graph.AddPass(
                     "ScreenProbes: Spatial Filter", RDG::PassFlags::Compute,
                     [&]( RDG::PassBuilder& builder )
                     {
                         builder
                              .Bindings( m_Layouts[SpatialFilter].Get( m_Shaders[SpatialFilter] ),
                                         Renderer::GetPipelineRouteFill( *pipeline ) )
                              .Storage( "ScreenProbeParamsUB", paramsBuffer, kR )
                              .Storage( "ScreenProbeIndexUB", index, kR )
                              .Storage( "ScreenProbesUB", probes, kR )
                              .Storage( "ScreenProbeDisocclusionUB", disocclusion, kR )
                              .Sampled( "ScreenProbeRadiance", input, kS, all, RDG::SamplerDesc::PointClamp() )
                              .Storage( "FilteredScreenProbeRadiance", output, kW );
                     },
                     [pipeline, probeGroups]( RDG::PassContext& context ) -> Common::BoolResultStr
                     {
                         RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                         return Renderer::DispatchCompute( bindings, *pipeline, probeGroups, 1u, 1u );
                     } );
                radiance = output;
            }
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
                          .Sampled( "ScreenProbeRadiance", radiance, kS, all, RDG::SamplerDesc::PointClamp() )
                          .Storage( "u_ScreenProbeFiltered", filtered, kW )
                          .Storage( "ScreenProbeSHUB", sh, kW )
                          .Storage( "ScreenProbeBackfaceUB", backfaceIrradiance, kW )
                          .Storage( "ScreenProbeTraceHitUB", traceHit, kR )
                          .Storage( "ScreenProbeMovingUB", moving, kW )
                          .Storage( "RadianceCacheParamsUB", inputs.RadianceCache.Params, kR )
                          .Storage( "RadianceCacheIndirectionUB", inputs.RadianceCache.Indirection, kR )
                          .Storage( "RadianceCacheProbesUB", inputs.RadianceCache.Probes, kR )
                          .Sampled( "u_RadianceCacheIrradiance", inputs.RadianceCache.Irradiance, kS, all,
                                    RDG::SamplerDesc::LinearClamp() )
                          .Sampled( "u_RadianceCacheDepth", inputs.RadianceCache.Depth, kS, all,
                                    RDG::SamplerDesc::LinearClamp() );
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
                          .Sampled( "u_ScreenProbeFiltered", filtered, kS, all, RDG::SamplerDesc::LinearClamp() )
                          .Storage( "u_ScreenProbeDiffuseOut", diffuse, kW )
                          .Storage( "u_ScreenProbeSpecularOut", specular, kW )
                          .Storage( "ScreenProbeBackfaceUB", backfaceIrradiance, kR )
                          .Storage( "u_ScreenProbeBackfaceOut", backface, kW )
                          .Storage( "ScreenProbeMovingUB", moving, kR )
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

        if ( !inputs.Denoise )
            return Common::MakeSuccess( ScreenProbeGatherFrame{ .Diffuse      = diffuse,
                                                                .Specular     = specular,
                                                                .Backface     = backface,
                                                                .ShortRangeAO = inputs.ShortRangeAO } );

        // TEMPORAL FILTER (UE ScreenProbeTemporalReprojectionCS): this frame's lighting blended with the view's
        // history into the view's new history set, which is also what the composite reads.
        const History::Refs current = history.CreateCurrent( graph, extent );
        {
            ComputePipeline* const pipeline = m_Pipelines[Temporal].get();
            const glm::uvec2 groups( DivideRoundUp( inputs.Extent.x, 8u ), DivideRoundUp( inputs.Extent.y, 8u ) );
            history.SetWriterPass( graph.GetPassCount() );
            graph.AddPass(
                 "ScreenProbes: Temporal", RDG::PassFlags::Compute,
                 [&]( RDG::PassBuilder& builder )
                 {
                     using H    = History;
                     auto block = builder.Bindings( m_Layouts[Temporal].Get( m_Shaders[Temporal] ),
                                                    Renderer::GetPipelineRouteFill( *pipeline ) );
                     block.Storage( "ScreenProbeParamsUB", paramsBuffer, kR )
                          .Storage( "GIFrameUB", inputs.GIFrame, kR )
                          .Sampled( "Velocity", inputs.Velocity, kS, all, RDG::SamplerDesc::PointClamp() )
                          .Sampled( "GBufferShadingWord", inputs.GBufferShadingWord, kS, all,
                                    RDG::SamplerDesc::PointClamp() )
                          .Sampled( "DiffuseIndirect", diffuse, kS, all, RDG::SamplerDesc::PointClamp() )
                          .Sampled( "RoughSpecularIndirect", specular, kS, all, RDG::SamplerDesc::PointClamp() )
                          .Sampled( "BackfaceDiffuseIndirect", backface, kS, all, RDG::SamplerDesc::PointClamp() )
                          .Sampled( "DiffuseIndirectHistory", historyOrBlack( H::DiffuseIndirect ), kS, all,
                                    RDG::SamplerDesc::PointClamp() )
                          .Sampled( "RoughSpecularIndirectHistory", historyOrBlack( H::RoughSpecularIndirect ), kS, all,
                                    RDG::SamplerDesc::PointClamp() )
                          .Sampled( "BackfaceDiffuseIndirectHistory", historyOrBlack( H::BackfaceDiffuseIndirect ), kS,
                                    all, RDG::SamplerDesc::PointClamp() )
                          .Sampled( "HistoryGuide", historyOrBlack( H::Guide ), kS, all,
                                    RDG::SamplerDesc::PointClamp() )
                          .Storage( "NewHistoryDiffuseIndirect", current[H::DiffuseIndirect], kW )
                          .Storage( "NewHistoryRoughSpecularIndirect", current[H::RoughSpecularIndirect], kW )
                          .Storage( "NewHistoryBackfaceDiffuseIndirect", current[H::BackfaceDiffuseIndirect], kW )
                          .Storage( "NewHistoryGuide", current[H::Guide], kW );
                     gbuf( block );
                 },
                 [pipeline, groups]( RDG::PassContext& context ) -> Common::BoolResultStr
                 {
                     RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                     return Renderer::DispatchCompute( bindings, *pipeline, groups.x, groups.y, 1u );
                 } );
        }
        return Common::MakeSuccess(
             ScreenProbeGatherFrame{ .Diffuse      = current[History::DiffuseIndirect],
                                     .Specular     = current[History::RoughSpecularIndirect],
                                     .Backface     = current[History::BackfaceDiffuseIndirect],
                                     .ShortRangeAO = inputs.ShortRangeAO } );
    }
} // namespace Desert::Graphic::GlobalIllumination
