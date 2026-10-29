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
        };

        constexpr const char* kClusterCutShader = "ClusterCut"; // Programs/VirtualGeometry/ClusterCut.shader
        constexpr uint32_t    kStepArgs         = 0u;
        constexpr uint32_t    kStepCut          = 1u;

        // ClusterCut.shader's push block.
        struct CutPush
        {
            glm::uvec4 Params{
                 0u }; // step, visible capacity, primitive rows of the pass table, candidate capacity
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
         const ClusterCandidates& candidates, const std::span<const ClusterCullView> views ) const
    {
        using Result = ClusterCullResult;
        if ( phase != GpuDriven::CullPhase::Main )
            return Common::MakeError<Result>(
                 "VirtualGeometry: Cluster Cut: the PostOcclusion phase needs the HZB carry (not built yet)" );
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
        if ( !meshes.Dags.IsValid() || !meshes.Clusters.IsValid() || !meshes.Hierarchy.IsValid() )
            return Common::MakeError<Result>( "VirtualGeometry: Cluster Cut: the VirtualMeshTable has no tables" );
        if ( !candidates.Rows.IsValid() || !candidates.Count.IsValid() || candidates.Capacity == 0u ||
             candidates.VisibleCapacity == 0u )
            return Common::MakeError<Result>( std::format(
                 "VirtualGeometry: Cluster Cut: no candidates (Rows {}, Count {}, capacity {}, visible {})",
                 candidates.Rows.IsValid() ? "valid" : "invalid", candidates.Count.IsValid() ? "valid" : "invalid",
                 candidates.Capacity, candidates.VisibleCapacity ) );
        if ( candidates.VisibleCapacity > kMaxVisibleClusters )
            return Common::MakeError<Result>(
                 std::format( "VirtualGeometry: Cluster Cut: {} visible clusters pass the visibility word's {}",
                              candidates.VisibleCapacity, kMaxVisibleClusters ) );
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
            if ( view.ViewHeight == 0u || view.MaxPixelsPerEdge <= 0.0f )
                return Common::MakeError<Result>(
                     std::format( "VirtualGeometry: Cluster Cut: view {} has height {} / {} pixels per edge", v,
                                  view.ViewHeight, view.MaxPixelsPerEdge ) );
            GpuDriven::GpuCullView row = view.Instance.View;
            for ( uint32_t plane = 0; plane < 6u; ++plane )
            {
                const Core::Frustum::Plane& fp =
                     view.Instance.Frustum.GetPlane( static_cast<Core::FrustumPlane>( plane ) );
                row.FrustumPlanes[plane] = glm::vec4( fp.Normal, fp.Distance );
            }
            row.ViewIndex = v;
            rows.push_back( row );
            // UE GetProjectedEdgeScales: a world length at distance d covers length x lodScale / d pixels.
            const float lodScale = 0.5f * static_cast<float>( view.ViewHeight ) * row.LodProjection[1][1];
            lods.emplace_back( lodScale, view.MaxPixelsPerEdge, 0.0f, 0.0f );
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
             RDG::BufferDesc{ .Bytes = uint64_t{ candidates.VisibleCapacity } * sizeof( GpuVisibleCluster ) },
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

        const Programs::Program& program       = m_Programs->ClusterCut;
        const auto               primitiveRows = static_cast<uint32_t>( passTable.PrimitiveDraws.size() );
        for ( const uint32_t step : { kStepArgs, kStepCut } )
        {
            const bool    indirect = step == kStepCut;
            const CutPush push{
                 glm::uvec4( step, candidates.VisibleCapacity, primitiveRows, candidates.Capacity ) };
            graph.AddPass(
                 indirect ? "VirtualGeometry: Cluster Cut" : "VirtualGeometry: Cluster Cut Args",
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
        result.VisibleCapacity = candidates.VisibleCapacity;
        return result;
    }

    // --- ClusterCutCounts ------------------------------------------------------------------------------------

    void ClusterCutCounts::AddFrame( RDG::Builder& graph, const uint64_t frameIndex,
                                     const ClusterCullResult& culled )
    {
        const uint32_t slots = std::max( 1u, EngineContext::GetInstance().GetMaxFramesInFlight() );
        if ( m_Slots.size() != slots )
        {
            m_Slots.clear();
            m_Slots.resize( slots );
        }
        Slot& slot = m_Slots[static_cast<std::size_t>( frameIndex % slots )];
        Count( slot );
        slot = Slot{};
        if ( !culled.RasterArgs.IsValid() || !culled.Status.IsValid() )
        {
            LOG_ERROR( "[VirtualGeometry] Cluster Cut counts: the cut left no RasterArgs / Status to read" );
            return;
        }
        graph.Extract( culled.RasterArgs, slot.RasterArgs, RDG::Access::HostRead );
        graph.Extract( culled.Status, slot.Status, RDG::Access::HostRead );
    }

    void ClusterCutCounts::Forget()
    {
        m_Slots.clear();
        m_Last = ClusterCutCount{};
    }

    void ClusterCutCounts::Count( const Slot& slot )
    {
        if ( !slot.RasterArgs.Physical || !slot.Status.Physical )
            return; // the slot has not been handed a frame yet
        const std::span<const std::byte> args   = slot.RasterArgs.Physical->ReadForHost();
        const std::span<const std::byte> status = slot.Status.Physical->ReadForHost();
        if ( args.size() < sizeof( GpuClusterRasterArgs ) || status.size() < sizeof( ClusterCutStatus ) )
        {
            std::string message =
                 std::format( "the readback holds {} / {} bytes, RasterArgs / Status need {} / {}", args.size(),
                              status.size(), sizeof( GpuClusterRasterArgs ), sizeof( ClusterCutStatus ) );
            if ( message != m_LastError )
                LOG_ERROR( "[VirtualGeometry] Cluster Cut counts: {}", message );
            m_LastError = std::move( message );
            return;
        }
        GpuClusterRasterArgs raster;
        std::memcpy( &raster, args.data(), sizeof( raster ) );
        ClusterCutCount count;
        count.ClustersVisible = raster.HwDraw[1];
        std::memcpy( &count.Status, status.data(), sizeof( count.Status ) );
        m_Last = count;

        // A failure is named by the mesh its lowest DAG row belongs to (rows outlive a frame-in-flight readback:
        // VirtualMeshTable::kRetireFrames). One message per distinct set of failures.
        std::string message;
        const auto  named = [&message]( const char* what, const uint32_t n, const uint32_t dag )
        {
            if ( n == 0u )
                return;
            const std::string mesh = VirtualMeshTable::Get().DagMeshName( dag );
            message += std::format( "{}'{}' (DAG row {}): {} x {}", message.empty() ? "" : "; ",
                                    mesh.empty() ? "<released>" : mesh, dag, n, what );
        };
        named( "hierarchy node dropped by a full traversal stack (ClusterCut.shader kStackDepth)",
               count.Status.StackOverflow, count.Status.StackOverflowDag );
        named( "candidate whose primitive has no lod-0 draw slot for the DAG's submesh in the view's pass",
               count.Status.MissingSlot, count.Status.MissingSlotDag );
        named( "cluster past the visible capacity", count.Status.VisibleOverflow, count.Status.VisibleOverflowDag );
        if ( !message.empty() && message != m_LastError )
            LOG_ERROR( "[VirtualGeometry] Cluster Cut: {}", message );
        m_LastError = std::move( message );
    }
} // namespace Desert::Graphic::VirtualGeometry
