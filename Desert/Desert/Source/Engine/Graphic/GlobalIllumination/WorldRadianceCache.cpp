// Ported from UE 5.5 Engine/Source/Runtime/Renderer/Private/Lumen/LumenRadianceCache.cpp (the update's pass
// sequence: mark, update used probes, allocate, select the max priority bucket, trace from probes, filter),
// adapted as described in WorldRadianceCache.hpp.
#include <Engine/Graphic/GlobalIllumination/WorldRadianceCache.hpp>

#include <Engine/Graphic/Image.hpp>
#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/ShaderResources/StorageBuffer.hpp>

#include <Common/Core/Math/Rounding.hpp>

#include <algorithm>
#include <cstring>
#include <format>
#include <span>

namespace Desert::Graphic::GlobalIllumination
{
    namespace
    {
        using Common::Math::DivideRoundUp;

        constexpr std::array<std::string_view, 6> kRadianceCacheShaderNames{
             "RadianceCacheMark",   "RadianceCacheUpdate", "RadianceCacheAllocate",
             "RadianceCacheSelect", "RadianceCacheTrace",  "RadianceCacheFilter" };

        // std430 — must match `PushConstant RadianceCachePush` in Common/RadianceCacheState.glslh.
        struct RadianceCachePush
        {
            uint32_t FrameIndex   = 0;
            uint32_t Reset        = 0;
            uint32_t MaxProbes    = 0;
            uint32_t UnusedFrames = RadianceCache::kProbeUnusedFramesBeforeFree;
            uint32_t Width        = 0;
            uint32_t Height       = 0;
            float    History      = RadianceCache::kHistoryWeight;
            float    DirtyHistory = RadianceCache::kDirtyHistoryWeight;
        };
        static_assert( sizeof( RadianceCachePush ) == 32 );

        // The schedule: the priority histogram, the traced count, the cut bucket's taken count, the trace list.
        constexpr uint32_t ScheduleEntries( const RadianceCache::RadianceCacheInputs& shape )
        {
            return RadianceCache::kPriorityBuckets + 2u + shape.ProbesPerFrame;
        }

        std::shared_ptr<Image2D> CreateAtlas( const std::string_view tag, const uint32_t side,
                                              const Core::Formats::ImageFormat format )
        {
            const Core::Formats::Image2DSpecification spec{
                 .Tag        = std::string( tag ),
                 .Width      = side,
                 .Height     = side,
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

    WorldRadianceCache::WorldRadianceCache()  = default;
    WorldRadianceCache::~WorldRadianceCache() = default;

    Common::BoolResultStr WorldRadianceCache::Initialize()
    {
        const auto shaders = Runtime::ResourceRegistry::GetShaderService();
        for ( uint32_t pass = 0; pass < PassCount; ++pass )
        {
            if ( pass == Trace )
                continue; // a tracer permutation, EnsureTraceProgram
            m_Shaders[pass] = shaders->GetByName( std::string( kRadianceCacheShaderNames[pass] ) );
            if ( !m_Shaders[pass] )
                return Common::MakeFormattedError<bool>( "radiance cache: shader {} not found",
                                                         kRadianceCacheShaderNames[pass] );
            auto pipeline = ComputePipeline::Create(
                 { .Shader = m_Shaders[pass], .DebugName = std::string( kRadianceCacheShaderNames[pass] ) } );
            if ( !pipeline )
                return Common::MakeFormattedError<bool>( "radiance cache: {}: {}", kRadianceCacheShaderNames[pass],
                                                         pipeline.GetError() );
            m_Pipelines[pass] = pipeline.GetValue();
        }
        return BOOLSUCCESS;
    }

    Common::BoolResultStr WorldRadianceCache::EnsureTraceProgram( const Pipeline& pipeline )
    {
        if ( m_Shaders[Trace] && m_Pipelines[Trace] && pipeline == m_TraceProgramFor )
            return BOOLSUCCESS;
        const std::string name( kRadianceCacheShaderNames[Trace] );
        auto              shader =
             Runtime::ResourceRegistry::GetShaderService()->AcquireVariant( name, TracerVariant( pipeline ) );
        if ( !shader )
            return Common::MakeFormattedError<bool>( "radiance cache: shader {} ({}, {}) did not compile", name,
                                                     TracerDefine( pipeline.Tracer ),
                                                     HitLightingDefine( pipeline.Hits ) );
        auto compute = ComputePipeline::Create(
             { .Shader = shader, .DebugName = std::format( "{} ({})", name, TracerDefine( pipeline.Tracer ) ) } );
        if ( !compute )
            return Common::MakeFormattedError<bool>( "radiance cache: {}: {}", name, compute.GetError() );
        m_Shaders[Trace]   = std::move( shader );
        m_Pipelines[Trace] = compute.GetValue();
        m_TraceProgramFor  = pipeline;
        return BOOLSUCCESS;
    }

    Common::BoolResultStr WorldRadianceCache::EnsureResources( const RadianceCache::RadianceCacheInputs& shape )
    {
        if ( m_HasShape && shape == m_Shape )
            return BOOLSUCCESS;
        const uint32_t entries   = RadianceCache::IndirectionEntries( shape );
        const uint32_t maxProbes = RadianceCache::MaxProbes( shape );
        m_Indirection =
             ShaderResources::StorageBuffer::Create( "RadianceCache.Indirection", entries * 4u, 1, true );
        m_LastUsed = ShaderResources::StorageBuffer::Create( "RadianceCache.LastUsed", entries * 4u, 2, true );
        m_Probes   = ShaderResources::StorageBuffer::Create(
             "RadianceCache.Probes",
             maxProbes * static_cast<uint32_t>( sizeof( RadianceCache::RadianceCacheProbePayload ) ), 3, true );
        m_FreeList =
             ShaderResources::StorageBuffer::Create( "RadianceCache.FreeList", ( 1u + maxProbes ) * 4u, 4, true );
        if ( !m_Indirection || !m_LastUsed || !m_Probes || !m_FreeList )
            return Common::MakeError<bool>( "radiance cache: a state buffer could not be created" );

        const uint32_t probes = shape.ProbeAtlasResolutionInProbes;
        m_RadianceAtlas       = CreateAtlas( "RadianceCache.Radiance", probes * shape.FinalProbeResolution(),
                                             RadianceCache::kRadianceAtlasFormat );
        m_IrradianceAtlas =
             CreateAtlas( "RadianceCache.Irradiance", probes * ( shape.IrradianceProbeResolution + 2u ),
                          RadianceCache::kIrradianceAtlasFormat );
        m_DepthAtlas = CreateAtlas( "RadianceCache.Depth", probes * ( RadianceCache::kDepthProbeResolution + 2u ),
                                    RadianceCache::kDepthAtlasFormat );
        if ( !m_RadianceAtlas || !m_IrradianceAtlas || !m_DepthAtlas )
            return Common::MakeError<bool>( "radiance cache: an atlas could not be created" );
        m_Shape    = shape;
        m_HasShape = true;
        m_Reset    = true;
        return BOOLSUCCESS;
    }

    Common::ResultStr<RadianceCacheFrame>
    WorldRadianceCache::AddUpdatePasses( RDG::Builder& graph, const ImportImage& import,
                                         const RadianceCache::RadianceCacheInputs& shape,
                                         const RadianceCacheUpdateInputs&          inputs )
    {
        if ( shape.ProbeResolution == 0 || shape.ProbesPerFrame == 0 )
            return Common::MakeError<RadianceCacheFrame>(
                 "radiance cache: the quality gives no probe resolution or budget" );
        if ( const auto ensured = EnsureResources( shape ); !ensured )
            return Common::MakeError<RadianceCacheFrame>( ensured.GetError() );
        if ( const std::string_view missing = TracerFrameMissing( inputs.Tracer ); !missing.empty() )
            return Common::MakeFormattedError<RadianceCacheFrame>( "radiance cache: {}", missing );
        // A hit reads the cards first (GIHitRadiance) unless the hardware tracer lights every hit in place.
        const bool readsCards = inputs.Tracer.Pipeline.Hits == HitLighting::SurfaceCache;
        if ( const std::string_view missing = SurfaceCacheFrameMissing( inputs.SurfaceCache );
             readsCards && !missing.empty() )
            return Common::MakeFormattedError<RadianceCacheFrame>( "radiance cache: {}", missing );
        if ( const auto traced = EnsureTraceProgram( inputs.Tracer.Pipeline ); !traced )
            return Common::MakeError<RadianceCacheFrame>( traced.GetError() );

        for ( const auto& [buffer, external, name] :
              { std::tuple{ &m_Indirection, &m_IndirectionImport, "RadianceCache.Indirection" },
                std::tuple{ &m_LastUsed, &m_LastUsedImport, "RadianceCache.LastUsed" },
                std::tuple{ &m_Probes, &m_ProbesImport, "RadianceCache.Probes" },
                std::tuple{ &m_FreeList, &m_FreeListImport, "RadianceCache.FreeList" } } )
            if ( const auto imported = Renderer::ImportBuffer( *buffer, *external ); !imported )
                return Common::MakeFormattedError<RadianceCacheFrame>( "radiance cache: {}: {}", name,
                                                                       imported.GetError() );
        const RDG::BufferRef indirection =
             graph.RegisterExternal( m_IndirectionImport, "RadianceCache.Indirection" );
        const RDG::BufferRef  lastUsed   = graph.RegisterExternal( m_LastUsedImport, "RadianceCache.LastUsed" );
        const RDG::BufferRef  probes     = graph.RegisterExternal( m_ProbesImport, "RadianceCache.Probes" );
        const RDG::BufferRef  freeList   = graph.RegisterExternal( m_FreeListImport, "RadianceCache.FreeList" );
        const RDG::TextureRef radiance   = import( m_RadianceAtlas, "RadianceCache.Radiance" );
        const RDG::TextureRef irradiance = import( m_IrradianceAtlas, "RadianceCache.Irradiance" );
        const RDG::TextureRef depth      = import( m_DepthAtlas, "RadianceCache.Depth" );

        const auto clipmaps = RadianceCache::MakeClipmaps( inputs.CameraPosition, shape );
        const RadianceCache::RadianceCacheParamsPayload paramsPayload =
             RadianceCache::MakeRadianceCacheParams( clipmaps, shape );
        const RDG::BufferRef params =
             graph.CreateBuffer( RDG::BufferDesc{ sizeof( paramsPayload ) }, "RadianceCache.Params" );
        graph.QueueBufferUpload( params, std::as_bytes( std::span( &paramsPayload, 1 ) ) );
        const RDG::BufferRef frame = inputs.GIFrame;
        // The extra readers' points (std430: a uvec4 header, then one vec4 per point; one zero vec4 when none, a
        // storage block is never empty).
        std::vector<glm::vec4> markPoints( 1u + std::max<size_t>( inputs.MarkPoints.size(), 1u ),
                                           glm::vec4( 0.0f ) );
        const auto             markCount = static_cast<uint32_t>( inputs.MarkPoints.size() );
        std::memcpy( markPoints.data(), &markCount, sizeof( markCount ) );
        for ( size_t i = 0; i < inputs.MarkPoints.size(); ++i )
            markPoints[1u + i] = glm::vec4( inputs.MarkPoints[i], 1.0f );
        const RDG::BufferRef markPointsBuffer = graph.CreateBuffer(
             RDG::BufferDesc{ markPoints.size() * sizeof( glm::vec4 ) }, "RadianceCache.MarkPoints" );
        graph.QueueBufferUpload( markPointsBuffer, std::as_bytes( std::span( markPoints ) ) );
        const RDG::BufferRef schedule =
             graph.CreateBuffer( RDG::BufferDesc{ ScheduleEntries( shape ) * 4ull }, "RadianceCache.Schedule" );

        RadianceCachePush push;
        push.FrameIndex = ++m_FrameIndex;
        push.Reset      = m_Reset ? 1u : 0u;
        push.MaxProbes  = RadianceCache::MaxProbes( shape );
        push.Width      = inputs.Extent.x;
        push.Height     = inputs.Extent.y;
        m_Reset         = false;

        const uint32_t   grid      = shape.GridResolution;
        const uint32_t   clipCount = shape.Clipmaps;
        const glm::uvec3 entryGroups( DivideRoundUp( grid * clipCount, 4u ), DivideRoundUp( grid, 4u ),
                                      DivideRoundUp( grid, 4u ) );
        const glm::uvec3 groups[PassCount] = {
             glm::uvec3( DivideRoundUp( DivideRoundUp( inputs.Extent.x, 2u ), 8u ),
                         DivideRoundUp( DivideRoundUp( inputs.Extent.y, 2u ), 8u ), 1u ),
             entryGroups,
             entryGroups,
             glm::uvec3( DivideRoundUp( push.MaxProbes, 64u ), 1u, 1u ),
             glm::uvec3( shape.ProbesPerFrame, 1u, 1u ),
             glm::uvec3( shape.ProbesPerFrame, 1u, 1u ) };
        constexpr std::array<std::string_view, PassCount> kPassNames{
             "RadianceCache: Mark",   "RadianceCache: Update", "RadianceCache: Allocate",
             "RadianceCache: Select", "RadianceCache: Trace",  "RadianceCache: Filter" };

        constexpr RDG::Access kRW = RDG::Access::StorageWrite;
        constexpr RDG::Access kR  = RDG::Access::StorageRead;
        constexpr RDG::Access kS  = RDG::Access::SampledCompute;
        const auto            all = RDG::SubresourceRange::All();
        for ( uint32_t pass = 0; pass < PassCount; ++pass )
        {
            ComputePipeline* const pipeline = m_Pipelines[pass].get();
            const glm::uvec3       count    = groups[pass];
            graph.AddPass(
                 kPassNames[pass], RDG::PassFlags::Compute,
                 [&]( RDG::PassBuilder& builder )
                 {
                     auto block = builder.Bindings( m_Layouts[pass].Get( m_Shaders[pass] ),
                                                    Renderer::GetPipelineRouteFill( *pipeline ) );
                     block.Storage( "RadianceCacheParamsUB", params, kR )
                          .Storage( "RadianceCacheIndirectionUB", indirection, kRW )
                          .Storage( "RadianceCacheLastUsedUB", lastUsed, kRW )
                          .Storage( "RadianceCacheProbesUB", probes, kRW )
                          .Storage( "RadianceCacheFreeListUB", freeList, kRW )
                          .Storage( "RadianceCacheScheduleUB", schedule, kRW )
                          .PushConstantBytes( static_cast<uint32_t>( sizeof( RadianceCachePush ) ) );
                     if ( pass == Mark || pass == Allocate || pass == Trace )
                         block.Storage( "GIFrameUB", frame, kR );
                     if ( pass == Mark )
                         block.Texture( "u_GBufferDepth", inputs.GBufferDepth, kS, all )
.Sampler( "u_PointClampSampler", RDG::SamplerDesc::PointClamp() )
                              .Texture( "u_GBufferB", inputs.GBufferB, kS, all )
.Sampler( "u_PointClampSampler", RDG::SamplerDesc::PointClamp() )
                              .Storage( "RadianceCacheMarkPointsUB", markPointsBuffer, kR );
                     if ( pass == Allocate || pass == Trace )
                         block.Storage( "PointLightsUB", inputs.PointLights, kR )
                              .Storage( "SpotLightsUB", inputs.SpotLights, kR )
                              .Storage( "LightGridParamsUB", inputs.WorldGridParams, kR )
                              .Storage( "LightGridWorldCellsUB", inputs.WorldGridCells, kR );
                     if ( pass == Trace )
                     {
                         DeclareTracerReads( block, inputs.Tracer );
                         if ( readsCards )
                             DeclareSurfaceCacheReads( block, inputs.SurfaceCache );
                         block.Texture( "u_EnvSpecularTex", inputs.EnvSpecular, kS, all )
                              .Sampler( "u_LinearClampSampler", RDG::SamplerDesc::LinearClamp() )
                              .Texture( "u_GBufferDepth", inputs.GBufferDepth, kS, all )
                              .Sampler( "u_PointClampSampler", RDG::SamplerDesc::PointClamp() )
                              .Texture( "u_GBufferA", inputs.GBufferA, kS, all )
                              .Texture( "u_GBufferEmissive", inputs.GBufferEmissive, kS, all )
                              .Texture( "u_RadianceCacheIrradiance", irradiance, kS, all )
                              .Texture( "u_RadianceCacheDepth", depth, kS, all )
                              .Storage( "u_RadianceCacheRadiance", radiance, kRW );
                     }
                     if ( pass == Filter )
                         block.Storage( "u_RadianceCacheRadiance", radiance, kRW )
                              .Storage( "u_RadianceCacheIrradianceOut", irradiance, kRW )
                              .Storage( "u_RadianceCacheDepthOut", depth, kRW );
                 },
                 [pipeline, push, count]( RDG::PassContext& context ) -> Common::BoolResultStr
                 {
                     RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                     bindings.PushConstants( &push, static_cast<uint32_t>( sizeof( push ) ) );
                     return Renderer::DispatchCompute( bindings, *pipeline, count.x, count.y, count.z );
                 } );
        }

        return Common::MakeSuccess( RadianceCacheFrame{ .Params      = params,
                                                        .Indirection = indirection,
                                                        .Probes      = probes,
                                                        .Irradiance  = irradiance,
                                                        .Depth       = depth,
                                                        .Radiance    = radiance } );
    }
} // namespace Desert::Graphic::GlobalIllumination
