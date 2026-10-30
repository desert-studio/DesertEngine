#include <Engine/Graphic/VirtualGeometry/VirtualGeometryRenderer.hpp>

#include <Common/Core/Logger.hpp>
#include <Engine/Core/EngineContext.hpp>
#include <Engine/Geometry/ClusterDag.hpp>
#include <Engine/Graphic/GpuDriven/GpuSceneLayout.hpp>
#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/RDG/RDGShaderParameters.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/ShaderBindingLayoutCache.hpp>
#include <Engine/Graphic/VirtualGeometry/VirtualGeometryLayout.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Runtime/Services/Shader/ShaderService.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <format>
#include <string>

namespace Desert::Graphic::VirtualGeometry
{
    static_assert( sizeof( GpuClusterCandidate ) == GpuDriven::kVirtualGeometryCandidateBytes,
                   "InstanceCulling sizes the candidate rows it appends by kVirtualGeometryCandidateBytes" );

    namespace
    {
        // The slot names of Programs/VirtualGeometry/ClusterCut.shader, spelled once (RDGShaderParameters.hpp).
        struct ClusterCutShaderParameters
        {
            DESERT_SHADER_PARAMETER( StorageBuffer, ClusterCutPrimitives );
            DESERT_SHADER_PARAMETER( StorageBuffer, ClusterCutInstances );
            DESERT_SHADER_PARAMETER( StorageBuffer, ClusterCutPassPrimitiveDraws );
            DESERT_SHADER_PARAMETER( StorageBuffer, ClusterCutPassDrawSlots );
            DESERT_SHADER_PARAMETER( StorageBuffer, ClusterCutSubmeshTransforms );
            DESERT_SHADER_PARAMETER( StorageBuffer, ClusterCutDags );
            DESERT_SHADER_PARAMETER( StorageBuffer, ClusterCutClusters );
            DESERT_SHADER_PARAMETER( StorageBuffer, ClusterCutHierarchy );
            DESERT_SHADER_PARAMETER( StorageBuffer, ClusterCutCandidates );
            DESERT_SHADER_PARAMETER( StorageBuffer, ClusterCutViews );
            DESERT_SHADER_PARAMETER( StorageBuffer, ClusterCutViewLods );
            DESERT_SHADER_PARAMETER( StorageBuffer, ClusterCutCandidateCount );
            DESERT_SHADER_PARAMETER( StorageBuffer, ClusterCutVisibleClusters );
            DESERT_SHADER_PARAMETER( StorageBuffer, ClusterCutRasterArgs );
            DESERT_SHADER_PARAMETER( StorageBuffer, ClusterCutStatus );
            DESERT_SHADER_PARAMETER( StorageBuffer, ClusterCutDispatchArgs );
            DESERT_SHADER_PARAMETER( StorageBuffer, ClusterCutOccludedCandidates );
            DESERT_SHADER_PARAMETER( StorageBuffer, ClusterCutOccludedCount );
            DESERT_SHADER_PARAMETER( StorageBuffer, ClusterCutVsmDirtyMask );
            DESERT_SHADER_PARAMETER( Sampler, PointClampSampler );
            DESERT_SHADER_PARAMETER( Texture, ClusterCutHzbTexture );
        };

        // The slot names of Programs/VirtualGeometry/RasterBinning.shader.
        struct RasterBinningShaderParameters
        {
            DESERT_SHADER_PARAMETER( StorageBuffer, RasterBinningInstances );
            DESERT_SHADER_PARAMETER( StorageBuffer, RasterBinningPrimitiveDraws );
            DESERT_SHADER_PARAMETER( StorageBuffer, RasterBinningDrawSlots );
            DESERT_SHADER_PARAMETER( StorageBuffer, RasterBinningDrawBins );
            DESERT_SHADER_PARAMETER( StorageBuffer, RasterBinningDags );
            DESERT_SHADER_PARAMETER( StorageBuffer, RasterBinningClusters );
            DESERT_SHADER_PARAMETER( StorageBuffer, RasterBinningVisibleClusters );
            DESERT_SHADER_PARAMETER( StorageBuffer, RasterBinningRasterArgs );
            DESERT_SHADER_PARAMETER( StorageBuffer, RasterBinningGroupArgs );
            DESERT_SHADER_PARAMETER( StorageBuffer, RasterBinningGroupCursor );
            DESERT_SHADER_PARAMETER( StorageBuffer, RasterBinningRows );
            DESERT_SHADER_PARAMETER( StorageBuffer, RasterBinningGroupViewCounts );
        };

        constexpr const char* kClusterCutShader = "ClusterCut"; // Programs/VirtualGeometry/ClusterCut.shader
        constexpr const char* kRasterBinningShader =
             "RasterBinning"; // Programs/VirtualGeometry/RasterBinning.shader
        constexpr uint32_t kBinStepCount   = 0u;
        constexpr uint32_t kBinStepOffsets = 1u;
        constexpr uint32_t kBinStepScatter = 2u;
        constexpr uint32_t kBinGroupSize   = 64u; // RasterBinning.shader LocalSize
        constexpr uint32_t kStepArgs       = 0u;
        constexpr uint32_t kStepCut        = 1u;

        // ClusterCut.shader's push block.
        struct CutPush
        {
            glm::uvec4 Params{
                 0u }; // step, visible capacity, primitive rows of the pass table, candidate rows per region
            glm::uvec4 Params2{ 0u }; // x = 1 when a HZB is bound, y = occluded carry capacity, z = 1 post phase
            glm::uvec4 Params3{ 0u }; // x = first cut view of the dispatch, y = its views
        };

        template <typename T>
        std::span<const std::byte> Bytes( const std::vector<T>& values )
        {
            return std::as_bytes( std::span( values ) );
        }

        RDG::BufferRef Upload( RDG::Builder& graph, const std::string& name, std::span<const std::byte> bytes )
        {
            const RDG::BufferRef buffer = graph.CreateBuffer( RDG::BufferDesc{ .Bytes = bytes.size() }, name );
            graph.QueueBufferUpload( buffer, bytes );
            return buffer;
        }
    } // namespace

    struct VirtualGeometryRenderer::Programs
    {
        struct Program
        {
            std::shared_ptr<Shader>          Shader;
            std::shared_ptr<ComputePipeline> Pipeline;
            mutable ShaderBindingLayoutCache Layout;

            Common::BoolResultStr Ensure( const char* name, const char* path )
            {
                if ( Pipeline )
                    return BOOLSUCCESS;
                Runtime::ShaderService* service = Runtime::ResourceRegistry::GetShaderService();
                Shader                          = service ? service->GetByName( std::string( name ) ) : nullptr;
                if ( !Shader )
                    return Common::MakeError(
                         std::format( "the shader '{}' did not compile - expected {}", name, path ) );
                const auto built =
                     ComputePipeline::Create( { .Shader = Shader, .DebugName = std::string( name ) } );
                if ( !built )
                    return Common::MakeError(
                         std::format( "the '{}' pipeline was not built: {}", name, built.GetError() ) );
                Pipeline = built.GetValue();
                return BOOLSUCCESS;
            }
        };

        Program ClusterCut;
        Program RasterBinning;
    };

    VirtualGeometryRenderer::VirtualGeometryRenderer() : m_Programs( std::make_unique<Programs>() )
    {
    }
    VirtualGeometryRenderer::~VirtualGeometryRenderer() = default;

    namespace
    {
        // Everything both ClusterCut steps bind (every slot of the layout is bound in each pass).
        struct CutBinds
        {
            RDG::BufferRef PrimitiveDraws, DrawSlots, SubmeshTransforms;
            RDG::BufferRef Candidates, CandidateCount, Views, ViewLods;
            RDG::BufferRef Visible, RasterArgs, Status;
            RDG::BufferRef DispatchArgs; // step 0 writes it, step 1 reads it as its indirect arguments
            RDG::BufferRef ArgsSlot;     // step 1's DispatchArgs slot: an unread buffer
            RDG::BufferRef OccludedCandidates, OccludedCount; // the main phase's post carry (post: unread slots)
            RDG::TextureRef
                 Hzb; // the views' HZB of the phase (previous frame / this frame), or the neutral texture
            RDG::BufferRef VsmDirtyMask; // the VSM levels' "VSM: Dirty Mask", or a one-word stand-in (no level)
        };

        void BindCut( RDG::PassBuilder& pass, const VirtualGeometryRenderer::Programs::Program& program,
                      const GpuDriven::GpuSceneBuffers& scene, const VirtualMeshBuffers& meshes, const CutBinds& b,
                      const bool indirect )
        {
            using P            = ClusterCutShaderParameters;
            constexpr auto R   = RDG::Access::StorageRead;
            constexpr auto W   = RDG::Access::StorageWrite;
            auto           blk = pass.Bindings( program.Layout.Get( program.Shader ),
                                                Renderer::GetPipelineRouteFill( *program.Pipeline ) );
            blk.Storage( P::ClusterCutPrimitives, scene.Primitives, R )
                 .Storage( P::ClusterCutInstances, scene.Instances, R )
                 .Storage( P::ClusterCutPassPrimitiveDraws, b.PrimitiveDraws, R )
                 .Storage( P::ClusterCutPassDrawSlots, b.DrawSlots, R )
                 .Storage( P::ClusterCutSubmeshTransforms, b.SubmeshTransforms, R )
                 .Storage( P::ClusterCutDags, meshes.Dags, R )
                 .Storage( P::ClusterCutClusters, meshes.Clusters, R )
                 .Storage( P::ClusterCutHierarchy, meshes.Hierarchy, R )
                 .Storage( P::ClusterCutCandidates, b.Candidates, R )
                 .Storage( P::ClusterCutViews, b.Views, R )
                 .Storage( P::ClusterCutViewLods, b.ViewLods, R )
                 .Storage( P::ClusterCutCandidateCount, b.CandidateCount, R )
                 .Storage( P::ClusterCutVisibleClusters, b.Visible, W )
                 .Storage( P::ClusterCutRasterArgs, b.RasterArgs, W )
                 .Storage( P::ClusterCutStatus, b.Status, W )
                 .Storage( P::ClusterCutOccludedCandidates, b.OccludedCandidates, W )
                 .Storage( P::ClusterCutOccludedCount, b.OccludedCount, W )
                 .Storage( P::ClusterCutVsmDirtyMask, b.VsmDirtyMask, R )
                 .Sampler( P::PointClampSampler, RDG::SamplerDesc::PointClamp() )
                 .Texture( P::ClusterCutHzbTexture, b.Hzb, RDG::Access::SampledCompute,
                           RDG::SubresourceRange::All() )
                 .PushConstantBytes( static_cast<uint32_t>( sizeof( CutPush ) ) );
            if ( indirect )
            {
                pass.Read( b.DispatchArgs, RDG::Access::IndirectArgs );
                blk.Storage( P::ClusterCutDispatchArgs, b.ArgsSlot, W );
            }
            else
                blk.Storage( P::ClusterCutDispatchArgs, b.DispatchArgs, W );
        }
    } // namespace

    Common::ResultStr<ClusterCullResult> VirtualGeometryRenderer::AddClusterCull(
         RDG::Builder& graph, const GpuDriven::CullPhase phase, const GpuDriven::GpuSceneBuffers& scene,
         const VirtualMeshBuffers& meshes, const GpuDriven::PassDrawTable& passTable,
         const GpuDriven::PassDrawBuffers& passSlots, const RDG::BufferRef submeshTransforms,
         const ClusterCandidates& candidates, const std::span<const ClusterCullView> views,
         const uint32_t firstView, const RDG::TextureRef neutralHzb ) const
    {
        using Result    = ClusterCullResult;
        const bool post = phase == GpuDriven::CullPhase::PostOcclusion;
        if ( views.empty() )
            return Common::MakeError<Result>( "VirtualGeometry: Cluster Cut: no view to cut for" );
        const uint32_t pass = views.front().Instance.View.Pass;
        for ( const ClusterCullView& view : views )
            if ( view.Instance.View.Pass != pass )
                return Common::MakeError<Result>( std::format(
                     "VirtualGeometry: Cluster Cut: views of mesh passes {} and {} in one dispatch (one pass "
                     "table per dispatch)",
                     pass, view.Instance.View.Pass ) );
        if ( passTable.PrimitiveDraws.empty() || !passSlots.PrimitiveDraws.IsValid() ||
             !passSlots.DrawSlots.IsValid() || !submeshTransforms.IsValid() )
            return Common::MakeError<Result>( std::format(
                 "VirtualGeometry: Cluster Cut: mesh pass {} has no draw table (PrimitiveDraws {} rows / {}, "
                 "DrawSlots {}, SubmeshTransforms {})",
                 pass, passTable.PrimitiveDraws.size(), passSlots.PrimitiveDraws.IsValid() ? "valid" : "invalid",
                 passSlots.DrawSlots.IsValid() ? "valid" : "invalid",
                 submeshTransforms.IsValid() ? "valid" : "invalid" ) );
        // One HZB per dispatch (the instance cull's rule): the views' previous-frame furthest HZB (Main), this
        // frame's (PostOcclusion: required, it is what the phase re-tests against).
        RDG::TextureRef hzb{};
        for ( const ClusterCullView& view : views )
        {
            const RDG::TextureRef viewHzb = post ? view.Instance.Hzb : view.Instance.PrevHzb;
            if ( !viewHzb.IsValid() )
                continue;
            if ( hzb.IsValid() && hzb != viewHzb )
                return Common::MakeError<Result>(
                     "VirtualGeometry: Cluster Cut: views with two HZBs in one dispatch" );
            hzb = viewHzb;
        }
        // One drawn-page mask per dispatch: a VSM level's view (kVsmPageMask) culls its nodes and clusters against
        // the frame's "VSM: Dirty Mask" (VirtualShadowMapFrame::DirtyMask, every level's words in one buffer).
        RDG::BufferRef vsmDirtyMask{};
        for ( const ClusterCullView& view : views )
        {
            if ( ( view.Instance.View.Flags & GpuDriven::CullViewFlags::kVsmPageMask ) == 0u )
                continue;
            if ( !view.Instance.VsmDirtyMask.IsValid() )
                return Common::MakeError<Result>(
                     std::format( "VirtualGeometry: Cluster Cut: VSM level {} has no drawn-page mask",
                                  view.Instance.View.VsmLevel ) );
            if ( vsmDirtyMask.IsValid() && vsmDirtyMask != view.Instance.VsmDirtyMask )
                return Common::MakeError<Result>(
                     "VirtualGeometry: Cluster Cut: VSM levels with two drawn-page masks in one dispatch" );
            vsmDirtyMask = view.Instance.VsmDirtyMask;
        }
        const bool hasHzb = hzb.IsValid();
        if ( post && !hasHzb )
            return Common::MakeError<Result>(
                 "VirtualGeometry: Cluster Cut: the PostOcclusion phase has no view with this frame's HZB" );
        if ( !hasHzb && !neutralHzb.IsValid() )
            return Common::MakeError<Result>(
                 "VirtualGeometry: Cluster Cut: no HZB and no neutral texture to bind" );
        if ( !meshes.Dags.IsValid() || !meshes.Clusters.IsValid() || !meshes.Hierarchy.IsValid() )
            return Common::MakeError<Result>( "VirtualGeometry: Cluster Cut: the VirtualMeshTable has no tables" );
        if ( !candidates.Rows.IsValid() || !candidates.Count.IsValid() || candidates.Capacity == 0u ||
             candidates.VisibleCapacity == 0u )
            return Common::MakeError<Result>( std::format(
                 "VirtualGeometry: Cluster Cut: no candidates (Rows {}, Count {}, capacity {}, visible {})",
                 candidates.Rows.IsValid() ? "valid" : "invalid", candidates.Count.IsValid() ? "valid" : "invalid",
                 candidates.Capacity, candidates.VisibleCapacity ) );
        // The layout follows the phase: the main cut reads InstanceCulling's per-cut-view regions, the post cut a
        // main cut's carry (ClusterCandidateLayout).
        const ClusterCandidateLayout expected =
             post ? ClusterCandidateLayout::PostCarry : ClusterCandidateLayout::PerCutView;
        if ( candidates.Layout != expected )
            return Common::MakeError<Result>( std::format(
                 "VirtualGeometry: Cluster Cut: the {} phase reads {} candidates, got {}", post ? "post" : "main",
                 post ? "post-carry" : "per-cut-view",
                 candidates.Layout == ClusterCandidateLayout::PostCarry ? "post-carry" : "per-cut-view" ) );
        if ( !post && uint64_t{ firstView } + views.size() > candidates.CutViews )
            return Common::MakeError<Result>(
                 std::format( "VirtualGeometry: Cluster Cut: cut views [{}, {}) past the candidates' {} regions",
                              firstView, uint64_t{ firstView } + views.size(), candidates.CutViews ) );
        // The visible list: the caller's exact bound, capped by the dispatch's budget (kMaxVisibleClustersPerCut,
        // UE MaxVisibleClusters). A cluster past it is counted (ClusterCutStatus::VisibleOverflow), not emitted.
        const uint32_t visibleCapacity = std::min( candidates.VisibleCapacity, kMaxVisibleClustersPerCut );
        if ( const Common::BoolResultStr ready = m_Programs->ClusterCut.Ensure(
                  kClusterCutShader, "Editor/Resources/Shaders/Programs/VirtualGeometry/ClusterCut.shader" );
             !ready )
            return Common::MakeError<Result>(
                 std::format( "VirtualGeometry: Cluster Cut: {}", ready.GetError() ) );

        // The view rows (GpuClusterCandidate::View indexes them) and their LOD terms.
        std::vector<GpuDriven::GpuCullView> rows;
        std::vector<glm::vec4>              lods;
        rows.reserve( views.size() );
        lods.reserve( views.size() );
        for ( uint32_t v = 0; v < views.size(); ++v )
        {
            const ClusterCullView& view = views[v];
            if ( !( view.Lod.LodScale > 0.0f ) || view.MaxPixelsPerEdge <= 0.0f )
                return Common::MakeError<Result>( std::format(
                     "VirtualGeometry: Cluster Cut: view {} has LOD scale {} ({}) / {} pixels per edge", v,
                     view.Lod.LodScale, view.Lod.Orthographic ? "orthographic" : "perspective",
                     view.MaxPixelsPerEdge ) );
            GpuDriven::GpuCullView row = view.Instance.View;
            for ( uint32_t plane = 0; plane < 6u; ++plane )
            {
                const Core::Frustum::Plane& fp =
                     view.Instance.Frustum.GetPlane( static_cast<Core::FrustumPlane>( plane ) );
                row.FrustumPlanes[plane] = glm::vec4( fp.Normal, fp.Distance );
            }
            row.ViewIndex = v;
            rows.push_back( row );
            // In the view's own pixels (ClusterLodMeasure): perspective length x lodScale / d, orthographic
            // length x lodScale (a cascade's texels).
            lods.emplace_back( view.Lod.LodScale, view.MaxPixelsPerEdge, view.Lod.Orthographic ? 1.0f : 0.0f,
                               0.0f );
        }

        CutBinds b;
        b.PrimitiveDraws    = passSlots.PrimitiveDraws;
        b.DrawSlots         = passSlots.DrawSlots;
        b.SubmeshTransforms = submeshTransforms;
        b.Candidates        = candidates.Rows;
        b.CandidateCount    = candidates.Count;
        b.Views             = Upload( graph, "VirtualGeometry.CutViews", Bytes( rows ) );
        b.ViewLods          = Upload( graph, "VirtualGeometry.CutViewLods", Bytes( lods ) );
        b.Visible           = graph.CreateBuffer(
             RDG::BufferDesc{ .Bytes = uint64_t{ visibleCapacity } * sizeof( GpuVisibleCluster ) },
             "VirtualGeometry.VisibleClusters" );
        // HwDraw: kClusterMaxTriangles * 3 vertices per HW cluster instance, instances counted by the cut.
        GpuClusterRasterArgs args;
        args.HwDraw[0] = Geometry::kClusterMaxTriangles * 3u;
        b.RasterArgs   = Upload( graph, "VirtualGeometry.RasterArgs", std::as_bytes( std::span( &args, 1 ) ) );
        const ClusterCutStatus status{};
        b.Status = Upload( graph, "VirtualGeometry.CutStatus", std::as_bytes( std::span( &status, 1 ) ) );
        const std::vector<uint32_t> zeros( 4u, 0u );
        b.DispatchArgs = Upload( graph, "VirtualGeometry.CutDispatchArgs", Bytes( zeros ) );
        b.ArgsSlot     = Upload( graph, "VirtualGeometry.CutArgsSlot", Bytes( zeros ) );
        b.Hzb          = hasHzb ? hzb : neutralHzb;
        const std::vector<uint32_t> noDirtyMask( 1u, 0u );
        b.VsmDirtyMask = vsmDirtyMask.IsValid()
                              ? vsmDirtyMask
                              : Upload( graph, "VirtualGeometry.CutVsmDirtyMaskSlot", Bytes( noDirtyMask ) );
        // The post carry (ClusterCandidateLayout::PostCarry): every entry the main phase finds occluded is a
        // disjoint subtree or a cluster of a candidate's cut, so the visible capacity bounds each region: node
        // children from row 0, clusters from row carryCapacity (packed 64 per post workgroup).
        const uint32_t carryCapacity = !post && hasHzb ? visibleCapacity : 0u;
        b.OccludedCount              = Upload(
             graph, post ? "VirtualGeometry.CutOccludedSlot" : "VirtualGeometry.OccludedCount", Bytes( zeros ) );
        b.OccludedCandidates = graph.CreateBuffer(
             RDG::BufferDesc{ .Bytes = uint64_t{ std::max( carryCapacity, 1u ) } * 2u *
                                       sizeof( GpuClusterCandidate ) },
             post ? "VirtualGeometry.CutOccludedRowsSlot" : "VirtualGeometry.OccludedCandidates" );

        const Programs::Program& program       = m_Programs->ClusterCut;
        const auto               primitiveRows = static_cast<uint32_t>( passTable.PrimitiveDraws.size() );
        for ( const uint32_t step : { kStepArgs, kStepCut } )
        {
            const bool    indirect = step == kStepCut;
            const CutPush push{ glm::uvec4( step, visibleCapacity, primitiveRows, candidates.Capacity ),
                                glm::uvec4( hasHzb ? 1u : 0u, carryCapacity, post ? 1u : 0u, 0u ),
                                glm::uvec4( firstView, static_cast<uint32_t>( views.size() ), 0u, 0u ) };
            graph.AddPass(
                 post ? ( indirect ? "VirtualGeometry: Cluster Cut Post"
                                   : "VirtualGeometry: Cluster Cut Post Args" )
                      : ( indirect ? "VirtualGeometry: Cluster Cut" : "VirtualGeometry: Cluster Cut Args" ),
                 RDG::PassFlags::Compute,
                 [&]( RDG::PassBuilder& builder ) { BindCut( builder, program, scene, meshes, b, indirect ); },
                 [pipeline     = program.Pipeline, push, indirect,
                  dispatchArgs = b.DispatchArgs]( RDG::PassContext& context ) -> Common::BoolResultStr
                 {
                     RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                     bindings.PushConstants( &push, static_cast<uint32_t>( sizeof( push ) ) );
                     if ( indirect )
                         return Renderer::DispatchComputeIndirect( bindings, *pipeline, dispatchArgs, 0u );
                     return Renderer::DispatchCompute( bindings, *pipeline, 1u, 1u, 1u );
                 } );
        }

        ClusterCullResult result;
        result.VisibleClusters = b.Visible;
        result.RasterArgs      = b.RasterArgs;
        result.Status          = b.Status;
        result.VisibleCapacity = visibleCapacity;
        if ( carryCapacity != 0u )
        {
            result.OccludedCandidates = b.OccludedCandidates;
            result.OccludedCount      = b.OccludedCount;
        }
        return Common::MakeSuccess( std::move( result ) );
    }

    Common::ResultStr<RasterBins> VirtualGeometryRenderer::AddRasterBinning(
         RDG::Builder& graph, const GpuDriven::GpuSceneBuffers& scene, const VirtualMeshBuffers& meshes,
         const GpuDriven::PassDrawTable& passTable, const GpuDriven::PassDrawBuffers& passSlots,
         const ClusterCullResult& culled, const uint32_t countViews ) const
    {
        using Result = RasterBins;
        if ( passTable.Groups.empty() || passTable.Bins.empty() || passTable.DrawSlots.empty() )
            return Common::MakeError<Result>(
                 std::format( "VirtualGeometry: Raster Binning: the pass has {} groups, {} bins, {} draw slots",
                              passTable.Groups.size(), passTable.Bins.size(), passTable.DrawSlots.size() ) );
        if ( !passSlots.PrimitiveDraws.IsValid() || !passSlots.DrawSlots.IsValid() || !passSlots.Bins.IsValid() )
            return Common::MakeError<Result>(
                 "VirtualGeometry: Raster Binning: the pass's draw tables are not uploaded" );
        if ( !culled.VisibleClusters.IsValid() || !culled.RasterArgs.IsValid() || culled.VisibleCapacity == 0u )
            return Common::MakeError<Result>( "VirtualGeometry: Raster Binning: no cluster cut to bin" );
        if ( !meshes.Dags.IsValid() || !meshes.Clusters.IsValid() )
            return Common::MakeError<Result>(
                 "VirtualGeometry: Raster Binning: the VirtualMeshTable has no tables" );
        if ( const Common::BoolResultStr ready = m_Programs->RasterBinning.Ensure(
                  kRasterBinningShader, "Editor/Resources/Shaders/Programs/VirtualGeometry/RasterBinning.shader" );
             !ready )
            return Common::MakeError<Result>(
                 std::format( "VirtualGeometry: Raster Binning: {}", ready.GetError() ) );

        const auto groupCount = static_cast<uint32_t>( passTable.Groups.size() );
        RasterBins bins;
        bins.GroupCount      = groupCount;
        bins.VisibleClusters = culled.VisibleClusters;
        // COUNT adds into instanceCount: every group starts at zero.
        const std::vector<uint32_t> zeros( std::size_t{ groupCount } * 4u, 0u );
        bins.GroupArgs = Upload( graph, "VirtualGeometry.RasterGroupArgs", Bytes( zeros ) );
        // COUNT adds per (group, view) too: zeros, one placeholder entry when nothing is counted per view.
        bins.CountViews = countViews;
        const std::vector<uint32_t> viewZeros(
             std::max<std::size_t>( 1u, std::size_t{ groupCount } * std::size_t{ countViews } ), 0u );
        bins.GroupViewCounts = Upload( graph, "VirtualGeometry.RasterGroupViewCounts", Bytes( viewZeros ) );
        const RDG::BufferRef cursor =
             graph.CreateBuffer( RDG::BufferDesc{ .Bytes = uint64_t{ groupCount } * sizeof( uint32_t ) },
                                 "VirtualGeometry.RasterGroupCursor" );
        bins.Rows = graph.CreateBuffer(
             RDG::BufferDesc{ .Bytes = uint64_t{ culled.VisibleCapacity } * sizeof( uint32_t ) },
             "VirtualGeometry.RasterRows" );

        using P                            = RasterBinningShaderParameters;
        const Programs::Program& program   = m_Programs->RasterBinning;
        const auto               primitive = static_cast<uint32_t>( passTable.PrimitiveDraws.size() );
        const auto               slots     = static_cast<uint32_t>( passTable.DrawSlots.size() );
        const auto               binRows   = static_cast<uint32_t>( passTable.Bins.size() );
        const uint32_t           threads   = ( culled.VisibleCapacity + kBinGroupSize - 1u ) / kBinGroupSize;
        for ( const uint32_t step : { kBinStepCount, kBinStepOffsets, kBinStepScatter } )
        {
            const CutPush  push{ glm::uvec4( step, culled.VisibleCapacity, groupCount, primitive ),
                                glm::uvec4( slots, binRows, Geometry::kClusterMaxTriangles * 3u, countViews ) };
            const uint32_t groupsX = step == kBinStepOffsets ? 1u : threads;
            graph.AddPass(
                 step == kBinStepCount     ? "VirtualGeometry: Raster Binning Count"
                 : step == kBinStepOffsets ? "VirtualGeometry: Raster Binning Offsets"
                                           : "VirtualGeometry: Raster Binning Scatter",
                 RDG::PassFlags::Compute,
                 [&]( RDG::PassBuilder& builder )
                 {
                     constexpr auto R   = RDG::Access::StorageRead;
                     constexpr auto W   = RDG::Access::StorageWrite;
                     auto           blk = builder.Bindings( program.Layout.Get( program.Shader ),
                                                            Renderer::GetPipelineRouteFill( *program.Pipeline ) );
                     blk.Storage( P::RasterBinningInstances, scene.Instances, R )
                          .Storage( P::RasterBinningPrimitiveDraws, passSlots.PrimitiveDraws, R )
                          .Storage( P::RasterBinningDrawSlots, passSlots.DrawSlots, R )
                          .Storage( P::RasterBinningDrawBins, passSlots.Bins, R )
                          .Storage( P::RasterBinningDags, meshes.Dags, R )
                          .Storage( P::RasterBinningClusters, meshes.Clusters, R )
                          .Storage( P::RasterBinningVisibleClusters, culled.VisibleClusters, R )
                          .Storage( P::RasterBinningRasterArgs, culled.RasterArgs, R )
                          .Storage( P::RasterBinningGroupArgs, bins.GroupArgs, W )
                          .Storage( P::RasterBinningGroupCursor, cursor, W )
                          .Storage( P::RasterBinningRows, bins.Rows, W )
                          .Storage( P::RasterBinningGroupViewCounts, bins.GroupViewCounts, W )
                          .PushConstantBytes( static_cast<uint32_t>( sizeof( CutPush ) ) );
                 },
                 [pipeline = program.Pipeline, push, groupsX]( RDG::PassContext& context ) -> Common::BoolResultStr
                 {
                     RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                     bindings.PushConstants( &push, static_cast<uint32_t>( sizeof( push ) ) );
                     return Renderer::DispatchCompute( bindings, *pipeline, groupsX, 1u, 1u );
                 } );
        }
        return Common::MakeSuccess( std::move( bins ) );
    }

    // --- ClusterCutCounts ------------------------------------------------------------------------------------

    void ClusterCutCounts::BeginFrame( const uint64_t frameIndex )
    {
        const uint32_t slots = std::max( 1u, EngineContext::GetInstance().GetMaxFramesInFlight() );
        if ( m_Slots.size() != slots )
        {
            m_Slots.clear();
            m_Slots.resize( slots );
        }
        Slot& slot = m_Slots[static_cast<std::size_t>( frameIndex % slots )];
        Count( slot );
        slot      = Slot{};
        m_Current = &slot;
    }

    void ClusterCutCounts::Add( RDG::Builder& graph, const ClusterCutKind kind, const ClusterCullResult& culled,
                                const RasterBins* bins, std::vector<uint8_t> drawnGroups )
    {
        if ( m_Current == nullptr )
        {
            LOG_ERROR( "[VirtualGeometry] Cluster Cut counts: a cut handed before BeginFrame" );
            return;
        }
        if ( !culled.RasterArgs.IsValid() || !culled.Status.IsValid() )
        {
            LOG_ERROR( "[VirtualGeometry] Cluster Cut counts: the cut left no RasterArgs / Status to read" );
            return;
        }
        Entry& entry = m_Current->Entries.emplace_back();
        entry.Kind   = kind;
        graph.Extract( culled.RasterArgs, entry.RasterArgs, RDG::Access::HostRead );
        graph.Extract( culled.Status, entry.Status, RDG::Access::HostRead );
        if ( bins != nullptr && bins->GroupArgs.IsValid() && drawnGroups.size() == bins->GroupCount )
        {
            graph.Extract( bins->GroupArgs, entry.GroupArgs, RDG::Access::HostRead );
            entry.DrawnGroups = std::move( drawnGroups );
            if ( bins->CountViews > 0u && bins->GroupViewCounts.IsValid() )
            {
                graph.Extract( bins->GroupViewCounts, entry.GroupViewCounts, RDG::Access::HostRead );
                entry.CountViews = bins->CountViews;
            }
        }
    }

    void ClusterCutCounts::Forget()
    {
        m_Slots.clear();
        m_Current = nullptr;
        m_Last    = ClusterCutCount{};
    }

    void ClusterCutCounts::Count( const Slot& slot )
    {
        if ( slot.Entries.empty() )
            return; // the slot has not been handed a frame yet
        ClusterCutCount count;
        std::string     message;
        const auto      fail = [&message]( std::string what )
        { message += std::format( "{}{}", message.empty() ? "" : "; ", what ); };
        for ( const Entry& entry : slot.Entries )
        {
            const char* const kind = entry.Kind == ClusterCutKind::Main            ? "main"
                                     : entry.Kind == ClusterCutKind::PostOcclusion ? "post-occlusion"
                                     : entry.Kind == ClusterCutKind::ShadowCascade ? "shadow cascade"
                                                                                   : "virtual shadow level";
            if ( !entry.RasterArgs.Physical || !entry.Status.Physical )
            {
                fail( std::format( "the {} cut's readback was not executed", kind ) );
                continue;
            }
            const std::span<const std::byte> args   = entry.RasterArgs.Physical->ReadForHost();
            const std::span<const std::byte> status = entry.Status.Physical->ReadForHost();
            if ( args.size() < sizeof( GpuClusterRasterArgs ) || status.size() < sizeof( ClusterCutStatus ) )
            {
                fail( std::format( "the {} cut's readback holds {} / {} bytes, RasterArgs / Status need {} / {}",
                                   kind, args.size(), status.size(), sizeof( GpuClusterRasterArgs ),
                                   sizeof( ClusterCutStatus ) ) );
                continue;
            }
            GpuClusterRasterArgs raster;
            std::memcpy( &raster, args.data(), sizeof( raster ) );
            ClusterCutStatus cut;
            std::memcpy( &cut, status.data(), sizeof( cut ) );
            if ( entry.Kind == ClusterCutKind::Main )
            {
                count.ClustersVisible = raster.HwDraw[1];
                count.Status          = cut;
            }

            // Drawn: the GroupArgs instance counts of the groups the pass drew.
            uint64_t drawn = 0;
            if ( entry.GroupArgs.Physical )
            {
                const std::span<const std::byte> groups = entry.GroupArgs.Physical->ReadForHost();
                const size_t rows = std::min( entry.DrawnGroups.size(), groups.size() / kRasterBinsArgsStride );
                for ( size_t g = 0; g < rows; ++g )
                {
                    if ( entry.DrawnGroups[g] == 0u )
                        continue;
                    uint32_t instances = 0; // VkDrawIndirectCommand::instanceCount
                    std::memcpy( &instances, groups.data() + g * kRasterBinsArgsStride + sizeof( uint32_t ),
                                 sizeof( instances ) );
                    drawn += instances;
                }
            }
            const auto clamped = static_cast<uint32_t>( std::min<uint64_t>( drawn, UINT32_MAX ) );
            if ( entry.Kind == ClusterCutKind::ShadowCascade )
                count.ShadowClustersDrawn += clamped;
            else if ( entry.Kind == ClusterCutKind::VirtualShadow )
            {
                // Per level: the drawn groups' GroupViewCounts (cut view = level).
                if ( entry.GroupViewCounts.Physical && entry.CountViews > 0u )
                {
                    const std::span<const std::byte> perView = entry.GroupViewCounts.Physical->ReadForHost();
                    const size_t                     views   = entry.CountViews;
                    const size_t rows = std::min( entry.DrawnGroups.size(), perView.size() / ( views * 4u ) );
                    std::vector<uint64_t> levels( views, 0u );
                    for ( size_t g = 0; g < rows; ++g )
                    {
                        if ( entry.DrawnGroups[g] == 0u )
                            continue;
                        for ( size_t v = 0; v < views; ++v )
                        {
                            uint32_t n = 0;
                            std::memcpy( &n, perView.data() + ( g * views + v ) * sizeof( uint32_t ),
                                         sizeof( n ) );
                            levels[v] += n;
                        }
                    }
                    count.VsmClustersDrawn.assign( views, 0u );
                    for ( size_t v = 0; v < views; ++v )
                        count.VsmClustersDrawn[v] =
                             static_cast<uint32_t>( std::min<uint64_t>( levels[v], UINT32_MAX ) );
                }
                else if ( entry.GroupArgs.Physical )
                    fail( std::format( "the {} cut was drawn with no per-level counts", kind ) );
            }
            else
                count.ClustersDrawn += clamped;

            // A failure is named by the mesh its lowest DAG row belongs to (rows outlive a frame-in-flight
            // readback: VirtualMeshTable::kRetireFrames).
            const auto named = [&fail, kind]( const char* what, const uint32_t n, const uint32_t dag )
            {
                if ( n == 0u )
                    return;
                const std::string mesh = VirtualMeshTable::Get().DagMeshName( dag );
                fail( std::format( "{} cut: '{}' (DAG row {}): {} x {}", kind, mesh.empty() ? "<released>" : mesh,
                                   dag, n, what ) );
            };
            named( "hierarchy node dropped by a full traversal stack (ClusterCut.shader kStackDepth)",
                   cut.StackOverflow, cut.StackOverflowDag );
            named( "candidate whose primitive has no lod-0 draw slot for the DAG's submesh in the view's pass",
                   cut.MissingSlot, cut.MissingSlotDag );
            named( "cluster past the visible capacity", cut.VisibleOverflow, cut.VisibleOverflowDag );
            if ( cut.OccludedOverflow != 0u )
                fail( std::format(
                     "{} cut: {} occluded entries past the post carry's capacity (missing this frame)", kind,
                     cut.OccludedOverflow ) );
        }
        m_Last = count;
        if ( !message.empty() && message != m_LastError )
            LOG_ERROR( "[VirtualGeometry] Cluster Cut: {}", message );
        m_LastError = std::move( message );
    }
} // namespace Desert::Graphic::VirtualGeometry
