// Ported from UE 5.5 Engine/Source/Runtime/Renderer/Private/Lumen/LumenRadianceCache.cpp (the update's pass
// sequence: mark, update used probes, allocate, select the max priority bucket, trace from probes, filter),
// adapted as described in WorldRadianceCache.hpp.
#include <Engine/Graphic/GlobalIllumination/WorldRadianceCache.hpp>

#include <Engine/Core/EngineContext.hpp>
#include <Engine/Graphic/Image.hpp>
#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/ShaderResources/StorageBuffer.hpp>

#include <Common/Core/Logger.hpp>
#include <Common/Core/Math/Rounding.hpp>

#include <algorithm>
#include <cstring>
#include <format>
#include <span>

namespace
{
    // The slot names this pass binds, spelled once (RDGShaderParameters.hpp).
    struct WorldRadianceCacheShaderParameters
    {
        DESERT_SHADER_PARAMETER( StorageBuffer, RadianceCacheParamsUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, RadianceCacheIndirectionUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, RadianceCacheLastUsedUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, RadianceCacheProbesUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, RadianceCacheFreeListUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, RadianceCacheScheduleUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, GIFrameUB );
        DESERT_SHADER_PARAMETER( Texture, GBufferDepthTexture );
        DESERT_SHADER_PARAMETER( Sampler, PointClampSampler );
        DESERT_SHADER_PARAMETER( Texture, GBufferBTexture );
        DESERT_SHADER_PARAMETER( StorageBuffer, PointLightsUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, SpotLightsUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, LightGridParamsUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, LightGridWorldCellsUB );
        DESERT_SHADER_PARAMETER( Texture, EnvironmentSpecularTexture );
        DESERT_SHADER_PARAMETER( Sampler, LinearClampSampler );
        DESERT_SHADER_PARAMETER( Texture, GBufferATexture );
        DESERT_SHADER_PARAMETER( Texture, GBufferEmissiveTexture );
        DESERT_SHADER_PARAMETER( Texture, RadianceCacheIrradianceTexture );
        DESERT_SHADER_PARAMETER( Texture, RadianceCacheDepthTexture );
        DESERT_SHADER_PARAMETER( StorageTexture, RadianceCacheRadianceTexture );
        DESERT_SHADER_PARAMETER( StorageTexture, RadianceCacheIrradianceOutTexture );
        DESERT_SHADER_PARAMETER( StorageTexture, RadianceCacheDepthOutTexture );
        DESERT_SHADER_PARAMETER( StorageBuffer, RadianceCacheMarkPointsUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, RadianceCacheUpdateCountsUB );
    };
} // namespace

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
            uint32_t LightingFrame = 0; // the last full update's FrameIndex (WorldRadianceCache::m_LightingFrame)
            uint32_t MarkFallback  = 0; // 1: Mark also stamps the clipmap past each pixel's coarse end
        };
        static_assert( sizeof( RadianceCachePush ) == 40 );

        // The update's event counters in the schedule header (RC_STAT_WORDS, Common/RadianceCacheState.glslh).
        constexpr uint32_t kScheduleCounters = 6;
        // A Trace workgroup's texels: one TRACE TILE of a probe, one ray per thread (RadianceCacheTrace's
        // LocalSize 8x8; UE TraceFromProbes over probe trace tiles), so the budget's probes fill the GPU instead
        // of leaving one group per probe to walk its res^2 rays in series.
        constexpr uint32_t kTraceTileTexels = 64;
        // RadianceCacheMark's box key: clamp(base, -1, grid - 1) + 1 in 9 bits an axis, the clipmap in 3 bits
        // under bit 31 (the key never reaches RC_MARK_NONE).
        constexpr uint32_t kMarkKeyMaxGrid     = 511;
        constexpr uint32_t kMarkKeyMaxClipmaps = 7;
        static_assert( RadianceCache::kGridResolution <= kMarkKeyMaxGrid );
        static_assert( RadianceCache::kMaxClipmaps <= kMarkKeyMaxClipmaps );
        // The "[RadianceCache] traced X of budget Y, full update Z" line: once per this many updates of a view.
        constexpr uint32_t kCountsLogFrames = 60;
        // The schedule: the priority histogram, the traced count, the cut bucket's taken count, the update's event
        // counters, the trace list.
        constexpr uint32_t ScheduleEntries( const uint32_t traced )
        {
            return RadianceCache::kPriorityBuckets + 2u + kScheduleCounters + traced;
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

    std::vector<Core::CookedProgramVariant> WorldRadianceCache::ProgramVariants()
    {
        return TracerProgramVariants( kRadianceCacheShaderNames[Trace] );
    }

    Common::BoolResultStr WorldRadianceCache::EnsureTraceProgram( const Pipeline& pipeline )
    {
        if ( m_Shaders[Trace] && m_Pipelines[Trace] && pipeline == m_TraceProgramFor )
            return BOOLSUCCESS;
        const std::string name( kRadianceCacheShaderNames[Trace] );
        auto              acquired =
             Runtime::ResourceRegistry::GetShaderService()->AcquireVariant( name, TracerVariant( pipeline ) );
        if ( !acquired )
            return Common::MakeFormattedError<bool>( "radiance cache: shader {} ({}, {}) was refused: {}", name,
                                                     TracerDefine( pipeline.Tracer ),
                                                     HitLightingDefine( pipeline.Hits ), acquired.GetError() );
        auto shader  = acquired.ExtractValue();
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
        // Two stamps per entry, the coverage's and the fallback's (RadianceCacheStampIndex of
        // Common/RadianceCacheState.glslh).
        m_LastUsed = ShaderResources::StorageBuffer::Create( "RadianceCache.LastUsed", entries * 8u, 2, true );
        m_Probes   = ShaderResources::StorageBuffer::Create(
             "RadianceCache.Probes",
             maxProbes * static_cast<uint32_t>( sizeof( RadianceCache::RadianceCacheProbePayload ) ), 3, true );
        m_FreeList =
             // The count, the slots, and the two failed-allocation words (RadianceCacheState.glslh).
             ShaderResources::StorageBuffer::Create( "RadianceCache.FreeList", ( 3u + maxProbes ) * 4u, 4, true );
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
        if ( shape.GridResolution > kMarkKeyMaxGrid || shape.Clipmaps > kMarkKeyMaxClipmaps )
            return Common::MakeFormattedError<RadianceCacheFrame>(
                 "radiance cache: grid {} x {} clipmaps exceeds the Mark key ({} x {})", shape.GridResolution,
                 shape.Clipmaps, kMarkKeyMaxGrid, kMarkKeyMaxClipmaps );
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

        // THE FULL UPDATE (UE r.Lumen.RadianceCache.ForceFullUpdate): the directional lights changed significantly
        // since the last one, so every probe this frame uses is traced in it with no history - the cache's cells
        // would otherwise hold the old sun for as many frames as the budget takes to reach them, blended at
        // HistoryWeight after that (a straight-edged patch fading over ~10 s, GRASS-SETTLE). The frame after a
        // reset has no probes to trace yet: it only records the lighting. The probes it does not use this frame
        // keep the old sun until a view reads them: push.LightingFrame drops their history on that trace.
        const bool fullUpdate = !m_Reset && m_LitBy && InvalidatesLightingHistory( *m_LitBy, inputs.Lighting );
        if ( m_Reset || fullUpdate || !m_LitBy )
            m_LitBy = inputs.Lighting;
        const uint32_t traced = fullUpdate ? RadianceCache::MaxProbes( shape ) : shape.ProbesPerFrame;

        const auto clipmaps = RadianceCache::MakeClipmaps( inputs.CameraPosition, shape );
        RadianceCache::RadianceCacheParamsPayload paramsPayload =
             RadianceCache::MakeRadianceCacheParams( clipmaps, shape );
        paramsPayload.Atlas.z = traced; // the Select pass's budget
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
             graph.CreateBuffer( RDG::BufferDesc{ ScheduleEntries( traced ) * 4ull }, "RadianceCache.Schedule" );
        // This frame's update in counts (RadianceCacheSelect.shader writes every word), read back for the profile.
        const RDG::BufferRef updateCounts = graph.CreateBuffer(
             RDG::BufferDesc{ RadianceCacheUpdateCounts::kWords * 4ull }, "RadianceCache.UpdateCounts" );

        RadianceCachePush push;
        push.FrameIndex = ++m_FrameIndex;
        push.Reset      = m_Reset ? 1u : 0u;
        push.MaxProbes  = RadianceCache::MaxProbes( shape );
        push.Width      = inputs.Extent.x;
        push.Height     = inputs.Extent.y;
        // Every probe last traced before the full update - this frame's and the ones it did not use, traced when
        // a view next reads them - is traced again without history (RadianceCacheTrace's "fresh").
        if ( fullUpdate )
            m_LightingFrame = push.FrameIndex;
        push.LightingFrame = m_LightingFrame;
        push.MarkFallback  = inputs.MarkRadianceFallback ? 1u : 0u;
        m_Reset            = false;

        const uint32_t   grid      = shape.GridResolution;
        const uint32_t   clipCount = shape.Clipmaps;
        const glm::uvec3 entryGroups( DivideRoundUp( grid * clipCount, 4u ), DivideRoundUp( grid, 4u ),
                                      DivideRoundUp( grid, 4u ) );
        // Trace: x the probe's place in the schedule (past its traced count a group ends), y its trace tile.
        const uint32_t traceTiles =
             DivideRoundUp( shape.ProbeResolution * shape.ProbeResolution, kTraceTileTexels );
        const glm::uvec3 groups[PassCount] = {
             // At least one group: Mark's first thread clears the schedule header Update and Allocate count into.
             glm::uvec3( std::max( 1u, DivideRoundUp( DivideRoundUp( inputs.Extent.x, 2u ), 8u ) ),
                         std::max( 1u, DivideRoundUp( DivideRoundUp( inputs.Extent.y, 2u ), 8u ) ), 1u ),
             entryGroups,
             entryGroups,
             glm::uvec3( DivideRoundUp( push.MaxProbes, 64u ), 1u, 1u ),
             glm::uvec3( traced, traceTiles, 1u ),
             glm::uvec3( traced, 1u, 1u ) };
        constexpr std::array<std::string_view, PassCount> kPassNames{
             "RadianceCache: Mark",   "RadianceCache: Update", "RadianceCache: Allocate",
             "RadianceCache: Select", "RadianceCache: Trace",  "RadianceCache: Filter" };

        constexpr RDG::Access kRW = RDG::Access::StorageWrite;
        constexpr RDG::Access kR  = RDG::Access::StorageRead;
        constexpr RDG::Access kS  = RDG::Access::SampledCompute;
        // ASYNC COMPUTE (UE Lumen radiance cache on the async pipe): the six nodes read and write graph resources
        // only (the cache's imports, the G-buffer depth / normal, the light uploads, the tracer, the surface
        // cache); the graph forks after their last graphics writer and joins at the first graphics reader of the
        // cache.
        const RDG::PassFlags  kAsyncPass = RDG::PassFlags::Compute | RDG::PassFlags::AsyncCompute;
        const auto            all = RDG::SubresourceRange::All();
        for ( uint32_t pass = 0; pass < PassCount; ++pass )
        {
            ComputePipeline* const pipeline = m_Pipelines[pass].get();
            const glm::uvec3       count    = groups[pass];
            graph.AddPass(
                 kPassNames[pass], kAsyncPass,
                 [&]( RDG::PassBuilder& builder )
                 {
                     auto block = builder.Bindings( m_Layouts[pass].Get( m_Shaders[pass] ),
                                                    Renderer::GetPipelineRouteFill( *pipeline ) );
                     block.Storage( WorldRadianceCacheShaderParameters::RadianceCacheParamsUB, params, kR )
                          .Storage( WorldRadianceCacheShaderParameters::RadianceCacheIndirectionUB, indirection,
                                    kRW )
                          .Storage( WorldRadianceCacheShaderParameters::RadianceCacheLastUsedUB, lastUsed, kRW )
                          .Storage( WorldRadianceCacheShaderParameters::RadianceCacheProbesUB, probes, kRW )
                          .Storage( WorldRadianceCacheShaderParameters::RadianceCacheFreeListUB, freeList, kRW )
                          .Storage( WorldRadianceCacheShaderParameters::RadianceCacheScheduleUB, schedule, kRW )
                          .PushConstantBytes( static_cast<uint32_t>( sizeof( RadianceCachePush ) ) );
                     if ( pass == Select )
                         block.Storage( WorldRadianceCacheShaderParameters::RadianceCacheUpdateCountsUB, updateCounts,
                                        kRW );
                     if ( pass == Mark || pass == Allocate || pass == Trace )
                         block.Storage( WorldRadianceCacheShaderParameters::GIFrameUB, frame, kR );
                     if ( pass == Mark )
                         block.Texture( WorldRadianceCacheShaderParameters::GBufferDepthTexture,
                                        inputs.GBufferDepth, kS, all )
                              .Sampler( WorldRadianceCacheShaderParameters::PointClampSampler,
                                        RDG::SamplerDesc::PointClamp() )
                              .Texture( WorldRadianceCacheShaderParameters::GBufferBTexture, inputs.GBufferB, kS,
                                        all )
                              .Sampler( WorldRadianceCacheShaderParameters::PointClampSampler,
                                        RDG::SamplerDesc::PointClamp() )
                              .Storage( WorldRadianceCacheShaderParameters::RadianceCacheMarkPointsUB,
                                        markPointsBuffer, kR );
                     if ( pass == Allocate || pass == Trace )
                         block.Storage( WorldRadianceCacheShaderParameters::PointLightsUB, inputs.PointLights, kR )
                              .Storage( WorldRadianceCacheShaderParameters::SpotLightsUB, inputs.SpotLights, kR )
                              .Storage( WorldRadianceCacheShaderParameters::LightGridParamsUB,
                                        inputs.WorldGridParams, kR )
                              .Storage( WorldRadianceCacheShaderParameters::LightGridWorldCellsUB,
                                        inputs.WorldGridCells, kR );
                     if ( pass == Trace )
                     {
                         DeclareTracerReads( block, inputs.Tracer );
                         if ( readsCards )
                             DeclareSurfaceCacheReads( block, inputs.SurfaceCache );
                         block.Texture( WorldRadianceCacheShaderParameters::EnvironmentSpecularTexture,
                                        inputs.EnvSpecular, kS, all )
                              .Sampler( WorldRadianceCacheShaderParameters::LinearClampSampler,
                                        RDG::SamplerDesc::LinearClamp() )
                              .Texture( WorldRadianceCacheShaderParameters::GBufferDepthTexture,
                                        inputs.GBufferDepth, kS, all )
                              .Sampler( WorldRadianceCacheShaderParameters::PointClampSampler,
                                        RDG::SamplerDesc::PointClamp() )
                              .Texture( WorldRadianceCacheShaderParameters::GBufferATexture, inputs.GBufferA, kS,
                                        all )
                              .Texture( WorldRadianceCacheShaderParameters::GBufferEmissiveTexture,
                                        inputs.GBufferEmissive, kS, all )
                              .Texture( WorldRadianceCacheShaderParameters::RadianceCacheIrradianceTexture,
                                        irradiance, kS, all )
                              .Texture( WorldRadianceCacheShaderParameters::RadianceCacheDepthTexture, depth, kS,
                                        all )
                              .Storage( WorldRadianceCacheShaderParameters::RadianceCacheRadianceTexture, radiance,
                                        kRW );
                     }
                     if ( pass == Filter )
                         block.Storage( WorldRadianceCacheShaderParameters::RadianceCacheRadianceTexture, radiance,
                                        kRW )
                              .Storage( WorldRadianceCacheShaderParameters::RadianceCacheIrradianceOutTexture,
                                        irradiance, kRW )
                              .Storage( WorldRadianceCacheShaderParameters::RadianceCacheDepthOutTexture, depth,
                                        kRW );
                 },
                 [pipeline, push, count]( RDG::PassContext& context ) -> Common::BoolResultStr
                 {
                     RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                     bindings.PushConstants( &push, static_cast<uint32_t>( sizeof( push ) ) );
                     return Renderer::DispatchCompute( bindings, *pipeline, count.x, count.y, count.z );
                 } );
        }

        m_UpdateCounts.AddFrame( graph, push.FrameIndex, updateCounts );
        // GPU-TURN-4: does the budget hold on a camera turn - the newest read-back frame's traced / budget, and
        // how many of the last kCountsLogFrames updates were full updates (each traces every used probe).
        if ( fullUpdate )
            ++m_FullUpdatesInWindow;
        if ( push.FrameIndex % kCountsLogFrames == 0u )
        {
            const RadianceCacheUpdateCounts& counts   = m_UpdateCounts.Last();
            const char* const                readBack = counts.Valid ? "" : ", none read back yet";
            LOG_INFO(
                 "[RadianceCache] traced {} of budget {}, full update {} (full updates of the last {} updates; "
                 "traced / budget of the newest frame read back{})",
                 counts.Traced, counts.Budget, m_FullUpdatesInWindow, kCountsLogFrames, readBack );
            m_FullUpdatesInWindow = 0;
        }

        return Common::MakeSuccess( RadianceCacheFrame{ .Params      = params,
                                                        .Indirection = indirection,
                                                        .Probes      = probes,
                                                        .Irradiance  = irradiance,
                                                        .Depth       = depth,
                                                        .Radiance    = radiance } );
    }

    void RadianceCacheUpdateCountsReadback::AddFrame( RDG::Builder& graph, const uint64_t frameIndex,
                                                      const RDG::BufferRef counts )
    {
        const uint32_t slots = std::max( 1u, EngineContext::GetInstance().GetMaxFramesInFlight() );
        if ( m_Slots.size() != slots )
        {
            m_Slots.clear();
            m_Slots.resize( slots );
        }
        RDG::ExternalBuffer& slot = m_Slots[static_cast<std::size_t>( frameIndex % slots )];
        // The words this slot was handed slots-in-flight frames ago: its fence has passed.
        if ( slot.Physical )
        {
            const std::span<const std::byte> bytes = slot.Physical->ReadForHost();
            std::array<uint32_t, RadianceCacheUpdateCounts::kWords> words{};
            if ( bytes.size() >= sizeof( words ) )
            {
                std::memcpy( words.data(), bytes.data(), sizeof( words ) );
                // RC_COUNT_* of Common/RadianceCacheState.glslh, in order.
                m_Last.Stamped       = words[0];
                m_Last.NeverTraced   = words[1];
                m_Last.Traced        = words[2];
                m_Last.Budget        = words[3];
                m_Last.Allocated     = words[4];
                m_Last.Failed        = words[5];
                m_Last.FreedStale    = words[6];
                m_Last.FreedUnused   = words[7];
                m_Last.FreedPressure = words[8];
                m_Last.Pressed       = words[9];
                m_Last.Live          = words[10];
                m_Last.MaxProbes     = words[11];
                for ( uint32_t clipmap = 0; clipmap < RadianceCache::kMaxClipmaps; ++clipmap )
                {
                    m_Last.NeverTracedByClipmap[clipmap] = words[12u + clipmap];
                    m_Last.FallbackByClipmap[clipmap]    = words[12u + RadianceCache::kMaxClipmaps + clipmap];
                }
                m_Last.Valid = true;
            }
        }
        slot = RDG::ExternalBuffer{};
        graph.Extract( counts, slot, RDG::Access::HostRead );
    }
} // namespace Desert::Graphic::GlobalIllumination
