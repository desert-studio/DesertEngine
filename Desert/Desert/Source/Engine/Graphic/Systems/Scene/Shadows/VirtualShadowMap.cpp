#include <Engine/Graphic/Systems/Scene/Shadows/VirtualShadowMap.hpp>

#include <Common/Core/Profiler.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Common/Core/Logger.hpp>
#include <Engine/Core/EngineContext.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/Systems/Scene/Shadows/RayTracedSunShadow.hpp>
#include <Engine/Graphic/ViewTargetFormats.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <vector>

namespace Desert::Graphic::System
{
    namespace
    {
        // One program per kernel, in VirtualShadowMapRenderer::Kernel order: the shader and the pass it adds.
        struct KernelName
        {
            const char* Shader;
            const char* Pass;
        };
        constexpr std::array<KernelName, 10> kKernelNames{ {
             { "VirtualShadowMapInvalidate", "VSM: Invalidate" },
             { "VirtualShadowMapInvalidateCasters", "VSM: Invalidate Casters" },
             { "VirtualShadowMapMark", "VSM: Mark" },
             { "VirtualShadowMapGather", "VSM: Gather Available Pages" },
             { "VirtualShadowMapAllocate", "VSM: Allocate" },
             { "VirtualShadowMapClearArgs", "VSM: Clear Args" },
             { "VirtualShadowMapClear", "VSM: Clear Pages" },
             { "VirtualShadowMapProject", "VSM: Project" },
             { "VirtualShadowMapDirtyMask", "VSM: Dirty Mask" },
             { "VirtualShadowMapCullCasters", "VSM: Cull Casters" },
        } };

        constexpr uint32_t kGroup64    = 64; // LocalSize(64, 1, 1) of the page and physical-page kernels
        constexpr uint32_t kScreenTile = 8;  // LocalSize(8, 8, 1) of Mark and Project

        constexpr uint32_t Groups( const uint32_t items, const uint32_t size )
        {
            return ( items + size - 1u ) / size;
        }

        template <class T>
        std::span<const std::byte> Bytes( const std::vector<T>& values )
        {
            return std::as_bytes( std::span<const T>( values ) );
        }
    } // namespace

    glm::mat4 VsmLightBasis( const glm::vec3& lightDirection )
    {
        const glm::vec3 direction = glm::normalize( lightDirection );
        const glm::vec3 up =
             glm::abs( direction.y ) > 0.99f ? glm::vec3( 0.0f, 0.0f, 1.0f ) : glm::vec3( 0.0f, 1.0f, 0.0f );
        return glm::lookAt( -direction, glm::vec3( 0.0f ), up );
    }

    VsmClipmapLevel MakeVsmClipmapLevel( const glm::mat4& lightBasis, const glm::vec3& camera,
                                         const uint32_t levelIndex )
    {
        VsmClipmapLevel level;
        level.Radius     = std::ldexp( 1.0f, kVsmClipmapFirstLevel + static_cast<int>( levelIndex ) );
        level.PageWorld  = 2.0f * level.Radius / static_cast<float>( kVsmLevelPages );
        level.DepthRange = 2.0f * level.Radius * kVsmClipmapZRangeScale;

        const glm::vec3  cameraLight = glm::vec3( lightBasis * glm::vec4( camera, 1.0f ) );
        const glm::ivec2 centrePage( static_cast<int>( std::floor( cameraLight.x / level.PageWorld ) ),
                                     static_cast<int>( std::floor( cameraLight.y / level.PageWorld ) ) );
        level.WindowOrigin = centrePage - glm::ivec2( static_cast<int>( kVsmLevelPages / 2 ) );

        // The depth centre moves in quarters of the half range: the camera stays at least a quarter range from
        // either end, and a level's cached depth survives every camera move inside a step.
        const float depthStep = 0.25f * 0.5f * level.DepthRange;
        level.DepthCentre     = std::round( cameraLight.z / depthStep ) * depthStep;

        const float left   = static_cast<float>( level.WindowOrigin.x ) * level.PageWorld;
        const float bottom = static_cast<float>( level.WindowOrigin.y ) * level.PageWorld;
        const float side   = static_cast<float>( kVsmLevelPages ) * level.PageWorld;
        const float half   = 0.5f * level.DepthRange;
        // The view looks down -z: view z = -zNear maps to 0 (toward the sun), -zFar to 1.
        const glm::mat4 projection =
             glm::orthoRH_ZO( left, left + side, bottom, bottom + side, -( level.DepthCentre + half ),
                              -( level.DepthCentre - half ) );
        level.ViewProjection = projection * lightBasis;
        return level;
    }

    Common::BoolResultStr VirtualShadowMapRenderer::Initialize()
    {
        for ( uint32_t k = 0; k < KernelCount; ++k )
        {
            KernelProgram& kernel = m_Kernels[k];
            kernel.Program = Runtime::ResourceRegistry::GetShaderService()->GetByName( kKernelNames[k].Shader );
            if ( !kernel.Program )
                return Common::MakeError(
                     std::format( "Shadows: Virtual Shadow Map: shader {} not found", kKernelNames[k].Shader ) );
            const auto pipeline =
                 ComputePipeline::Create( { .Shader = kernel.Program, .DebugName = kKernelNames[k].Shader } );
            if ( !pipeline )
                return Common::MakeError( pipeline.GetError() );
            kernel.Pipeline = pipeline.GetValue();
        }
        return BOOLSUCCESS;
    }

    bool VirtualShadowMapRenderer::AcquirePersistent( RDG::Builder& graph, const uint32_t physicalPages,
                                                      RDG::TextureRef& pool, RDG::BufferRef& pageTable,
                                                      RDG::BufferRef& owners )
    {
        if ( m_PhysicalPages == physicalPages )
        {
            pool      = graph.RegisterExternal( m_Pool, "VSM.PhysicalPool" );
            pageTable = graph.RegisterExternal( m_PageTable, "VSM.PageTable" );
            owners    = graph.RegisterExternal( m_Owners, "VSM.PhysicalOwners" );
            return false;
        }
        m_Pool      = {};
        m_PageTable = {};
        m_Owners    = {};

        const uint32_t rows = Groups( physicalPages, kVsmPoolPagesPerRow );
        pool = graph.CreateTexture( RDG::TextureDesc{ .Size   = { .Width  = kVsmPoolPagesPerRow * kVsmPageSize,
                                                                  .Height = rows * kVsmPageSize },
                                                      .Format = Core::Formats::ImageFormat::R32_UINT },
                                    "VSM.PhysicalPool" );
        // Written by "VSM: Clear Pages" and the casters, read by "VSM: Project": the last use of a frame is the
        // projection's read, the next frame's first the clear's write.
        graph.Extract( pool, m_Pool, RDG::Access::StorageRead, { RDG::Access::StorageWrite } );

        // Every entry unmapped (flags 0), every physical page free: the pool's texels are never read before a
        // clear.
        const std::vector<VsmPageEntryGpu> entries( static_cast<std::size_t>( kVsmClipmapLevelCount ) *
                                                    kVsmPagesPerLevel );
        pageTable = graph.CreateBuffer( RDG::BufferDesc{ .Bytes = entries.size() * sizeof( VsmPageEntryGpu ) },
                                        "VSM.PageTable" );
        graph.QueueBufferUpload( pageTable, Bytes( entries ) );
        graph.Extract( pageTable, m_PageTable, RDG::Access::StorageWrite );

        const std::vector<uint32_t> unowned( physicalPages, kVsmInvalid );
        owners = graph.CreateBuffer( RDG::BufferDesc{ .Bytes = unowned.size() * sizeof( uint32_t ) },
                                     "VSM.PhysicalOwners" );
        graph.QueueBufferUpload( owners, Bytes( unowned ) );
        graph.Extract( owners, m_Owners, RDG::Access::StorageWrite );

        m_PhysicalPages = physicalPages;
        return true;
    }

    bool VirtualShadowMapRenderer::UpdateClipmap( const glm::vec3& directionToSun, const glm::vec3& camera,
                                                  std::array<VsmLevelGpu, kVsmClipmapLevelCount>& levels )
    {
        const glm::vec3 lightDirection = -glm::normalize( directionToSun );
        // Any turn of the sun redraws every page (UE: a moved light invalidates its whole VSM).
        const bool lightChanged = !m_LightDirection.has_value() || *m_LightDirection != lightDirection;
        m_LightDirection        = lightDirection;
        const glm::mat4 basis   = VsmLightBasis( lightDirection );
        for ( uint32_t i = 0; i < kVsmClipmapLevelCount; ++i )
        {
            const VsmClipmapLevel level      = MakeVsmClipmapLevel( basis, camera, i );
            const bool            depthMoved = !m_DepthCentresValid[i] || m_DepthCentres[i] != level.DepthCentre;
            m_DepthCentres[i]                = level.DepthCentre;
            m_DepthCentresValid[i]           = true;
            levels[i]                        = VsmLevelGpu{ .ViewProjection = level.ViewProjection,
                                                            .Window         = glm::ivec4( level.WindowOrigin,
                                                                                          kVsmClipmapFirstLevel + static_cast<int>( i ),
                                                           depthMoved ? 1 : 0 ),
                                                            .Extent         = glm::vec4( level.PageWorld, level.DepthRange, level.Radius,
                                                                                         level.PageWorld / static_cast<float>( kVsmPageSize ) ) };
        }
        return lightChanged;
    }

    void VirtualShadowMapRenderer::ReportOverflow( const uint32_t slot )
    {
        const RDG::ExternalBuffer& readback = m_AllocatorReadback[slot];
        if ( !readback.Physical )
            return; // the slot has not been used yet
        const std::span<const std::byte> bytes = readback.Physical->ReadForHost();
        if ( bytes.size() < kVsmOverflowOffset + sizeof( uint32_t ) )
        {
            LOG_ERROR( "[Shadows] Virtual Shadow Map: the allocator readback holds {} bytes, the overflow count is at "
                       "{}; the pool overflow is not known",
                       bytes.size(), kVsmOverflowOffset );
            return;
        }
        uint32_t overflow = 0;
        std::memcpy( &overflow, bytes.data() + kVsmOverflowOffset, sizeof( overflow ) );
        if ( overflow == m_ReportedOverflow )
            return;
        m_ReportedOverflow = overflow;
        if ( overflow == 0 )
        {
            LOG_INFO( "[Shadows] Virtual Shadow Map: the physical page pool holds every requested page again" );
        }
        else
        {
            LOG_WARN( "[Shadows] Virtual Shadow Map: physical page pool overflow - {} requested page(s) found no "
                      "page among Shadows.VirtualMaxPhysicalPages = {}; their receivers are shaded from a coarser "
                      "level",
                      overflow, m_ReadbackPhysicalPages[slot] );
        }
    }

    Common::ResultStr<VirtualShadowMapFrame> VirtualShadowMapRenderer::AddPagePasses(
         RDG::Builder& graph, const VirtualShadowMapViewInputs& view, const VirtualShadowMapSun& sun,
         const VirtualShadowMapQuality& quality, const std::span<const VsmInvalidationBox> invalidations,
         const bool staticCastersChanged )
    {
        if ( !m_Kernels[Project].Pipeline )
            return Common::MakeError<VirtualShadowMapFrame>(
                 "Shadows: Virtual Shadow Map: the pipelines are not initialised" );
        if ( view.Width == 0 || view.Height == 0 || !view.Depth.IsValid() || !view.Normal.IsValid() )
            return Common::MakeError<VirtualShadowMapFrame>(
                 std::format( "Shadows: Virtual Shadow Map: the view has no G-buffer to mark pages from ({}x{})",
                              view.Width, view.Height ) );
        if ( quality.MaxPhysicalPages == 0 )
            return Common::MakeError<VirtualShadowMapFrame>(
                 "Shadows: Virtual Shadow Map: Shadows.VirtualMaxPhysicalPages is 0" );

        std::array<VsmLevelGpu, kVsmClipmapLevelCount> levels{};
        const bool lightChanged = UpdateClipmap( sun.DirectionToSun, view.CameraPosition, levels );

        VirtualShadowMapFrame frame;
        RDG::BufferRef        owners;
        const bool            made =
             AcquirePersistent( graph, quality.MaxPhysicalPages, frame.PhysicalPool, frame.PageTable, owners );
        const uint32_t resetAll = made || lightChanged || staticCastersChanged ? 1u : 0u;
        for ( uint32_t i = 0; i < kVsmClipmapLevelCount; ++i )
            frame.LevelViewProjection[i] = levels[i].ViewProjection;

        // VsmFrameData: the view's constants, then the levels.
        const VsmFrameGpu header{
             .InvViewProjection = view.InvJitteredViewProjection,
             .CameraFootprint   = glm::vec4( view.CameraPosition, 2.0f / ( std::abs( view.Projection[1][1] ) *
                                                                         static_cast<float>( view.Height ) ) ),
             .Lod   = glm::vec4( quality.ResolutionLodBias, static_cast<float>( kVsmClipmapLevelCount ),
                                 static_cast<float>( kVsmClipmapFirstLevel ),
                                 static_cast<float>( static_cast<uint32_t>( view.FrameIndex & 0xFFFFu ) ) ),
             .ToSun = glm::vec4( glm::normalize( sun.DirectionToSun ),
                                 glm::radians( std::max( sun.SourceAngleDegrees, 0.0f ) ) ),
             .Smrt  = glm::vec4( static_cast<float>( quality.SmrtRayCount ),
                                 static_cast<float>( quality.SmrtSamplesPerRay ), kVsmNormalBiasTexels,
                                 kVsmSmrtRayLengthPages ) };
        std::vector<std::byte> frameBytes( sizeof( VsmFrameGpu ) + sizeof( levels ) );
        std::memcpy( frameBytes.data(), &header, sizeof( header ) );
        std::memcpy( frameBytes.data() + sizeof( header ), levels.data(), sizeof( levels ) );
        frame.FrameData = graph.CreateBuffer( RDG::BufferDesc{ .Bytes = frameBytes.size() }, "VSM.FrameData" );
        graph.QueueBufferUpload( frame.FrameData, frameBytes );

        const uint32_t       pages = quality.MaxPhysicalPages;
        const RDG::BufferRef allocator =
             graph.CreateBuffer( RDG::BufferDesc{ .Bytes = kVsmAllocatorBytes }, "VSM.Allocator" );
        const std::vector<uint32_t> zeroCounters( kVsmAllocatorBytes / sizeof( uint32_t ), 0u );
        graph.QueueBufferUpload( allocator, Bytes( zeroCounters ) );
        const RDG::BufferRef lists =
             graph.CreateBuffer( RDG::BufferDesc{ .Bytes = 3ull * pages * sizeof( uint32_t ) }, "VSM.Lists" );

        // The overflow this slot's previous frame counted, then this frame's counters handed to the host in its
        // place (read when the slot comes round again).
        const uint32_t slots = std::max( 1u, EngineContext::GetInstance().GetMaxFramesInFlight() );
        if ( m_AllocatorReadback.size() != slots )
        {
            m_AllocatorReadback.assign( slots, RDG::ExternalBuffer{} );
            m_ReadbackPhysicalPages.assign( slots, 0u );
        }
        const uint32_t slot = static_cast<uint32_t>( view.FrameIndex % slots );
        ReportOverflow( slot );
        m_AllocatorReadback[slot]     = RDG::ExternalBuffer{};
        m_ReadbackPhysicalPages[slot] = pages;
        graph.Extract( allocator, m_AllocatorReadback[slot], RDG::Access::HostRead );

        for ( uint32_t i = 0; i < kVsmClipmapLevelCount; ++i )
        {
            const glm::uvec4 level( i, 0u, 0u, 0u );
            frame.RasterLevel[i] = graph.CreateBuffer( RDG::BufferDesc{ .Bytes = sizeof( level ) },
                                                       std::format( "VSM.RasterLevel{}", i ) );
            graph.QueueBufferUpload( frame.RasterLevel[i], std::as_bytes( std::span<const glm::uvec4>( &level, 1 ) ) );
        }

        const RDG::BufferRef  frameData = frame.FrameData;
        const RDG::BufferRef  pageTable = frame.PageTable;
        const RDG::TextureRef pool      = frame.PhysicalPool;
        constexpr auto        kR        = RDG::Access::StorageRead;
        constexpr auto        kW        = RDG::Access::StorageWrite;

        // One dispatch of kernel @p k: its block declared by @p declare, @p push (16 B) when the kernel has one.
        const auto dispatch =
             [&]( const Kernel k, auto&& declare, const std::optional<glm::uvec4> push, const glm::uvec3 groups )
        {
            const KernelProgram& kernel = m_Kernels[k];
            graph.AddPass(
                 kKernelNames[k].Pass, RDG::PassFlags::Compute,
                 [&]( RDG::PassBuilder& pass )
                 {
                     RDG::BindingBlockBuilder block =
                          pass.Bindings( kernel.Layout.Get( kernel.Program ),
                                         Renderer::GetPipelineRouteFill( *kernel.Pipeline ) );
                     declare( block );
                     if ( push )
                         block.PushConstantBytes( static_cast<uint32_t>( sizeof( glm::uvec4 ) ) );
                 },
                 [pipeline = kernel.Pipeline, push, groups,
                  name     = kKernelNames[k].Pass]( RDG::PassContext& context ) -> Common::BoolResultStr
                 {
                     DESERT_PROFILE_PASS_DYNAMIC( name );
                     RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                     if ( push )
                         bindings.PushConstants( &*push, static_cast<uint32_t>( sizeof( glm::uvec4 ) ) );
                     return Renderer::GetInstance().DispatchCompute( bindings, *pipeline, groups.x, groups.y,
                                                                     groups.z );
                 } );
        };
        const uint32_t entryGroups = Groups( kVsmPagesPerLevel, kGroup64 );

        dispatch(
             Invalidate,
             [&]( RDG::BindingBlockBuilder& block )
             {
                 block.Storage( "VsmFrameData", frameData, kR )
                      .Storage( "VsmPageTable", pageTable, kW )
                      .Storage( "VsmPhysicalOwners", owners, kW );
             },
             glm::uvec4( resetAll, 0u, 0u, 0u ), glm::uvec3( entryGroups, kVsmClipmapLevelCount, 1u ) );

        // Nothing to invalidate per caster when every page was just dropped.
        if ( !invalidations.empty() && resetAll == 0u )
        {
            std::vector<glm::vec4> boxes;
            boxes.reserve( invalidations.size() * 2 );
            for ( const VsmInvalidationBox& box : invalidations )
            {
                boxes.emplace_back( box.Min, 0.0f );
                boxes.emplace_back( box.Max, 0.0f );
            }
            const RDG::BufferRef boxBuffer = graph.CreateBuffer(
                 RDG::BufferDesc{ .Bytes = boxes.size() * sizeof( glm::vec4 ) }, "VSM.Invalidations" );
            graph.QueueBufferUpload( boxBuffer, Bytes( boxes ) );
            dispatch(
                 InvalidateCasters,
                 [&]( RDG::BindingBlockBuilder& block )
                 {
                     block.Storage( "VsmFrameData", frameData, kR )
                          .Storage( "VsmPageTable", pageTable, kW )
                          .Storage( "VsmInvalidations", boxBuffer, kR );
                 },
                 std::nullopt,
                 glm::uvec3( static_cast<uint32_t>( invalidations.size() ), kVsmClipmapLevelCount, 1u ) );
        }

        dispatch(
             Mark,
             [&]( RDG::BindingBlockBuilder& block )
             {
                 block.Storage( "VsmFrameData", frameData, kR )
                      .Storage( "VsmPageTable", pageTable, kW )
                      .Sampler( "u_PointClampSampler", RDG::SamplerDesc::PointClamp() )
                      .Texture( "u_GBufferDepth", view.Depth, RDG::Access::SampledCompute,
                                RDG::SubresourceRange::All() );
             },
             glm::uvec4( view.Width, view.Height, 0u, 0u ),
             glm::uvec3( Groups( view.Width, kScreenTile ), Groups( view.Height, kScreenTile ), 1u ) );

        dispatch(
             Gather,
             [&]( RDG::BindingBlockBuilder& block )
             {
                 block.Storage( "VsmPageTable", pageTable, kR )
                      .Storage( "VsmPhysicalOwners", owners, kR )
                      .Storage( "VsmAllocator", allocator, kW )
                      .Storage( "VsmLists", lists, kW );
             },
             glm::uvec4( pages, 0u, 0u, 0u ), glm::uvec3( Groups( pages, kGroup64 ), 1u, 1u ) );

        dispatch(
             Allocate,
             [&]( RDG::BindingBlockBuilder& block )
             {
                 block.Storage( "VsmFrameData", frameData, kR )
                      .Storage( "VsmPageTable", pageTable, kW )
                      .Storage( "VsmPhysicalOwners", owners, kW )
                      .Storage( "VsmAllocator", allocator, kW )
                      .Storage( "VsmLists", lists, kW );
             },
             glm::uvec4( pages, 0u, 0u, 0u ), glm::uvec3( entryGroups, kVsmClipmapLevelCount, 1u ) );

        // The cells of each level holding a page drawn this frame, for the casters' cull.
        frame.DirtyMask = graph.CreateBuffer(
             RDG::BufferDesc{ .Bytes = uint64_t{ kVsmClipmapLevelCount } * kVsmMaskWordsPerLevel * sizeof( uint32_t ) },
             "VSM.DirtyMask" );
        const std::vector<uint32_t> zeroMask( size_t{ kVsmClipmapLevelCount } * kVsmMaskWordsPerLevel, 0u );
        graph.QueueBufferUpload( frame.DirtyMask, Bytes( zeroMask ) );
        const RDG::BufferRef dirtyMask = frame.DirtyMask;
        dispatch(
             DirtyMask,
             [&]( RDG::BindingBlockBuilder& block )
             {
                 block.Storage( "VsmFrameData", frameData, kR )
                      .Storage( "VsmPageTable", pageTable, kR )
                      .Storage( "VsmDirtyMask", dirtyMask, kW );
             },
             std::nullopt, glm::uvec3( entryGroups, kVsmClipmapLevelCount, 1u ) );

        dispatch(
             ClearArgs, [&]( RDG::BindingBlockBuilder& block ) { block.Storage( "VsmAllocator", allocator, kW ); },
             std::nullopt, glm::uvec3( 1u ) );

        // "VSM: Clear Pages": one group per dirty page, sized on the GPU.
        {
            const KernelProgram& kernel = m_Kernels[Clear];
            const glm::uvec4     push( pages, 0u, 0u, 0u );
            graph.AddPass(
                 kKernelNames[Clear].Pass, RDG::PassFlags::Compute,
                 [&]( RDG::PassBuilder& pass )
                 {
                     pass.Read( allocator, RDG::Access::IndirectArgs );
                     pass.Bindings( kernel.Layout.Get( kernel.Program ),
                                    Renderer::GetPipelineRouteFill( *kernel.Pipeline ) )
                          .Storage( "VsmPageTable", pageTable, kR )
                          .Storage( "VsmLists", lists, kR )
                          .Storage( "u_PhysicalPool", pool, kW )
                          .PushConstantBytes( static_cast<uint32_t>( sizeof( push ) ) );
                 },
                 [pipeline = kernel.Pipeline, push,
                  allocator]( RDG::PassContext& context ) -> Common::BoolResultStr
                 {
                     DESERT_PROFILE_PASS( "VSM: Clear Pages" );
                     RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                     bindings.PushConstants( &push, static_cast<uint32_t>( sizeof( push ) ) );
                     return Renderer::DispatchComputeIndirect( bindings, *pipeline, allocator,
                                                               kVsmClearArgsOffset );
                 } );
        }
        return Common::MakeSuccess( frame );
    }

    Common::ResultStr<VsmCulledCasters> VirtualShadowMapRenderer::AddCasterCulling( RDG::Builder&                graph,
                                                                                   const VirtualShadowMapFrame& frame,
                                                                                   const uint32_t               level,
                                                                                   const VsmCasterCull& cull ) const
    {
        const KernelProgram& kernel = m_Kernels[CullCasters];
        if ( !kernel.Pipeline )
            return Common::MakeError<VsmCulledCasters>(
                 "Shadows: Virtual Shadow Map: the pipelines are not initialised" );
        if ( cull.Instances.empty() || cull.Draws.empty() || cull.Arguments.empty() || cull.RemapSize == 0 )
            return Common::MakeError<VsmCulledCasters>(
                 std::format( "VSM: Cull Casters L{}: nothing to cull ({} instances, {} draws, {} argument words)",
                              level, cull.Instances.size(), cull.Draws.size(), cull.Arguments.size() ) );
        const auto instances = static_cast<uint32_t>( cull.Instances.size() );
        // One thread per instance, the groups spread over y past the dispatch limit of x.
        constexpr uint32_t kMaxGroupsX = 65535u;
        const uint32_t     groups      = Groups( instances, kGroup64 );
        const glm::uvec3   grid( std::min( groups, kMaxGroupsX ), Groups( groups, kMaxGroupsX ), 1u );

        VsmCulledCasters culled;
        culled.Arguments = graph.CreateBuffer( RDG::BufferDesc{ .Bytes = cull.Arguments.size() * sizeof( uint32_t ) },
                                               std::format( "VSM.CasterArguments{}", level ) );
        graph.QueueBufferUpload( culled.Arguments, Bytes( cull.Arguments ) );
        culled.InstanceRemap =
             graph.CreateBuffer( RDG::BufferDesc{ .Bytes = uint64_t{ cull.RemapSize } * sizeof( uint32_t ) },
                                 std::format( "VSM.InstanceRemap{}", level ) );
        const RDG::BufferRef draws =
             graph.CreateBuffer( RDG::BufferDesc{ .Bytes = cull.Draws.size() * sizeof( VsmCullDrawGpu ) },
                                 std::format( "VSM.CullDraws{}", level ) );
        graph.QueueBufferUpload( draws, Bytes( cull.Draws ) );
        const RDG::BufferRef boxes =
             graph.CreateBuffer( RDG::BufferDesc{ .Bytes = cull.Instances.size() * sizeof( VsmCullInstanceGpu ) },
                                 std::format( "VSM.CullInstances{}", level ) );
        graph.QueueBufferUpload( boxes, Bytes( cull.Instances ) );

        // x = level, y = instance count, z = threads per row of groups (the y stride of the instance index).
        const glm::uvec4 push( level, instances, grid.x * kGroup64, 0u );
        graph.AddPass(
             std::format( "VSM: Cull Casters L{}", level ), RDG::PassFlags::Compute,
             [&]( RDG::PassBuilder& pass )
             {
                 pass.Bindings( kernel.Layout.Get( kernel.Program ),
                                Renderer::GetPipelineRouteFill( *kernel.Pipeline ) )
                      .Storage( "VsmFrameData", frame.FrameData, RDG::Access::StorageRead )
                      .Storage( "VsmDirtyMask", frame.DirtyMask, RDG::Access::StorageRead )
                      .Storage( "VsmCullInstances", boxes, RDG::Access::StorageRead )
                      .Storage( "VsmCullDraws", draws, RDG::Access::StorageRead )
                      .Storage( "VsmCasterArguments", culled.Arguments, RDG::Access::StorageWrite )
                      .Storage( "VsmInstanceRemap", culled.InstanceRemap, RDG::Access::StorageWrite )
                      .PushConstantBytes( static_cast<uint32_t>( sizeof( push ) ) );
             },
             [pipeline = kernel.Pipeline, push, grid]( RDG::PassContext& context ) -> Common::BoolResultStr
             {
                 DESERT_PROFILE_PASS( "VSM: Cull Casters" );
                 RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                 bindings.PushConstants( &push, static_cast<uint32_t>( sizeof( push ) ) );
                 return Renderer::GetInstance().DispatchCompute( bindings, *pipeline, grid.x, grid.y, grid.z );
             } );
        return Common::MakeSuccess( culled );
    }

    Common::ResultStr<RDG::TextureRef>
    VirtualShadowMapRenderer::AddProjection( RDG::Builder& graph, const VirtualShadowMapFrame& frame,
                                             const VirtualShadowMapViewInputs& view )
    {
        const KernelProgram& kernel = m_Kernels[Project];
        if ( !kernel.Pipeline )
            return Common::MakeError<RDG::TextureRef>(
                 "Shadows: Virtual Shadow Map: the pipelines are not initialised" );
        const RDG::TextureRef mask =
             graph.CreateTexture( RDG::TextureDesc{ .Size   = { .Width = view.Width, .Height = view.Height },
                                                    .Format = ViewTargetFormats::kDenoiserSignal },
                                  kShadowMaskSunName );
        graph.SetFaultDefault( mask, RDG::FaultDefault::White );
        const uint32_t width  = view.Width;
        const uint32_t height = view.Height;
        graph.AddPass(
             kKernelNames[Project].Pass, RDG::PassFlags::Compute,
             [&]( RDG::PassBuilder& pass )
             {
                 pass.Bindings( kernel.Layout.Get( kernel.Program ),
                                Renderer::GetPipelineRouteFill( *kernel.Pipeline ) )
                      .Storage( "VsmFrameData", frame.FrameData, RDG::Access::StorageRead )
                      .Storage( "VsmPageTable", frame.PageTable, RDG::Access::StorageRead )
                      .Storage( "u_PhysicalPool", frame.PhysicalPool, RDG::Access::StorageRead )
                      .Sampler( "u_PointClampSampler", RDG::SamplerDesc::PointClamp() )
                      .Texture( "u_GBufferDepth", view.Depth, RDG::Access::SampledCompute,
                                RDG::SubresourceRange::All() )
                      .Texture( "u_GBufferNormal", view.Normal, RDG::Access::SampledCompute,
                                RDG::SubresourceRange::All() )
                      .Storage( "u_ShadowMask", mask, RDG::Access::StorageWrite );
             },
             [pipeline = kernel.Pipeline, width, height]( RDG::PassContext& context ) -> Common::BoolResultStr
             {
                 DESERT_PROFILE_PASS( "VSM: Project" );
                 RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                 return Renderer::GetInstance().DispatchCompute( bindings, *pipeline, Groups( width, kScreenTile ),
                                                                 Groups( height, kScreenTile ), 1 );
             } );
        return Common::MakeSuccess( mask );
    }
} // namespace Desert::Graphic::System
