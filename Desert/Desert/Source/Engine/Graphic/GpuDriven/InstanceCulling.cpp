#include <Engine/Graphic/GpuDriven/InstanceCulling.hpp>

#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/RDG/RDGShaderParameters.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/ShaderBindingLayoutCache.hpp>
#include <Engine/Graphic/Systems/Scene/Shadows/VirtualShadowMap.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Runtime/Services/Shader/ShaderService.hpp>
#include <Engine/ShaderResources/StorageBuffer.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <format>
#include <limits>
#include <string>

namespace Desert::Graphic::GpuDriven
{
    namespace
    {
        // The slot names of Programs/GpuDriven/InstanceCull.shader, spelled once (RDGShaderParameters.hpp).
        struct InstanceCullShaderParameters
        {
            DESERT_SHADER_PARAMETER( StorageBuffer, GpuCullPrimitives );
            DESERT_SHADER_PARAMETER( StorageBuffer, GpuCullInstances );
            DESERT_SHADER_PARAMETER( StorageBuffer, GpuCullLodScreenSizes );
            DESERT_SHADER_PARAMETER( StorageBuffer, GpuCullWorldBounds );
            DESERT_SHADER_PARAMETER( StorageBuffer, GpuCullViews );
            DESERT_SHADER_PARAMETER( StorageBuffer, GpuCullPassPrimitiveDraws );
            DESERT_SHADER_PARAMETER( StorageBuffer, GpuCullPassDrawSlots );
            DESERT_SHADER_PARAMETER( StorageBuffer, GpuCullPassBins );
            DESERT_SHADER_PARAMETER( StorageBuffer, GpuCullIndirectDraws );
            DESERT_SHADER_PARAMETER( StorageBuffer, GpuCullVisibleInstances );
            DESERT_SHADER_PARAMETER( StorageBuffer, GpuCullOccluded );
            DESERT_SHADER_PARAMETER( StorageBuffer, GpuCullOccludedCount );
            DESERT_SHADER_PARAMETER( StorageBuffer, GpuCullDispatchArgs );
            DESERT_SHADER_PARAMETER( StorageBuffer, GpuCullVisibility );
            DESERT_SHADER_PARAMETER( StorageBuffer, GpuCullVsmDirtyMask );
            DESERT_SHADER_PARAMETER( StorageBuffer, GpuCullVisibleInstanceIds );
            DESERT_SHADER_PARAMETER( StorageBuffer, GpuCullVgPrimitiveDags );
            DESERT_SHADER_PARAMETER( StorageBuffer, GpuCullVgDagEntries );
            DESERT_SHADER_PARAMETER( StorageBuffer, GpuCullVgCutViewOf );
            DESERT_SHADER_PARAMETER( StorageBuffer, GpuCullVgCandidates );
            DESERT_SHADER_PARAMETER( StorageBuffer, GpuCullVgCandidateCount );
            DESERT_SHADER_PARAMETER( StorageBuffer, GpuCullContentHash );
            DESERT_SHADER_PARAMETER( Sampler, PointClampSampler );
            DESERT_SHADER_PARAMETER( Texture, GpuCullHzbTexture );
        };

        constexpr const char* kShaderName    = "InstanceCull"; // Programs/GpuDriven/InstanceCull.shader
        constexpr uint32_t    kGroupSize     = 64u;
        constexpr uint32_t    kMaxGroupsX    = 65535u;
        constexpr uint32_t    kStepInitDraws = 0u;
        constexpr uint32_t    kStepMain      = 1u;
        constexpr uint32_t    kStepPost      = 2u;
        constexpr uint32_t    kStepArgs      = 3u;

        // InstanceCull.shader's push block: Params, Params2, Params3 (x = VG primitive rows, y = VG candidate
        // rows per cut view; 0 = no append).
        struct CullPush
        {
            glm::uvec4 Params{ 0u };
            glm::uvec4 Params2{ 0u };
            glm::uvec4 Params3{ 0u };
        };

        glm::uvec3 Grid( const uint64_t threads )
        {
            const auto groups =
                 static_cast<uint32_t>( std::max<uint64_t>( 1u, ( threads + kGroupSize - 1u ) / kGroupSize ) );
            const uint32_t x = std::min( groups, kMaxGroupsX );
            return { x, ( groups + x - 1u ) / x, 1u };
        }

        template <typename T>
        std::span<const std::byte> Bytes( const std::vector<T>& values )
        {
            return std::as_bytes( std::span( values ) );
        }

        // A zero-filled graph buffer for a slot the dispatch does not read (a pass with no bins, no VSM view):
        // written by its clear, so the graph orders and accepts the bind. FPS-CPU-UPLOAD: a GPU fill in the
        // graph's upload batch (QueueBufferClear), not a heap vector of zeros copied per frame.
        RDG::BufferRef ZeroBuffer( RDG::Builder& graph, const std::string& name, const uint64_t bytes = 16u )
        {
            const RDG::BufferRef buffer = graph.CreateBuffer( RDG::BufferDesc{ .Bytes = bytes }, name );
            graph.QueueBufferClear( buffer, 0u );
            return buffer;
        }

        // FPS-SET-MEMO4: @p bytes of a kept device-local buffer (CulledRowStore), imported into @p graph as
        // @p name. Grow-only to the power of two of the need, so the buffer (and its size in the bound-sets memo
        // key) stays put while VisibleCapacity moves; the shaders index rows by Params2.y, a larger buffer reads
        // the same. Refused over a buffer's 32-bit size or when the buffer cannot be made or imported.
        Common::ResultStr<RDG::BufferRef> KeptBuffer( RDG::Builder&                                              graph,
                                                      std::shared_ptr<::Desert::ShaderResources::StorageBuffer>& buffer,
                                                      RDG::ExternalBuffer& import, uint64_t& capacity,
                                                      const uint64_t bytes, const std::string& name ) noexcept
        {
            constexpr uint64_t kMaxBytes = std::numeric_limits<uint32_t>::max();
            if ( buffer == nullptr || bytes > capacity )
            {
                if ( bytes > kMaxBytes )
                    return Common::MakeError<RDG::BufferRef>( std::format(
                         "GPUDriven: Cull: '{}' needs {} bytes, over a buffer's 32-bit size", name, bytes ) );
                const uint64_t grown = std::min( std::bit_ceil( std::max<uint64_t>( bytes, 256u ) ), kMaxBytes );
                buffer               = ::Desert::ShaderResources::StorageBuffer::CreateDeviceLocal(
                     name, static_cast<uint32_t>( grown ), 0u );
                import               = RDG::ExternalBuffer{};
                if ( buffer == nullptr )
                {
                    capacity = 0u;
                    return Common::MakeError<RDG::BufferRef>(
                         std::format( "GPUDriven: Cull: '{}' ({} bytes) was not made", name, grown ) );
                }
                capacity = grown;
            }
            if ( const Common::BoolResultStr imported = Renderer::ImportBuffer( buffer, import ); !imported )
                return Common::MakeError<RDG::BufferRef>(
                     std::format( "GPUDriven: Cull: '{}': {}", name, imported.GetError() ) );
            return Common::MakeSuccess( graph.RegisterExternal( import, name ) );
        }

        // The visible rows of one pass's dispatch: the call site's kept pair (@p rows), @p rowCount rows each.
        Common::BoolResultStr KeptRows( RDG::Builder& graph, CulledRowStore::Rows& rows, const uint64_t rowCount,
                                        const std::string& suffix, RDG::BufferRef& visible,
                                        RDG::BufferRef& visibleIds ) noexcept
        {
            const uint64_t                          count = std::max<uint64_t>( 1u, rowCount );
            const Common::ResultStr<RDG::BufferRef> v =
                 KeptBuffer( graph, rows.Visible, rows.VisibleImport, rows.VisibleBytes,
                             count * sizeof( GpuVisibleInstance ), "GPUDriven.VisibleInstances" + suffix );
            if ( !v )
                return Common::MakeError( v.GetError() );
            const Common::ResultStr<RDG::BufferRef> ids =
                 KeptBuffer( graph, rows.VisibleIds, rows.VisibleIdsImport, rows.VisibleIdsBytes,
                             count * sizeof( uint32_t ), "GPUDriven.VisibleInstanceIds" + suffix );
            if ( !ids )
                return Common::MakeError( ids.GetError() );
            visible    = v.GetValue();
            visibleIds = ids.GetValue();
            return BOOLSUCCESS;
        }

        // The views of each mesh pass, in the caller's order.
        std::array<std::vector<uint32_t>, kMeshPassCount> GroupByPass( std::span<const CullViewInput> views )
        {
            std::array<std::vector<uint32_t>, kMeshPassCount> groups;
            for ( uint32_t v = 0; v < views.size(); ++v )
                groups[std::min( views[v].View.Pass, kMeshPassCount - 1u )].push_back( v );
            return groups;
        }
    } // namespace

    struct InstanceCulling::Program
    {
        std::shared_ptr<Shader>          Shader;
        std::shared_ptr<ComputePipeline> Pipeline;
        mutable ShaderBindingLayoutCache Layout;

        Common::BoolResultStr Ensure()
        {
            if ( Pipeline )
                return BOOLSUCCESS;
            Runtime::ShaderService* service = Runtime::ResourceRegistry::GetShaderService();
            Shader                          = service ? service->GetByName( std::string( kShaderName ) ) : nullptr;
            if ( !Shader )
                return Common::MakeError(
                     std::format( "the shader '{}' did not compile - expected Editor/Resources/"
                                  "Shaders/Programs/GpuDriven/InstanceCull.shader",
                                  kShaderName ) );
            const auto built =
                 ComputePipeline::Create( { .Shader = Shader, .DebugName = std::string( kShaderName ) } );
            if ( !built )
                return Common::MakeError(
                     std::format( "the '{}' pipeline was not built: {}", kShaderName, built.GetError() ) );
            Pipeline = built.GetValue();
            return BOOLSUCCESS;
        }
    };

    InstanceCulling::InstanceCulling() : m_Program( std::make_unique<Program>() )
    {
    }
    InstanceCulling::~InstanceCulling() = default;

    namespace
    {
        // Everything one dispatch of one mesh pass binds.
        struct DispatchBinds
        {
            RDG::BufferRef  Views, PrimitiveDraws, DrawSlots, Bins;
            RDG::BufferRef  Draws, Visible, VisibleIds;
            RDG::BufferRef  Occluded, OccludedCount, DispatchArgs, Visibility, DirtyMask;
            // The step-2 dispatch reads DispatchArgs as its indirect arguments, so the program's DispatchArgs slot
            // (written by step 3 only) is filled with this unread buffer there: every slot of the layout is bound.
            RDG::BufferRef  ArgsSlot;
            RDG::TextureRef Hzb;
            // The VG candidate append (VirtualGeometryCandidateInput / Output); zero buffers when it is off.
            RDG::BufferRef VgPrimitiveDags, VgDagEntries, VgCutViewOf, VgCandidates, VgCandidateCount;
            // The cascade content hash (MainPhaseResult::ContentHash); phase 2 binds an unread zero buffer.
            RDG::BufferRef ContentHash;
        };

        // Fills the VG slots of a dispatch that appends no candidate (Params3 = 0): every slot is bound.
        void BindNoVirtualGeometry( RDG::Builder& graph, DispatchBinds& b, const std::string& suffix )
        {
            b.VgPrimitiveDags  = ZeroBuffer( graph, "GPUDriven.NoVgPrimitiveDags" + suffix );
            b.VgDagEntries     = ZeroBuffer( graph, "GPUDriven.NoVgDagEntries" + suffix );
            b.VgCutViewOf      = ZeroBuffer( graph, "GPUDriven.NoVgCutViewOf" + suffix );
            b.VgCandidates     = ZeroBuffer( graph, "GPUDriven.NoVgCandidates" + suffix );
            b.VgCandidateCount = ZeroBuffer( graph, "GPUDriven.NoVgCandidateCount" + suffix );
        }

        void BindAll( RDG::PassBuilder& pass, const InstanceCulling::Program& program,
                      const GpuSceneBuffers& scene, const DispatchBinds& b, const bool argsAsIndirect )
        {
            using P            = InstanceCullShaderParameters;
            constexpr auto R   = RDG::Access::StorageRead;
            constexpr auto W   = RDG::Access::StorageWrite;
            auto           blk = pass.Bindings( program.Layout.Get( program.Shader ),
                                                Renderer::GetPipelineRouteFill( *program.Pipeline ) );
            blk.Storage( P::GpuCullPrimitives, scene.Primitives, R )
                 .Storage( P::GpuCullInstances, scene.Instances, R )
                 .Storage( P::GpuCullLodScreenSizes, scene.LodScreenSizes, R )
                 .Storage( P::GpuCullWorldBounds, scene.WorldBounds, R )
                 .Storage( P::GpuCullViews, b.Views, R )
                 .Storage( P::GpuCullPassPrimitiveDraws, b.PrimitiveDraws, R )
                 .Storage( P::GpuCullPassDrawSlots, b.DrawSlots, R )
                 .Storage( P::GpuCullPassBins, b.Bins, R )
                 .Storage( P::GpuCullIndirectDraws, b.Draws, W )
                 .Storage( P::GpuCullVisibleInstances, b.Visible, W )
                 .Storage( P::GpuCullVisibleInstanceIds, b.VisibleIds, W )
                 .Storage( P::GpuCullOccluded, b.Occluded, W )
                 .Storage( P::GpuCullOccludedCount, b.OccludedCount, W )
                 .Storage( P::GpuCullVisibility, b.Visibility, W )
                 .Storage( P::GpuCullVsmDirtyMask, b.DirtyMask, R )
                 .Storage( P::GpuCullVgPrimitiveDags, b.VgPrimitiveDags, R )
                 .Storage( P::GpuCullVgDagEntries, b.VgDagEntries, R )
                 .Storage( P::GpuCullVgCutViewOf, b.VgCutViewOf, R )
                 .Storage( P::GpuCullVgCandidates, b.VgCandidates, W )
                 .Storage( P::GpuCullVgCandidateCount, b.VgCandidateCount, W )
                 .Storage( P::GpuCullContentHash, b.ContentHash, W )
                 .Sampler( P::PointClampSampler, RDG::SamplerDesc::PointClamp() )
                 .Texture( P::GpuCullHzbTexture, b.Hzb, RDG::Access::SampledCompute, RDG::SubresourceRange::All() )
                 .PushConstantBytes( static_cast<uint32_t>( sizeof( CullPush ) ) );
            if ( argsAsIndirect )
            {
                pass.Read( b.DispatchArgs, RDG::Access::IndirectArgs );
                blk.Storage( P::GpuCullDispatchArgs, b.ArgsSlot, W );
            }
            else
                blk.Storage( P::GpuCullDispatchArgs, b.DispatchArgs, W );
        }

        void AddStep( RDG::Builder& graph, const std::string& name, const InstanceCulling::Program& program,
                      const GpuSceneBuffers& scene, const DispatchBinds& binds, const CullPush push,
                      const uint64_t threads, const bool indirect )
        {
            const glm::uvec3 grid = Grid( threads );
            graph.AddPass(
                 name, RDG::PassFlags::Compute,
                 [&]( RDG::PassBuilder& pass ) { BindAll( pass, program, scene, binds, indirect ); },
                 [pipeline = program.Pipeline, push, grid, indirect,
                  args     = binds.DispatchArgs]( RDG::PassContext& context ) -> Common::BoolResultStr
                 {
                     RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                     bindings.PushConstants( &push, static_cast<uint32_t>( sizeof( push ) ) );
                     if ( indirect )
                         return Renderer::DispatchComputeIndirect( bindings, *pipeline, args, 0u );
                     return Renderer::DispatchCompute( bindings, *pipeline, grid.x, grid.y, grid.z );
                 } );
        }
    } // namespace

    Common::ResultStr<MainPhaseResult> InstanceCulling::AddMainPhase(
         RDG::Builder& graph, const GpuSceneBuffers& scene, const std::span<const PassDrawTable> tables,
         const std::span<const PassDrawBuffers> passes, const std::span<const CullViewInput> views,
         const RDG::TextureRef neutralHzb, const VirtualGeometryCandidateInput& virtualGeometry,
         CulledRowStore& rows ) const
    {
        if ( views.empty() )
            return Common::MakeError<MainPhaseResult>( "GPUDriven: Cull: no view to cull for" );
        const bool appendsVg = virtualGeometry.PrimitiveDags.IsValid();
        if ( appendsVg && ( !virtualGeometry.DagEntries.IsValid() || !virtualGeometry.CutViewOf.IsValid() ||
                            virtualGeometry.PrimitiveRows == 0u || virtualGeometry.Capacity == 0u ||
                            virtualGeometry.CutViews == 0u ) )
            return Common::MakeError<MainPhaseResult>( std::format(
                 "GPUDriven: Cull: virtual-geometry candidates half given (entries {}, view map {}, {} primitive "
                 "rows, capacity {} per cut view, {} cut views)",
                 virtualGeometry.DagEntries.IsValid() ? "valid" : "invalid",
                 virtualGeometry.CutViewOf.IsValid() ? "valid" : "invalid", virtualGeometry.PrimitiveRows,
                 virtualGeometry.Capacity, virtualGeometry.CutViews ) );
        if ( appendsVg && uint64_t{ virtualGeometry.Capacity } * virtualGeometry.CutViews > UINT32_MAX )
            return Common::MakeError<MainPhaseResult>(
                 std::format( "GPUDriven: Cull: {} cut views x {} candidate rows pass the row index range",
                              virtualGeometry.CutViews, virtualGeometry.Capacity ) );
        if ( tables.size() != kMeshPassCount || passes.size() != kMeshPassCount )
            return Common::MakeError<MainPhaseResult>(
                 std::format( "GPUDriven: Cull: {} pass tables / {} pass buffers, expected one per MeshPass ({})",
                              tables.size(), passes.size(), kMeshPassCount ) );
        if ( const Common::BoolResultStr ready = m_Program->Ensure(); !ready )
            return Common::MakeError<MainPhaseResult>( std::format( "GPUDriven: Cull: {}", ready.GetError() ) );

        const auto     viewCount = static_cast<uint32_t>( views.size() );
        const uint32_t instances = std::max( scene.InstanceCount, 1u );
        const auto     groups    = GroupByPass( views );

        // The view rows, grouped by pass (a dispatch reads Views[first, +count)); ViewIndex = the caller's index.
        std::vector<GpuCullView>             rows;
        std::array<uint32_t, kMeshPassCount> firstRow{};
        rows.reserve( viewCount );
        for ( uint32_t p = 0; p < kMeshPassCount; ++p )
        {
            firstRow[p] = static_cast<uint32_t>( rows.size() );
            for ( const uint32_t v : groups[p] )
            {
                GpuCullView row = views[v].View;
                for ( uint32_t plane = 0; plane < 6u; ++plane )
                {
                    const Core::Frustum::Plane& fp =
                         views[v].Frustum.GetPlane( static_cast<Core::FrustumPlane>( plane ) );
                    row.FrustumPlanes[plane] = glm::vec4( fp.Normal, fp.Distance );
                }
                row.Strips = glm::uvec4( std::min( views[v].StripCount, 2u ), 0u, 0u, 0u );
                for ( uint32_t s = 0; s < row.Strips.x; ++s )
                    for ( uint32_t plane = 0; plane < 6u; ++plane )
                    {
                        const Core::Frustum::Plane& fp =
                             views[v].Strips[s].GetPlane( static_cast<Core::FrustumPlane>( plane ) );
                        row.StripPlanes[s * 6u + plane] = glm::vec4( fp.Normal, fp.Distance );
                    }
                row.ViewIndex = v;
                rows.push_back( row );
            }
        }
        const RDG::BufferRef viewBuffer = graph.CreateBuffer(
             RDG::BufferDesc{ .Bytes = rows.size() * sizeof( GpuCullView ) }, "GPUDriven.CullViews" );
        graph.QueueBufferUpload( viewBuffer, Bytes( rows ) );

        MainPhaseResult result;
        result.Draws.resize( viewCount );
        const uint64_t occludedCapacity = uint64_t{ viewCount } * instances;
        result.Carry.ViewCount          = viewCount;
        result.Carry.InstanceCount      = instances;
        result.Carry.OccludedInstances  = graph.CreateBuffer(
             RDG::BufferDesc{ .Bytes = occludedCapacity * sizeof( GpuOccludedInstance ) }, "GPUDriven.Occluded" );
        const std::vector<uint32_t> countHeader{ 0u, static_cast<uint32_t>( occludedCapacity ), 0u, 0u };
        result.Carry.OccludedCount =
             graph.CreateBuffer( RDG::BufferDesc{ .Bytes = sizeof( uint32_t ) * 4u }, "GPUDriven.OccludedCount" );
        graph.QueueBufferUpload( result.Carry.OccludedCount, Bytes( countHeader ) );
        result.Carry.DispatchArgs = ZeroBuffer( graph, "GPUDriven.PostOcclusionArgs" );
        result.Carry.Visibility   = graph.CreateBuffer(
             RDG::BufferDesc{ .Bytes = occludedCapacity * sizeof( uint32_t ) }, "GPUDriven.Visibility" );
        if ( appendsVg )
        {
            result.VirtualGeometry.Capacity = virtualGeometry.Capacity;
            result.VirtualGeometry.CutViews = virtualGeometry.CutViews;
            result.VirtualGeometry.Rows     = graph.CreateBuffer(
                 RDG::BufferDesc{ .Bytes = uint64_t{ virtualGeometry.Capacity } * virtualGeometry.CutViews *
                                           kVirtualGeometryCandidateBytes },
                 "GPUDriven.VgCandidates" );
            // [0] = binned VG instances, [1 + v] = cut view v's append count.
            result.VirtualGeometry.Count =
                 ZeroBuffer( graph, "GPUDriven.VgCandidateCount",
                             std::max<uint64_t>( 16u, ( uint64_t{ virtualGeometry.CutViews } + 1u ) * 4u ) );
        }
        // THE CASCADE CONTENT HASH (CullViewFlags::kContentHash): one region per content-hash view's
        // ContentCells.w, zeroed here, summed by the views' instances.
        for ( const CullViewInput& view : views )
            if ( ( view.View.Flags & CullViewFlags::kContentHash ) != 0u && view.View.ContentCells.w >= 0 )
                result.ContentRegions =
                     std::max( result.ContentRegions, static_cast<uint32_t>( view.View.ContentCells.w ) + 1u );
        result.ContentHash =
             ZeroBuffer( graph, "GPUDriven.CascadeContentHash",
                         std::max<uint64_t>( 16u, uint64_t{ result.ContentRegions } * CascadeContentHash::kCells *
                                                       CascadeContentHash::kWords * sizeof( uint32_t ) ) );

        for ( uint32_t p = 0; p < kMeshPassCount; ++p )
        {
            if ( groups[p].empty() )
                continue;
            DispatchBinds binds;
            binds.Views         = viewBuffer;
            binds.ContentHash   = result.ContentHash;
            if ( appendsVg )
            {
                binds.VgPrimitiveDags  = virtualGeometry.PrimitiveDags;
                binds.VgDagEntries     = virtualGeometry.DagEntries;
                binds.VgCutViewOf      = virtualGeometry.CutViewOf;
                binds.VgCandidates     = result.VirtualGeometry.Rows;
                binds.VgCandidateCount = result.VirtualGeometry.Count;
            }
            else
                BindNoVirtualGeometry( graph, binds, std::format( ".P{}", p ) );
            binds.Occluded      = result.Carry.OccludedInstances;
            binds.OccludedCount = result.Carry.OccludedCount;
            binds.DispatchArgs  = result.Carry.DispatchArgs;
            binds.Visibility    = result.Carry.Visibility;
            for ( const uint32_t v : groups[p] )
            {
                const CullViewInput& view = views[v];
                if ( view.PrevHzb.IsValid() )
                {
                    if ( binds.Hzb.IsValid() && binds.Hzb != view.PrevHzb )
                        return Common::MakeError<MainPhaseResult>( std::format(
                             "GPUDriven: Cull: pass {} has views with two HZBs (one per dispatch)", p ) );
                    binds.Hzb = view.PrevHzb;
                }
                if ( view.VsmDirtyMask.IsValid() )
                    binds.DirtyMask = view.VsmDirtyMask;
            }
            const bool hasHzb = binds.Hzb.IsValid();
            if ( !hasHzb )
                binds.Hzb = neutralHzb;
            if ( !binds.DirtyMask.IsValid() )
                binds.DirtyMask = ZeroBuffer( graph, "GPUDriven.NoVsmMask" );

            const PassDrawTable&   table    = tables[p];
            const PassDrawBuffers& pass     = passes[p];
            const auto             bins     = static_cast<uint32_t>( table.Bins.size() );
            const uint32_t         capacity = table.VisibleCapacity;
            const auto             local    = static_cast<uint32_t>( groups[p].size() );
            const bool             hasBins  = bins > 0u && pass.Bins.IsValid();
            binds.PrimitiveDraws =
                 hasBins ? pass.PrimitiveDraws : ZeroBuffer( graph, "GPUDriven.NoPrimitiveDraws" );
            binds.DrawSlots = hasBins ? pass.DrawSlots : ZeroBuffer( graph, "GPUDriven.NoDrawSlots" );
            binds.Bins      = hasBins ? pass.Bins : ZeroBuffer( graph, "GPUDriven.NoBins" );
            binds.Draws =
                 graph.CreateBuffer( RDG::BufferDesc{ .Bytes = std::max<uint64_t>( 1u, uint64_t{ local } * bins ) *
                                                               sizeof( GpuIndirectDraw ) },
                                     std::format( "GPUDriven.IndirectDraws.P{}", p ) );
            if ( const Common::BoolResultStr kept = KeptRows( graph, rows.Main[p], uint64_t{ local } * capacity,
                                                              std::format( ".P{}", p ), binds.Visible,
                                                              binds.VisibleIds );
                 !kept )
                return Common::MakeError<MainPhaseResult>( kept.GetError() );

            CullPush push;
            push.Params  = { kStepInitDraws, instances, local, firstRow[p] };
            push.Params2 = { hasBins ? bins : 0u, capacity,
                             hasBins ? static_cast<uint32_t>( table.PrimitiveDraws.size() ) : 0u,
                             hasHzb ? 1u : 0u };
            push.Params3 = appendsVg
                                ? glm::uvec4( virtualGeometry.PrimitiveRows, virtualGeometry.Capacity, 0u, 0u )
                                : glm::uvec4( 0u );
            AddStep( graph, std::format( "GPUDriven: Cull Init Draws P{}", p ), *m_Program, scene, binds, push,
                     uint64_t{ local } * std::max( bins, 1u ), false );
            push.Params.x = kStepMain;
            AddStep( graph, std::format( "GPUDriven: Cull Phase 1 P{}", p ), *m_Program, scene, binds, push,
                     uint64_t{ local } * instances, false );
            for ( uint32_t k = 0; k < local; ++k )
                result.Draws[groups[p][k]] = CulledDraws{ .IndirectDraws      = binds.Draws,
                                                          .VisibleInstances   = binds.Visible,
                                                          .VisibleInstanceIds = binds.VisibleIds,
                                                          .FirstIndirectDraw  = k * bins,
                                                          .VisibleRowEnd      = ( k + 1u ) * capacity };
        }
        return Common::MakeSuccess( std::move( result ) );
    }

    Common::ResultStr<std::vector<CulledDraws>> InstanceCulling::AddPostOcclusionPhase(
         RDG::Builder& graph, const GpuSceneBuffers& scene, const std::span<const PassDrawTable> tables,
         const std::span<const PassDrawBuffers> passes, const std::span<const CullViewInput> views,
         const OcclusionCarry& carry, CulledRowStore& rows ) const
    {
        using Result = std::vector<CulledDraws>;
        if ( views.empty() || views.size() != carry.ViewCount )
            return Common::MakeError<Result>( std::format( "GPUDriven: Cull Phase 2: {} views, phase 1 culled {}",
                                                           views.size(), carry.ViewCount ) );
        if ( tables.size() != kMeshPassCount || passes.size() != kMeshPassCount )
            return Common::MakeError<Result>( "GPUDriven: Cull Phase 2: expected one pass table per MeshPass" );
        if ( const Common::BoolResultStr ready = m_Program->Ensure(); !ready )
            return Common::MakeError<Result>( std::format( "GPUDriven: Cull Phase 2: {}", ready.GetError() ) );

        // The same row order as phase 1, with ViewProjection now meaning "this frame's HZB".
        const auto                           groups = GroupByPass( views );
        std::vector<GpuCullView>             rows;
        std::array<uint32_t, kMeshPassCount> firstRow{};
        for ( uint32_t p = 0; p < kMeshPassCount; ++p )
        {
            firstRow[p] = static_cast<uint32_t>( rows.size() );
            for ( const uint32_t v : groups[p] )
            {
                GpuCullView row = views[v].View;
                row.ViewIndex   = v;
                rows.push_back( row );
            }
        }
        const RDG::BufferRef viewBuffer = graph.CreateBuffer(
             RDG::BufferDesc{ .Bytes = rows.size() * sizeof( GpuCullView ) }, "GPUDriven.CullViews2" );
        graph.QueueBufferUpload( viewBuffer, Bytes( rows ) );

        Result result( views.size() );
        bool   argsWritten = false;
        for ( uint32_t p = 0; p < kMeshPassCount; ++p )
        {
            DispatchBinds binds;
            for ( const uint32_t v : groups[p] )
                if ( views[v].Hzb.IsValid() )
                    binds.Hzb = views[v].Hzb;
            if ( !binds.Hzb.IsValid() )
                continue; // no view of this pass tests occlusion: everything it draws is in phase 1
            binds.Views         = viewBuffer;
            binds.Occluded      = carry.OccludedInstances;
            binds.OccludedCount = carry.OccludedCount;
            binds.DispatchArgs  = carry.DispatchArgs;
            binds.Visibility    = carry.Visibility;
            binds.DirtyMask     = ZeroBuffer( graph, "GPUDriven.NoVsmMask2" );
            BindNoVirtualGeometry( graph, binds, std::format( "2.P{}", p ) ); // phase 2 appends no candidate
            binds.ContentHash = ZeroBuffer( graph, std::format( "GPUDriven.NoContentHash2.P{}", p ) );

            const PassDrawTable&   table   = tables[p];
            const PassDrawBuffers& pass    = passes[p];
            const auto             bins    = static_cast<uint32_t>( table.Bins.size() );
            const auto             local   = static_cast<uint32_t>( groups[p].size() );
            const bool             hasBins = bins > 0u && pass.Bins.IsValid();
            binds.PrimitiveDraws =
                 hasBins ? pass.PrimitiveDraws : ZeroBuffer( graph, "GPUDriven.NoPrimitiveDraws2" );
            binds.DrawSlots = hasBins ? pass.DrawSlots : ZeroBuffer( graph, "GPUDriven.NoDrawSlots2" );
            binds.Bins      = hasBins ? pass.Bins : ZeroBuffer( graph, "GPUDriven.NoBins2" );
            binds.Draws =
                 graph.CreateBuffer( RDG::BufferDesc{ .Bytes = std::max<uint64_t>( 1u, uint64_t{ local } * bins ) *
                                                               sizeof( GpuIndirectDraw ) },
                                     std::format( "GPUDriven.IndirectDraws2.P{}", p ) );
            if ( const Common::BoolResultStr kept =
                      KeptRows( graph, rows.Post[p], uint64_t{ local } * table.VisibleCapacity,
                                std::format( "2.P{}", p ), binds.Visible, binds.VisibleIds );
                 !kept )
                return Common::MakeError<Result>( kept.GetError() );
            binds.ArgsSlot = ZeroBuffer( graph, std::format( "GPUDriven.PostArgsSlot.P{}", p ) );

            CullPush push;
            push.Params  = { kStepInitDraws, carry.InstanceCount, local, firstRow[p] };
            push.Params2 = { hasBins ? bins : 0u, table.VisibleCapacity,
                             hasBins ? static_cast<uint32_t>( table.PrimitiveDraws.size() ) : 0u, 1u };
            AddStep( graph, std::format( "GPUDriven: Cull Init Draws 2 P{}", p ), *m_Program, scene, binds, push,
                     uint64_t{ local } * std::max( bins, 1u ), false );
            if ( !argsWritten )
            {
                push.Params.x = kStepArgs;
                AddStep( graph, "GPUDriven: Cull Phase 2 Args", *m_Program, scene, binds, push, 1u, false );
                argsWritten = true;
            }
            push.Params.x = kStepPost;
            AddStep( graph, std::format( "GPUDriven: Cull Phase 2 P{}", p ), *m_Program, scene, binds, push, 0u,
                     true );
            for ( uint32_t k = 0; k < local; ++k )
                result[groups[p][k]] = CulledDraws{ .IndirectDraws      = binds.Draws,
                                                    .VisibleInstances   = binds.Visible,
                                                    .VisibleInstanceIds = binds.VisibleIds,
                                                    .FirstIndirectDraw  = k * bins,
                                                    .VisibleRowEnd      = ( k + 1u ) * table.VisibleCapacity };
        }
        return Common::MakeSuccess( std::move( result ) );
    }
} // namespace Desert::Graphic::GpuDriven
