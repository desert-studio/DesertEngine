// MeshRenderer's deferred half: the manual G-buffer pass and its pipeline.
#include "MeshRendererInternal.hpp"
#include <Engine/Graphic/ViewTargetLayouts.hpp>
#include <Engine/Graphic/View/ViewFrame.hpp>

namespace Desert::Graphic::System
{
    void MeshRenderer::DeclareGBufferDraws( RDG::PassBuilder& pass, const SceneViewInputs& view )
    {
        m_GBufferDraws.Clear();
        if ( !m_StaticGBufferPipeline )
            return;
        const auto& gbuffer = m_SceneRenderer != nullptr ? m_SceneRenderer->GetGBuffer() : nullptr;
        if ( !gbuffer || m_SceneRenderer->GetMainCamera() == nullptr )
            return;

        // The graph opens the render pass and clears it to ZERO (SceneRenderer::AddFrameGBuffer). The G-buffer
        // shaders sample one scene input, the rain occlusion map their wetness reads (SurfaceWeather.glslh), bound
        // where a block's shader has the slot; everything else in a block is the material's.
        // Skinned meshes are drawn here too, through the (Skinned x GBuffer) cell (UE's base pass): no forward
        // redraw follows on the deferred path.
        m_GeometryPass = MeshPass::GBuffer;
        // THE DEFERRED EARLY-Z PAIRING (UE r.EarlyZPass; DeferredPrepassHalf): when "Deferred: Masked Depth
        // Prepass" drew this frame's groups, the G-buffer draws them EQUAL through their DepthEqual cells (the Equal
        // half) and every other one as before (the Rest half), each half one call of the same builders.
        if ( m_EarlyZPass != ::Common::Scalability::EarlyZPassMode::Off && PrepassCurrent() )
        {
            m_IsmInstancesDrawn = 0;
            m_DeferredHalf      = DeferredPrepassHalf::Equal;
            BuildStaticDraws( m_GBufferDraws );
            BuildSkinnedDraws( m_GBufferDraws );
            m_DeferredHalf = DeferredPrepassHalf::Rest;
            BuildStaticDraws( m_GBufferDraws );
            BuildSkinnedDraws( m_GBufferDraws );
            m_DeferredHalf = DeferredPrepassHalf::None;
        }
        else
        {
            BuildStaticDraws( m_GBufferDraws );
            BuildSkinnedDraws( m_GBufferDraws );
        }
        m_GeometryPass = MeshPass::Forward;
        // Shader Overrides naming a surface template are surface materials: their (Static x GBuffer) cell, here.
        BuildGenericDraws( m_GBufferDraws, GenericSlice::DeferredSurfaceGBuffer );
        // The bins: this frame's early-Z issue when "Deferred: Masked Depth Prepass" drew some of them
        // (DeclareEarlyZBins: those groups EQUAL through their twins), the view's own otherwise.
        const ViewFrame* const frame = m_SceneRenderer->GetViewFrame();
        if ( m_EarlyZGBufferIssue && frame != nullptr && m_EarlyZBinFrame == frame->FrameIndex )
            m_GBufferDraws.SetBinned( std::move( *m_EarlyZGBufferIssue ) );
        else
            AttachBinned( m_GBufferDraws, GpuDriven::MeshPass::GBuffer, kBinnedMainView );
        m_EarlyZGBufferIssue.reset();
        m_GBufferDraws.Declare( pass, view );
        // The skin cache's streams the (Skinned x GBuffer) cell fetches by address (Vertex_Skinned.glslh): this
        // pass reads them after "SkinCache: Skin" wrote them.
        for ( const RDG::BufferRef& streams : m_SkinCacheReads )
            pass.Read( streams, RDG::Access::StorageRead );
    }

    Common::BoolResultStr MeshRenderer::RenderGBufferManual( const RDG::PassContext& context ) const
    {
        return m_GBufferDraws.Record( context );
    }

    void MeshRenderer::DeclareMaskedDepthDraws( RDG::PassBuilder& pass, const SceneViewInputs& view )
    {
        m_MaskedDepthDraws.Clear();
        m_MaskedPrepassed.clear(); // this frame's: the G-buffer's setup reads it right after
        m_MaskedPrepassedPost.clear(); // phase 2's: "Grass: Depth Prepass Post-Occlusion" refills it this frame
        // This frame's admissions start empty here, whatever returns below: the G-buffer reads only a set stamped
        // with its own frame (PrepassCellDraw), and only when the static / skinned halves below ran.
        m_PrepassCellDraws.clear();
        m_PrepassGpuSections.clear();
        m_PrepassFrame.reset();
        m_EarlyZGBufferIssue.reset();
        m_EarlyZBinFrame.reset();
        m_EarlyZPostGBufferIssue.reset();
        m_EarlyZPostFrame.reset();
        // This frame's grass visibility sections, decided before the prepass and the G-buffer skip them (GRASS-VIS).
        m_GrassVisibilitySections.clear();
        if ( m_GrassVisibilityActive && m_InstancedVisibilityPipeline && m_InstancedVisibilityResolvePipeline &&
             m_SceneRenderer != nullptr && m_SceneRenderer->GetMainCamera() != nullptr )
            for ( const GpuInstancedRenderData& gpu : m_GpuInstancedQueue )
                if ( gpu.Path == MeshVertexPath::GrassBlade && GrassVisibilityReady( gpu ) )
                    m_GrassVisibilitySections.push_back( gpu.Key );
        if ( m_EarlyZPass == ::Common::Scalability::EarlyZPassMode::Off || m_SceneRenderer == nullptr ||
             m_SceneRenderer->GetMainCamera() == nullptr )
            return;
        // The CPU-walked statics (per object, auto-batched, ISMs) and skinned meshes EarlyZAdmits: the Depth half of
        // the pairing, through the forward prepass's (path x Depth) states (the G-buffer's depth format), with both
        // halves' readiness folded and recorded for "Deferred: GBuffer" (m_PrepassCellDraws).
        if ( m_StaticDepthPipeline && m_StaticGBufferEqualPipeline )
        {
            const ViewFrame* const frame = m_SceneRenderer->GetViewFrame();
            m_PrepassFrame = frame != nullptr ? std::optional<uint64_t>( frame->FrameIndex ) : std::nullopt;
            m_GeometryPass = MeshPass::Depth;
            m_DeferredHalf = DeferredPrepassHalf::Depth;
            BuildStaticDraws( m_MaskedDepthDraws );
            BuildSkinnedDraws( m_MaskedDepthDraws );
            m_DeferredHalf = DeferredPrepassHalf::None;
            m_GeometryPass = MeshPass::Forward;
        }
        // The GPU-instanced sections (the landscape grass): paired by BuildGpuInstancedDraws (m_MaskedPrepassed).
        if ( m_InstancedViewDepthPipeline && m_InstancedGBufferEqualPipeline )
            BuildGpuInstancedDraws( m_MaskedDepthDraws, m_InstancedViewDepthPipeline.get(), MeshPass::Depth,
                                    CaptureFrameState( m_SceneRenderer->GetViewFrame() ), nullptr, false );
        // The GPU-driven G-buffer bins EarlyZAdmitsCell: the same cull's records through their depth twins
        // (DeclareEarlyZBins), the G-buffer then drawing them through their equal twins.
        DeclareEarlyZBins();
        m_MaskedDepthDraws.Declare( pass, view );
        // The skin cache's streams the (Skinned x Depth) cell fetches by address (Vertex_Skinned.glslh).
        for ( const RDG::BufferRef& streams : m_SkinCacheReads )
            pass.Read( streams, RDG::Access::StorageRead );
    }

    bool MeshRenderer::EarlyZAdmits( const DataDrivenMaterial* surface ) const
    {
        return surface != nullptr && EarlyZAdmitsCell( surface->GetSchema().Blend, surface->GetShaderName() );
    }

    bool MeshRenderer::EarlyZAdmitsCell( const Core::Formats::SurfaceBlendMode blend,
                                         const std::string_view          cellShader ) const
    {
        using Mode = ::Common::Scalability::EarlyZPassMode;
        if ( Core::Formats::IsTranslucentBlend( blend ) )
            return false;
        // A `.Parallax` surface writes its pixel depth offset in the G-buffer (Pass_GBuffer.glslh gl_FragDepth); the
        // (path x Depth) cells write the geometric depth, so an EQUAL test against them would reject it. It keeps
        // its own depth in the G-buffer until the Depth cells write the same offset (REMAINDER GRASS-VIS-10).
        const bool parallax = IsParallaxCell( BinnedBaseOf( cellShader ) );
        if ( parallax )
            return false;
        switch ( m_EarlyZPass )
        {
            case Mode::All:
                return true;
            case Mode::Masked:
                return ShadowCasterCellFor( blend, parallax ) == ShadowCasterCell::Own; // IsMaskedSurface's rule
            case Mode::Off:
                return false;
        }
        return false;
    }

    bool MeshRenderer::DeferredHalfTakes( const DataDrivenMaterial* source, const PrepassCellKey& key ) const
    {
        using CellDraw = MaterialPipelineTracker::CellDraw;
        switch ( m_DeferredHalf )
        {
            case DeferredPrepassHalf::None:
                return true;
            case DeferredPrepassHalf::Depth:
                return EarlyZAdmits( source );
            case DeferredPrepassHalf::Equal:
                return PrepassCellDraw( key ) == CellDraw::Own;
            case DeferredPrepassHalf::Rest:
                return PrepassCellDraw( key ) != CellDraw::Own;
        }
        return true;
    }

    DataDrivenMaterial* MeshRenderer::DepthEqualCell( const DataDrivenMaterial* built, const MeshVertexPath path ) const
    {
        auto* const materials = Runtime::ResourceRegistry::GetMaterialService();
        if ( materials == nullptr || built == nullptr )
            return nullptr;
        return materials->GetVariant( built, path, MeshPass::GBuffer, SurfaceDepthEqualVariant() );
    }

    std::shared_ptr<GraphicsPipeline> MeshRenderer::EarlyZTwinPipeline( const std::shared_ptr<Shader>& shader,
                                                                        const GraphicsPipeline&        passState,
                                                                        const CullMode                 cull,
                                                                        const std::string&             label )
    {
        GraphicsPipelineSpecification spec = passState.GetSpecification();
        spec.Shader                        = shader;
        spec.CullMode                      = cull; // the group's (BinnedSpecPipeline: a two-sided material's None)
        spec.DebugName                     = std::format( "{} {}", spec.DebugName, shader->GetName() );
        auto built = m_SceneRenderer->GetPipelineCache().GetOrCreateMaterial( spec );
        if ( !built )
        {
            if ( m_BinnedRefused.insert( label + "|earlyz" ).second )
                LOG_ERROR( "[MeshRenderer] early-Z twin '{}' has no pipeline ({}): its bins clip and write their "
                           "depth in the G-buffer",
                           label, built.GetError() );
            return nullptr;
        }
        return built.GetValue();
    }

    std::pair<const GraphicsPipeline*, const GraphicsPipeline*>
    MeshRenderer::EarlyZTwinPairOf( EarlyZTwinPairSlot& slot, const std::string& equalCell,
                                    const std::string& depthCell, const GraphicsPipeline* depthState,
                                    const GraphicsPipeline* equalState, const CullMode cull )
    {
        if ( slot.Refused )
            return {};
        std::shared_ptr<GraphicsPipeline> depth = slot.Depth.lock();
        std::shared_ptr<GraphicsPipeline> equal = slot.Equal.lock();
        if ( !depth || !equal )
        {
            auto* const                   shaders     = Runtime::ResourceRegistry::GetShaderService();
            const std::shared_ptr<Shader> depthShader =
                 shaders != nullptr && !depthCell.empty() ? shaders->GetByName( depthCell ) : nullptr;
            std::shared_ptr<Shader>       equalShader;
            std::string                   refusal = "no shader service";
            if ( shaders != nullptr )
            {
                auto acquired = shaders->AcquireVariant( equalCell, SurfaceDepthEqualVariant() );
                if ( acquired )
                    equalShader = acquired.ExtractValue();
                else
                    refusal = std::string( acquired.GetError() );
            }
            if ( depthState == nullptr || equalState == nullptr || !depthShader || !equalShader )
            {
                slot.Refused = true;
                if ( m_BinnedRefused.insert( equalCell + "|earlyz" ).second )
                {
                    std::string why;
                    if ( depthState == nullptr || equalState == nullptr )
                        why = "no depth / equal pass state for its path";
                    else if ( depthCell.empty() )
                        why = "its template has no (path x Depth) cell";
                    else if ( !depthShader )
                        why = std::format( "no '{}' cell", depthCell );
                    else
                        why = std::format( "no DESERT_SURFACE_DEPTH_EQUAL permutation: {}", refusal );
                    LOG_ERROR( "[MeshRenderer] G-buffer bin '{}' has no early-Z twin ({}): it clips and writes its "
                               "depth in the G-buffer",
                               equalCell, why );
                }
                return {};
            }
            depth = EarlyZTwinPipeline( depthShader, *depthState, cull, depthCell );
            equal = EarlyZTwinPipeline( equalShader, *equalState, cull, equalCell );
            if ( !depth || !equal )
            {
                slot.Refused = true;
                return {};
            }
            slot.Depth = depth;
            slot.Equal = equal;
        }
        if ( depth->GetReadiness() == PipelineReadiness::Failed ||
             equal->GetReadiness() == PipelineReadiness::Failed )
        {
            slot.Refused = true;
            if ( m_BinnedRefused.insert( equalCell + "|earlyz" ).second )
                LOG_ERROR( "[MeshRenderer] the early-Z twins of G-buffer bin '{}' did not compile: it clips and "
                           "writes its depth in the G-buffer",
                           equalCell );
            return {};
        }
        // Both or neither: while either compiles the group draws as before (asked again next frame).
        if ( depth->GetReadiness() != PipelineReadiness::Ready || equal->GetReadiness() != PipelineReadiness::Ready )
            return {};
        return { depth.get(), equal.get() };
    }

    MeshRenderer::EarlyZBinTwins MeshRenderer::EarlyZBinTwinsOf( const GpuDriven::IndirectGroup& group )
    {
        if ( group.Pipeline == nullptr )
            return {};
        const std::shared_ptr<Shader>& cell = group.Pipeline->GetSpecification().Shader;
        if ( !cell || !EarlyZAdmitsCell( cell->GetProgramMeta().Blend, cell->GetName() ) )
            return {};
        EarlyZBinTwinSlot& slot = m_EarlyZBinTwins[group.Pipeline];
        if ( slot.GroupShader.lock() != cell )
            slot = EarlyZBinTwinSlot{ .GroupShader = cell };
        // The pass states of the group's path: the prepass's (path x Depth) (the G-buffer's depth format) and the
        // G-buffer's EQUAL / no-write one.
        const std::string&                  name       = cell->GetName();
        const std::optional<MeshVertexPath> path       = MeshCellPath( name );
        const GraphicsPipeline*             depthState = nullptr;
        const GraphicsPipeline*             equalState = nullptr;
        if ( path == MeshVertexPath::Static )
        {
            depthState = m_StaticDepthPipeline.get();
            equalState = m_StaticGBufferEqualPipeline.get();
        }
        else if ( path == MeshVertexPath::Instanced )
        {
            depthState = m_InstancedDepthPipeline.get();
            equalState = m_InstancedGBufferEqualPipeline.get();
        }
        else if ( path == MeshVertexPath::Skinned )
        {
            depthState = m_SkinnedDepthPipeline.get();
            equalState = m_SkinnedGBufferEqualPipeline.get();
        }
        const std::string_view           templ = std::string_view( name ).substr( 0, name.find( '/' ) );
        const std::optional<std::string> depthCell =
             path ? MeshShaderFor( templ, *path, MeshPass::Depth ) : std::nullopt;
        const std::string depthBinned = depthCell ? BinnedCellOf( *depthCell ) : std::string();
        EarlyZBinTwins    twins;
        const auto [binDepth, binEqual] = EarlyZTwinPairOf( slot.Bins, name, depthBinned, depthState, equalState,
                                                            group.Pipeline->GetSpecification().CullMode );
        twins.Depth = binDepth;
        twins.Equal = binEqual;
        // The group's clusters (its `.VirtualGeometry` twin, ResolveVirtualGeometryPipeline): the same pair over the
        // cluster cells, `<Path>.Depth.Binned.VirtualGeometry` first, then the G-buffer cluster cell EQUAL.
        if ( group.VirtualGeometryPipeline != nullptr && depthCell )
        {
            const std::shared_ptr<Shader>& clusterCell = group.VirtualGeometryPipeline->GetSpecification().Shader;
            if ( clusterCell )
            {
                if ( slot.ClusterShader.lock() != clusterCell )
                {
                    slot.ClusterShader = clusterCell;
                    slot.Clusters      = EarlyZTwinPairSlot{};
                }
                const auto [clusterDepth, clusterEqual] = EarlyZTwinPairOf(
                     slot.Clusters, clusterCell->GetName(), VirtualGeometryCellOf( depthBinned ), depthState,
                     equalState, group.VirtualGeometryPipeline->GetSpecification().CullMode );
                twins.VgDepth = clusterDepth;
                twins.VgEqual = clusterEqual;
            }
        }
        return twins;
    }

    bool MeshRenderer::EarlyZSplit( const MeshRendererDetail::BinnedIssue& source,
                                    const SceneFrameBinding& frameState, MeshRendererDetail::BinnedIssue& depth,
                                    MeshRendererDetail::BinnedIssue&                                gbuffer,
                                    std::unordered_map<const GraphicsPipeline*, EarlyZBinTwinSlot>* kept )
    {
        if ( source.Table == nullptr || ( !source.BinsIssued && !source.VirtualGeometry ) )
            return false;
        const std::vector<GpuDriven::IndirectGroup>& groups = source.Table->Groups;
        // The depth issue: the same cull's records (CulledDraws) and tables, the paired groups only.
        depth                   = MeshRendererDetail::BinnedIssue{};
        depth.Table             = source.Table;
        depth.Culled            = source.Culled;
        depth.Instances         = source.Instances;
        depth.DrawSlots         = source.DrawSlots;
        depth.SubmeshTransforms = source.SubmeshTransforms;
        depth.Mode              = source.Mode;
        depth.GroupCarriers.assign( groups.size(), nullptr );
        depth.GroupPipelines.assign( groups.size(), nullptr );
        // The G-buffer's issue: the source, its paired groups swapped to their equal twins.
        gbuffer = source;
        gbuffer.GroupPipelines.resize( groups.size() );
        for ( size_t group = 0; group < groups.size(); ++group )
            gbuffer.GroupPipelines[group] = source.GroupPipeline( group );
        // The cluster raster's two issues likewise (the same cut's bins and rows).
        std::optional<MeshRendererDetail::VirtualGeometryIssue> depthClusters;
        if ( source.VirtualGeometry )
        {
            depthClusters = *source.VirtualGeometry;
            depthClusters->GroupCarriers.assign( groups.size(), nullptr );
            depthClusters->GroupPipelines.assign( groups.size(), nullptr );
            MeshRendererDetail::VirtualGeometryIssue& clusters = *gbuffer.VirtualGeometry;
            clusters.GroupPipelines.resize( groups.size() );
            for ( size_t group = 0; group < groups.size(); ++group )
                clusters.GroupPipelines[group] = source.VirtualGeometry->GroupPipeline( *source.Table, group );
        }
        bool binsPaired     = false;
        bool clustersPaired = false;
        // Both carriers of a pair, or the pair is not drawn early-Z (said once per message).
        const auto carriers = [&]( const GraphicsPipeline* depthTwin, const GraphicsPipeline* equalTwin )
             -> std::pair<const MaterialExecutor*, const MaterialExecutor*>
        {
            std::string                   error;
            const MaterialExecutor* const depthCarrier =
                 PrepareBinnedCarrier( depthTwin->GetSpecification().Shader, kBinnedMainView, frameState, error );
            const MaterialExecutor* const equalCarrier =
                 depthCarrier != nullptr
                      ? PrepareBinnedCarrier( equalTwin->GetSpecification().Shader, kBinnedMainView, frameState, error )
                      : nullptr;
            if ( depthCarrier == nullptr || equalCarrier == nullptr )
            {
                if ( m_BinnedRefused.insert( error ).second )
                    LOG_ERROR( "[MeshRenderer] {}; that G-buffer bin clips and writes its depth in the G-buffer",
                               error );
                return {};
            }
            return { depthCarrier, equalCarrier };
        };
        for ( size_t group = 0; group < groups.size(); ++group )
        {
            const GpuDriven::IndirectGroup& indirect = groups[group];
            if ( indirect.BinCount == 0u || indirect.Pipeline == nullptr )
                continue;
            // A group this issue already draws through another pipeline is not this split's to pair.
            if ( source.GroupPipeline( group ) != indirect.Pipeline )
                continue;
            const EarlyZBinTwins twins = EarlyZBinTwinsOf( indirect );
            if ( kept != nullptr )
                if ( const auto slot = m_EarlyZBinTwins.find( indirect.Pipeline ); slot != m_EarlyZBinTwins.end() )
                    kept->insert( *slot );
            if ( source.BinsIssued && twins.Depth != nullptr && twins.Equal != nullptr )
                if ( const auto [depthCarrier, equalCarrier] = carriers( twins.Depth, twins.Equal ); depthCarrier )
                {
                    depth.GroupPipelines[group]   = twins.Depth;
                    depth.GroupCarriers[group]    = depthCarrier;
                    gbuffer.GroupPipelines[group] = twins.Equal;
                    gbuffer.GroupCarriers[group]  = equalCarrier;
                    binsPaired                    = true;
                }
            // The group's clusters: only those the cut rasters with the group's own cluster pipeline
            // (SetFrameVirtualGeometry made its carrier).
            if ( depthClusters && source.VirtualGeometry->GroupCarriers[group] != nullptr &&
                 source.VirtualGeometry->GroupPipeline( *source.Table, group ) == indirect.VirtualGeometryPipeline &&
                 twins.VgDepth != nullptr && twins.VgEqual != nullptr )
                if ( const auto [depthCarrier, equalCarrier] = carriers( twins.VgDepth, twins.VgEqual );
                     depthCarrier )
                {
                    depthClusters->GroupPipelines[group]             = twins.VgDepth;
                    depthClusters->GroupCarriers[group]              = depthCarrier;
                    gbuffer.VirtualGeometry->GroupPipelines[group] = twins.VgEqual;
                    gbuffer.VirtualGeometry->GroupCarriers[group]  = equalCarrier;
                    clustersPaired                                 = true;
                }
        }
        if ( !binsPaired && !clustersPaired )
            return false;
        // A depth issue without paired bins rasters clusters only (BinnedIssue::BinsIssued false: Culled unused).
        depth.BinsIssued = binsPaired;
        if ( clustersPaired )
            depth.VirtualGeometry = std::move( depthClusters );
        return true;
    }

    void MeshRenderer::DeclareEarlyZBins()
    {
        const FrameBinnedView* const view  = FrameBinned( GpuDriven::MeshPass::GBuffer, kBinnedMainView );
        const ViewFrame* const       frame = m_SceneRenderer->GetViewFrame();
        if ( view == nullptr || frame == nullptr )
            return;
        std::unordered_map<const GraphicsPipeline*, EarlyZBinTwinSlot> kept;
        MeshRendererDetail::BinnedIssue                                depth;
        MeshRendererDetail::BinnedIssue                                gbuffer;
        const bool paired = EarlyZSplit( view->Issue, CaptureFrameState( frame ), depth, gbuffer, &kept );
        m_EarlyZBinTwins  = std::move( kept );
        if ( !paired )
            return;
        m_MaskedDepthDraws.SetBinned( std::move( depth ) );
        m_EarlyZGBufferIssue = std::move( gbuffer );
        m_EarlyZBinFrame     = frame->FrameIndex;
    }

    void MeshRenderer::DeclareEarlyZPostOcclusionDraws( RDG::PassBuilder& pass, const SceneViewInputs& view )
    {
        m_EarlyZPostDepthDraws.Clear();
        m_EarlyZPostGBufferIssue.reset();
        m_EarlyZPostFrame.reset();
        if ( m_EarlyZPass == ::Common::Scalability::EarlyZPassMode::Off || m_SceneRenderer == nullptr ||
             m_SceneRenderer->GetMainCamera() == nullptr )
        {
            m_EarlyZPostDepthDraws.Declare( pass, view );
            return;
        }
        const FrameBinnedView* const binned = FrameBinned( GpuDriven::MeshPass::GBuffer, kBinnedMainView );
        const ViewFrame* const       frame  = m_SceneRenderer->GetViewFrame();
        // Phase 2's records and the post cut's clusters through the same twins as phase 1's (m_EarlyZBinTwins,
        // pruned by phase 1 only: phase 2 draws the same table's groups).
        MeshRendererDetail::BinnedIssue depth;
        MeshRendererDetail::BinnedIssue gbuffer;
        if ( binned != nullptr && binned->PostOcclusion && frame != nullptr &&
             EarlyZSplit( *binned->PostOcclusion, CaptureFrameState( frame ), depth, gbuffer, nullptr ) )
        {
            m_EarlyZPostDepthDraws.SetBinned( std::move( depth ) );
            m_EarlyZPostGBufferIssue = std::move( gbuffer );
            m_EarlyZPostFrame        = frame->FrameIndex;
        }
        m_EarlyZPostDepthDraws.Declare( pass, view );
    }

    Common::BoolResultStr MeshRenderer::RenderEarlyZPostOcclusionManual( const RDG::PassContext& context ) const
    {
        return m_EarlyZPostDepthDraws.Record( context );
    }

    Common::BoolResultStr MeshRenderer::RenderMaskedDepthManual( const RDG::PassContext& context ) const
    {
        return m_MaskedDepthDraws.Record( context );
    }

    void MeshRenderer::DeclareGrassVisibilityRasterDraws( RDG::PassBuilder& pass, const SceneViewInputs& view,
                                                          const RDG::BufferRef visibility, const bool postOcclusion )
    {
        MeshRendererDetail::MeshDrawList& list =
             postOcclusion ? m_GrassVisibilityRasterPostDraws : m_GrassVisibilityRasterDraws;
        list.Clear();
        if ( m_GrassVisibilitySections.empty() || !visibility.IsValid() || !m_InstancedVisibilityPipeline )
            return;
        BuildGpuInstancedDraws( list, m_InstancedVisibilityPipeline.get(), MeshPass::Visibility,
                                CaptureFrameState( m_SceneRenderer->GetViewFrame() ), nullptr, postOcclusion );
        list.Declare( pass, view,
                                              [visibility]( RDG::BindingBlockBuilder&         block,
                                                            const RDG::ShaderBindingLayout& layout )
                                              {
                                                  if ( SceneViewDetail::LayoutReadsBuffer(
                                                            layout, SceneResources::kVisibilityBuffer.Name ) )
                                                      block.Storage( SceneResources::kVisibilityBuffer, visibility,
                                                                     RDG::Access::StorageWrite );
                                              } );
    }

    Common::BoolResultStr MeshRenderer::RenderGrassVisibilityRasterManual( const RDG::PassContext& context,
                                                                           const bool postOcclusion ) const
    {
        return ( postOcclusion ? m_GrassVisibilityRasterPostDraws : m_GrassVisibilityRasterDraws ).Record( context );
    }

    void MeshRenderer::DeclareGrassVisibilityResolveDraws( RDG::PassBuilder& pass, const SceneViewInputs& view,
                                                           const RDG::BufferRef visibility,
                                                           const RDG::BufferRef resolveArgs )
    {
        MeshRendererDetail::MeshDrawList& list = m_GrassVisibilityResolveDraws;
        list.Clear();
        m_GrassResolveArgs = resolveArgs;
        if ( m_GrassVisibilitySections.empty() || !visibility.IsValid() || !resolveArgs.IsValid() ||
             !m_InstancedVisibilityResolvePipeline )
            return;
        BuildGpuInstancedDraws( list, m_InstancedVisibilityResolvePipeline.get(), MeshPass::VisibilityResolve,
                                CaptureFrameState( m_SceneRenderer->GetViewFrame() ), nullptr, false );
        list.Declare( pass, view,
                                               [visibility]( RDG::BindingBlockBuilder&         block,
                                                             const RDG::ShaderBindingLayout& layout )
                                               {
                                                   if ( SceneViewDetail::LayoutReadsBuffer(
                                                             layout, SceneResources::kVisibilityBuffer.Name ) )
                                                       block.Storage( SceneResources::kVisibilityBuffer, visibility,
                                                                      RDG::Access::StorageRead );
                                               } );
    }

    Common::BoolResultStr MeshRenderer::RenderGrassVisibilityResolveManual( const RDG::PassContext& context ) const
    {
        return m_GrassVisibilityResolveDraws.Record( context );
    }

    void MeshRenderer::DeclareGrassDepthPostOcclusionDraws( RDG::PassBuilder& pass, const SceneViewInputs& view )
    {
        m_GrassDepthPostDraws.Clear();
        m_MaskedPrepassedPost.clear();
        // Phase 2's grass by phase 1's rule (BuildGpuInstancedDraws' companion-less Depth call): the sections
        // EarlyZAdmits whose DEPTH_EQUAL G-buffer half is Ready write their clipped depth here, through their
        // phase-2 records, and are recorded in m_MaskedPrepassedPost for "Grass: GBuffer Post-Occlusion".
        if ( m_EarlyZPass != ::Common::Scalability::EarlyZPassMode::Off && m_InstancedViewDepthPipeline &&
             m_InstancedGBufferEqualPipeline && m_SceneRenderer != nullptr &&
             m_SceneRenderer->GetMainCamera() != nullptr )
            BuildGpuInstancedDraws( m_GrassDepthPostDraws, m_InstancedViewDepthPipeline.get(), MeshPass::Depth,
                                    CaptureFrameState( m_SceneRenderer->GetViewFrame() ), nullptr, true );
        m_GrassDepthPostDraws.Declare( pass, view );
    }

    Common::BoolResultStr MeshRenderer::RenderGrassDepthPostOcclusionManual( const RDG::PassContext& context ) const
    {
        return m_GrassDepthPostDraws.Record( context );
    }

    void MeshRenderer::DeclareGrassGBufferPostOcclusionDraws( RDG::PassBuilder& pass, const SceneViewInputs& view )
    {
        m_GrassGBufferPostDraws.Clear();
        if ( !m_InstancedGBufferPipeline || m_SceneRenderer == nullptr || m_SceneRenderer->GetMainCamera() == nullptr )
            return;
        // Phase 2's grass that the G-buffer path draws (a mesh variety, a blade off the visibility buffer), phase
        // 2's records: the sections "Grass: Depth Prepass Post-Occlusion" drew through their DEPTH_EQUAL cell (EQUAL,
        // no writes, no clip), every other one through its (path x GBuffer) cell with its own clip and depth writes.
        BuildGpuInstancedDraws( m_GrassGBufferPostDraws, m_InstancedGBufferPipeline.get(), MeshPass::GBuffer,
                                CaptureFrameState( m_SceneRenderer->GetViewFrame() ), nullptr, true );
        m_GrassGBufferPostDraws.Declare( pass, view );
    }

    Common::BoolResultStr MeshRenderer::RenderGrassGBufferPostOcclusionManual( const RDG::PassContext& context ) const
    {
        return m_GrassGBufferPostDraws.Record( context );
    }

    void MeshRenderer::DeclareGBufferPostOcclusionDraws( RDG::PassBuilder& pass, const SceneViewInputs& view )
    {
        m_GBufferPostDraws.Clear();
        const FrameBinnedView* const binned = FrameBinned( GpuDriven::MeshPass::GBuffer, kBinnedMainView );
        if ( binned == nullptr || !binned->PostOcclusion )
            return;
        // Only bins: every CPU record of the view was drawn (or dropped for its phase-1 bin) by "Deferred:
        // GBuffer". This frame's early-Z issue of phase 2 when "Deferred: Depth Prepass Post-Occlusion" drew some
        // of its groups (DeclareEarlyZPostOcclusionDraws: those EQUAL through their twins), the view's own otherwise.
        const ViewFrame* const frame = m_SceneRenderer != nullptr ? m_SceneRenderer->GetViewFrame() : nullptr;
        if ( m_EarlyZPostGBufferIssue && frame != nullptr && m_EarlyZPostFrame == frame->FrameIndex )
            m_GBufferPostDraws.SetBinned( std::move( *m_EarlyZPostGBufferIssue ) );
        else
            m_GBufferPostDraws.SetBinned( *binned->PostOcclusion );
        m_EarlyZPostGBufferIssue.reset();
        m_GBufferPostDraws.Declare( pass, view );
    }

    Common::BoolResultStr MeshRenderer::RenderGBufferPostOcclusionManual( const RDG::PassContext& context ) const
    {
        return m_GBufferPostDraws.Record( context );
    }

    void MeshRenderer::DeclareForwardDepthNormalDraws( RDG::PassBuilder& pass, const SceneViewInputs& view,
                                                       const bool rayTracedSun, const bool localShadowMasks,
                                                       const bool virtualLocal )
    {
        m_DepthNormalDraws.Clear();
        // This frame's admissions start empty here, whatever returns below: "Deferred: Generic" reads only a set
        // stamped with its own frame.
        m_GenericPrepassed.clear();
        const ViewFrame* const frame = m_SceneRenderer != nullptr ? m_SceneRenderer->GetViewFrame() : nullptr;
        m_GenericPrepassFrame = frame != nullptr ? std::optional<uint64_t>( frame->FrameIndex ) : std::nullopt;
        m_GenericPrepassRayTracedSun = rayTracedSun;
        m_GenericPrepassLocalShadowMasks = localShadowMasks;
        m_GenericPrepassVirtualLocal     = virtualLocal;
        const auto& gbuffer          = m_SceneRenderer != nullptr ? m_SceneRenderer->GetGBuffer() : nullptr;
        if ( !gbuffer || m_SceneRenderer->GetMainCamera() == nullptr || m_GenericQueue.empty() )
            return;
        // The graph LOADs the G-buffer the static, skinned and terrain draws wrote. The unbinned draws carry the
        // camera and the material row in their own blocks; the binned ones (the DepthPrepass bins, culled by the
        // "depthPrepass" view) read the GPU scene's primitive rows, the view input bound here.
        BuildGenericDraws( m_DepthNormalDraws, GenericSlice::DeferredDepthNormal );
        AttachBinned( m_DepthNormalDraws, GpuDriven::MeshPass::DepthPrepass, kBinnedMainView );
        m_DepthNormalDraws.Declare( pass, view );
    }

    Common::BoolResultStr MeshRenderer::RenderForwardDepthNormalManual( const RDG::PassContext& context ) const
    {
        return m_DepthNormalDraws.Record( context );
    }

    void MeshRenderer::DeclareDepthPrepassDraws( RDG::PassBuilder& pass, const SceneViewInputs& view )
    {
        m_DepthPrepassDraws.Clear();
        // This frame's admissions start empty here, whatever returns below: the base pass reads only a set
        // stamped with its own frame (PrepassCellDraw, OpaqueGenericAdmitted).
        m_PrepassCellDraws.clear();
        m_PrepassGpuSections.clear();
        m_GenericPrepassed.clear();
        const ViewFrame* const frame = m_SceneRenderer != nullptr ? m_SceneRenderer->GetViewFrame() : nullptr;
        m_PrepassFrame        = frame != nullptr ? std::optional<uint64_t>( frame->FrameIndex ) : std::nullopt;
        m_GenericPrepassFrame = m_PrepassFrame;
        if ( m_SceneRenderer == nullptr || m_SceneRenderer->GetMainCamera() == nullptr || !m_StaticDepthPipeline )
            return;
        // The base pass's choices, so the Forward cells checked here are the ones it records: no screen masks on
        // the forward path, the virtual shadow map's permutation under VirtualMaps (MeshGeometryPass sets the same).
        m_ForwardVirtualLocal            = m_SceneRenderer->LocalShadowsAreVirtual();
        m_GenericPrepassRayTracedSun     = false;
        m_GenericPrepassLocalShadowMasks = false;
        m_GenericPrepassVirtualLocal     = m_ForwardVirtualLocal;
        m_GeometryPass                   = MeshPass::Depth;
        BuildStaticDraws( m_DepthPrepassDraws );
        BuildSkinnedDraws( m_DepthPrepassDraws );
        m_GeometryPass = MeshPass::Forward;
        BuildGenericDraws( m_DepthPrepassDraws, GenericSlice::ForwardDepth );
        m_DepthPrepassDraws.Declare( pass, view );
        // The skin cache's streams the (Skinned x Depth) cell fetches by address (Vertex_Skinned.glslh).
        for ( const RDG::BufferRef& streams : m_SkinCacheReads )
            pass.Read( streams, RDG::Access::StorageRead );
    }

    Common::BoolResultStr MeshRenderer::RenderDepthPrepassManual( const RDG::PassContext& context ) const
    {
        return m_DepthPrepassDraws.Record( context );
    }

    bool MeshRenderer::PrepassCurrent() const
    {
        const ViewFrame* const frame = m_SceneRenderer != nullptr ? m_SceneRenderer->GetViewFrame() : nullptr;
        return frame != nullptr && m_PrepassFrame == frame->FrameIndex;
    }

    std::optional<MaterialPipelineTracker::CellDraw>
    MeshRenderer::PrepassCellDraw( const PrepassCellKey& key ) const
    {
        if ( !PrepassCurrent() )
            return std::nullopt;
        const auto found = m_PrepassCellDraws.find( key );
        return found != m_PrepassCellDraws.end() ? std::optional( found->second ) : std::nullopt;
    }

    bool MeshRenderer::PairsWithPrepass( const bool wireframeReplaced ) const
    {
        // Only the forward path builds these two passes (the deferred one draws its meshes into the G-buffer), and
        // only with the prepass's cells; the Wireframe view's static and instanced twins write their own depth.
        if ( m_GeometryPass != MeshPass::Forward && m_GeometryPass != MeshPass::Depth )
            return false;
        if ( wireframeReplaced && WireframeView() )
            return false;
        return m_StaticDepthPipeline != nullptr;
    }

    bool MeshRenderer::SetupDepthPrepass()
    {
        // (Static x Depth): the pass state of every static draw of "Forward: Depth Prepass" - the default surface's
        // view depth program on the scene depth alone, Closer, depth written (UE: the depth pass's DepthWrite
        // state). REQUIRED: the forward base pass tests CloserOrEqual against this depth and writes none.
        const auto staticShader = DefaultSurfaceProgram( MeshVertexPath::Static, MeshPass::Depth );
        if ( !staticShader )
            return false; // DefaultSurfaceShaderName named the missing cell
        GraphicsPipelineSpecification spec;
        spec.DebugName         = "StaticMeshDepth";
        spec.Layout            = MeshVertexLayout( MeshVertexPath::Static );
        spec.DepthCompareOp    = DepthCompare::Closer;
        spec.DepthWriteEnabled = true;
        spec.CullMode          = CullMode::Back;
        spec.Shader            = staticShader;
        spec.TargetLayout      = SceneDepthLayout();
        const auto staticPipeline = m_SceneRenderer->GetPipelineCache().GetOrCreate( spec );
        if ( !staticPipeline )
        {
            LOG_ERROR( "[MeshRenderer] the forward depth prepass has no static pipeline: {}",
                       staticPipeline.GetError() );
            return false;
        }
        m_StaticDepthPipeline = staticPipeline.GetValue();
        // The unowned spare, as m_GBufferUnownedMaterial (SetupGBufferPass says why one is enough).
        m_DepthUnownedMaterial = CreateCellMaterial( MeshVertexPath::Static, MeshPass::Depth );
        if ( !m_DepthUnownedMaterial )
            return false;

        // (Instanced x Depth): the auto-batched statics, the Instanced Static Meshes and the landscape grass. A
        // refusal turns instancing off in BOTH forward passes (the base pass draws only what the prepass can), so
        // an ISM is then not drawn on the forward path - said here and by BuildStaticDraws.
        if ( const auto instancedShader = DefaultSurfaceProgram( MeshVertexPath::Instanced, MeshPass::Depth ) )
        {
            GraphicsPipelineSpecification ispec = spec;
            ispec.DebugName                     = "StaticMeshDepthInstanced";
            ispec.Layout                        = MeshVertexLayout( MeshVertexPath::Instanced );
            ispec.Shader                        = instancedShader;
            if ( const auto instanced = m_SceneRenderer->GetPipelineCache().GetOrCreate( ispec ) )
            {
                m_InstancedDepthMaterial = CreateCellMaterial( MeshVertexPath::Instanced, MeshPass::Depth );
                if ( m_InstancedDepthMaterial )
                {
                    m_InstancedDepthPipeline = instanced.GetValue();
                    m_InstancedDepthInstance = m_InstancedDepthMaterial->CreateInstance( "StaticInstancedDepthBatch" );
                }
            }
            else
                LOG_ERROR( "[MeshRenderer] instanced drawing is off in the FORWARD path (no depth prepass cell): {}",
                           instanced.GetError() );
        }

        // (Skinned x Depth): a refusal leaves skinned meshes out of the forward path (they would be shaded with no
        // depth of their own).
        if ( const auto skinnedShader = DefaultSurfaceProgram( MeshVertexPath::Skinned, MeshPass::Depth ) )
        {
            GraphicsPipelineSpecification sspec = spec;
            sspec.DebugName                     = "SkinnedMeshDepth";
            sspec.Layout                        = MeshVertexLayout( MeshVertexPath::Skinned );
            sspec.Shader                        = skinnedShader;
            m_SkinnedDepthUnownedMaterial       = CreateCellMaterial( MeshVertexPath::Skinned, MeshPass::Depth );
            const auto skinned                  = m_SceneRenderer->GetPipelineCache().GetOrCreate( sspec );
            if ( skinned && m_SkinnedDepthUnownedMaterial )
                m_SkinnedDepthPipeline = skinned.GetValue();
            else
                LOG_ERROR( "[MeshRenderer] skinned meshes are not drawn in the FORWARD path (no depth prepass "
                           "cell): {}",
                           skinned ? std::string( "the cell material was refused" ) : std::string( skinned.GetError() ) );
        }
        return true;
    }

    bool MeshRenderer::SetupGBufferPass()
    {
        // Optional: only present when the deferred G-buffer shader exists and the scene renderer has a
        // G-buffer. Failure here does NOT fail Initialize — the forward path stays fully functional.
        m_StaticGBufferShader = DefaultSurfaceProgram( MeshVertexPath::Static, MeshPass::GBuffer );
        if ( !m_StaticGBufferShader )
            return false;

        const auto& gbuffer = m_SceneRenderer != nullptr ? m_SceneRenderer->GetGBuffer() : nullptr;
        if ( !gbuffer )
            return false;

        GraphicsPipelineSpecification spec;
        spec.DebugName      = "StaticMeshGBuffer";
        spec.Layout         = MeshVertexLayout( MeshVertexPath::Static );
        spec.DepthCompareOp = DepthCompare::CloserOrEqual;
        spec.CullMode       = CullMode::Back;
        spec.Shader         = m_StaticGBufferShader;
        spec.TargetLayout   = GBufferLayout();

        const auto gbufferPipeline = m_SceneRenderer->GetPipelineCache().GetOrCreate( spec );
        if ( !gbufferPipeline )
        {
            LOG_ERROR( "[MeshRenderer] the deferred path is off, the forward one still draws: {}",
                       gbufferPipeline.GetError() );
            return false;
        }
        m_StaticGBufferPipeline = gbufferPipeline.GetValue();
        // The Equal half of the deferred early-Z pairing (DeclareGBufferDraws): EQUAL, no depth writes - the
        // prepass's depth is final. Optional: refused, the statics clip and write their own depth here, said once.
        GraphicsPipelineSpecification equalSpec = spec;
        equalSpec.DebugName                     = "StaticMeshGBufferDepthEqual";
        equalSpec.DepthCompareOp                = DepthCompare::Equal;
        equalSpec.DepthWriteEnabled             = false;
        if ( const auto equal = m_SceneRenderer->GetPipelineCache().GetOrCreate( equalSpec ) )
            m_StaticGBufferEqualPipeline = equal.GetValue();
        else
            LOG_ERROR( "[MeshRenderer] no deferred early-Z pass for static meshes and ISMs: they clip and write their "
                       "own depth in the G-buffer. {}",
                       equal.GetError() );

        // (Static x GBuffer) for the meshes whose FORWARD material is not service-owned. There is exactly
        // one such material in the engine — MeshECSSystem::m_DefaultMaterial, which stands in for every
        // mesh whose slot does not resolve — and it has no `.demat`, so MaterialService has no sibling of
        // it to hand out. Without this the deferred pass drew nothing for those meshes: measured on
        // Resources/Assets/Scenes/MAT_ProbeDeferredNoSlot.desce, a Cornell box with the material stripped
        // off one cube, and the cube vanished.
        //
        // ONE material is enough and that is a property of the engine, not an assumption: a MeshRenderer
        // draws one scene's queue, a scene has one MeshECSSystem, and a MeshECSSystem has one default
        // material. DrawStaticMeshes checks it — a second distinct unowned material in one pass would
        // share this material's Materials[] buffer and the last group to fill it would win.
        //
        // It needs no textures: the default material has none either, so both sample the backend's
        // fallbacks and the surface is identical. What it supplies is the descriptor SETS, allocated from
        // the G-buffer shader's own reflection.
        m_GBufferUnownedMaterial = CreateCellMaterial( MeshVertexPath::Static, MeshPass::GBuffer );
        if ( !m_GBufferUnownedMaterial )
            return false;

        // (Instanced x GBuffer). Without it the deferred pass had no instanced cell, and since the ISM
        // queue is the one queue with NO per-object fallback, every InstancedStaticMesh entity was
        // dropped there in silence -- in the render path most of this repository's scenes state. Optional
        // like the rest of this pass: a refusal costs instancing in the G-buffer, and DrawStaticMeshes
        // logs what that costs rather than dropping the queue without a word.
        m_InstancedGBufferShader = DefaultSurfaceProgram( MeshVertexPath::Instanced, MeshPass::GBuffer );
        if ( m_InstancedGBufferShader )
        {
            GraphicsPipelineSpecification ispec;
            ispec.DebugName      = "StaticMeshGBufferInstanced";
            ispec.Layout         = MeshVertexLayout( MeshVertexPath::Instanced );
            ispec.DepthCompareOp = DepthCompare::CloserOrEqual;
            ispec.CullMode       = CullMode::Back;
            ispec.Shader         = m_InstancedGBufferShader;
            ispec.TargetLayout   = GBufferLayout();

            if ( const auto instanced = m_SceneRenderer->GetPipelineCache().GetOrCreate( ispec ) )
            {
                m_InstancedGBufferPipeline = instanced.GetValue();
                // The masked depth prepass's two states (DeclareMaskedDepthDraws). Optional: refused, the masked
                // sections keep drawing their clip in the G-buffer alone, said once here. The G-buffer half tests
                // EQUAL and writes none (UE's base pass after the masked early-Z pass): the prepass's clip already
                // decided every texel, so its cell is the clip-less DESERT_SURFACE_DEPTH_EQUAL permutation.
                GraphicsPipelineSpecification equal = ispec;
                equal.DebugName                     = "StaticMeshGBufferInstancedDepthEqual";
                equal.DepthCompareOp                = DepthCompare::Equal;
                equal.DepthWriteEnabled             = false;
                GraphicsPipelineSpecification depth;
                depth.DebugName      = "StaticMeshViewDepthInstanced";
                depth.Layout         = MeshVertexLayout( MeshVertexPath::Instanced );
                depth.DepthCompareOp = DepthCompare::CloserOrEqual;
                depth.CullMode       = CullMode::Back;
                depth.Shader         = DefaultSurfaceProgram( MeshVertexPath::Instanced, MeshPass::Depth );
                depth.TargetLayout   = RenderTargetLayout{ {}, ViewTargetFormats::kGBufferDepth, 1 };
                std::string refused = depth.Shader ? std::string{} : std::string( "no (Instanced x Depth) cell" );
                if ( depth.Shader )
                {
                    const auto equalPipeline = m_SceneRenderer->GetPipelineCache().GetOrCreate( equal );
                    const auto depthPipeline = m_SceneRenderer->GetPipelineCache().GetOrCreate( depth );
                    if ( equalPipeline && depthPipeline )
                    {
                        m_InstancedGBufferEqualPipeline = equalPipeline.GetValue();
                        m_InstancedViewDepthPipeline    = depthPipeline.GetValue();
                    }
                    else
                        refused = std::format( "depth: {} | equal: {}", depthPipeline ? "ok" : depthPipeline.GetError(),
                                               equalPipeline ? "ok" : equalPipeline.GetError() );
                    // GRASS-VIS carriers: the raster tests the view's depth and writes none (its output is the
                    // visibility word), the resolve is a full-screen triangle into the G-buffer with the word's depth.
                    GraphicsPipelineSpecification visibility = depth;
                    visibility.DebugName                     = "GrassVisibilityRaster";
                    visibility.DepthWriteEnabled             = false;
                    GraphicsPipelineSpecification resolve    = ispec;
                    resolve.DebugName                        = "GrassVisibilityResolve";
                    resolve.CullMode                         = CullMode::None;
                    const auto visibilityPipeline = m_SceneRenderer->GetPipelineCache().GetOrCreate( visibility );
                    const auto resolvePipeline    = m_SceneRenderer->GetPipelineCache().GetOrCreate( resolve );
                    if ( visibilityPipeline && resolvePipeline )
                    {
                        m_InstancedVisibilityPipeline        = visibilityPipeline.GetValue();
                        m_InstancedVisibilityResolvePipeline = resolvePipeline.GetValue();
                    }
                    else
                        LOG_ERROR( "[MeshRenderer] no grass visibility buffer path: the blades keep the G-buffer. "
                                   "raster: {} | resolve: {}",
                                   visibilityPipeline ? "ok" : visibilityPipeline.GetError(),
                                   resolvePipeline ? "ok" : resolvePipeline.GetError() );
                }
                if ( !refused.empty() )
                    LOG_ERROR( "[MeshRenderer] no early-Z pass for instanced meshes: masked grass, auto-batched "
                               "statics and ISMs clip and write their own depth in the G-buffer. {}",
                               refused );
                // The Equal half's instanced spare (StaticCellsOf depthEqual): its own InstanceTransforms, so the
                // two halves of one G-buffer never fill one buffer.
                if ( m_InstancedGBufferEqualPipeline )
                {
                    m_InstancedGBufferEqualMaterial = CreateCellMaterial( MeshVertexPath::Instanced, MeshPass::GBuffer );
                    if ( m_InstancedGBufferEqualMaterial )
                        m_InstancedGBufferEqualInstance =
                             m_InstancedGBufferEqualMaterial->CreateInstance( "StaticInstancedGBufferEqualBatch" );
                }
                m_InstancedGBufferMaterial = CreateCellMaterial( MeshVertexPath::Instanced, MeshPass::GBuffer );
                if ( m_InstancedGBufferMaterial )
                {
                    m_InstancedGBufferInstance =
                         m_InstancedGBufferMaterial->CreateInstance( "StaticInstancedGBufferBatch" );
                }
            }
            else
            {
                LOG_ERROR( "[MeshRenderer] instanced drawing is off in the DEFERRED path: {}. Instanced "
                           "Static Meshes will not be drawn while the scene renders deferred.",
                           instanced.GetError() );
            }
        }
        else
        {
            LOG_ERROR( "[MeshRenderer] shader '{}' is missing; Instanced Static Meshes will not be drawn "
                       "while the scene renders deferred.",
                       DefaultSurfaceShaderName( MeshVertexPath::Instanced, MeshPass::GBuffer ).value_or( "?" ) );
        }

        // (Skinned x GBuffer): skinned meshes are drawn in the base pass on the deferred path (BuildSkinnedDraws
        // under m_GeometryPass GBuffer), never forward over the composite, and the surface cache captures a skinned
        // mesh's cards with the same pass state. Optional like the instanced cell: a refusal costs skinned meshes
        // in a deferred scene and in the surface cache, said here.
        m_SkinnedGBufferShader = DefaultSurfaceProgram( MeshVertexPath::Skinned, MeshPass::GBuffer );
        if ( m_SkinnedGBufferShader )
        {
            GraphicsPipelineSpecification sspec;
            sspec.DebugName                 = "SkinnedMeshGBuffer";
            sspec.Layout                    = MeshVertexLayout( MeshVertexPath::Skinned );
            sspec.DepthCompareOp            = DepthCompare::CloserOrEqual;
            sspec.CullMode                  = CullMode::Back;
            sspec.Shader                    = m_SkinnedGBufferShader;
            sspec.TargetLayout              = GBufferLayout();
            const auto skinned              = m_SceneRenderer->GetPipelineCache().GetOrCreate( sspec );
            m_SkinnedGBufferUnownedMaterial = CreateCellMaterial( MeshVertexPath::Skinned, MeshPass::GBuffer );
            if ( !skinned )
            {
                LOG_ERROR( "[MeshRenderer] skinned meshes will not be drawn while the scene renders deferred, nor "
                           "be in the surface cache: {}",
                           skinned.GetError() );
            }
            else if ( !m_SkinnedGBufferUnownedMaterial )
            {
                LOG_ERROR( "[MeshRenderer] skinned meshes will not be drawn while the scene renders deferred: "
                           "the (Skinned x GBuffer) cell material was refused." );
            }
            else
            {
                m_SkinnedGBufferPipeline = skinned.GetValue();
                // The Equal half of the deferred early-Z pairing, as the static one.
                GraphicsPipelineSpecification equalSpec = sspec;
                equalSpec.DebugName                     = "SkinnedMeshGBufferDepthEqual";
                equalSpec.DepthCompareOp                = DepthCompare::Equal;
                equalSpec.DepthWriteEnabled             = false;
                if ( const auto equal = m_SceneRenderer->GetPipelineCache().GetOrCreate( equalSpec ) )
                    m_SkinnedGBufferEqualPipeline = equal.GetValue();
                else
                    LOG_ERROR( "[MeshRenderer] no deferred early-Z pass for skinned meshes: they write their own "
                               "depth in the G-buffer. {}",
                               equal.GetError() );
            }
        }
        else
        {
            LOG_ERROR( "[MeshRenderer] shader '{}' is missing; skinned meshes will not be drawn while the scene "
                       "renders deferred, nor be in the surface cache.",
                       DefaultSurfaceShaderName( MeshVertexPath::Skinned, MeshPass::GBuffer ).value_or( "?" ) );
        }
        return true;
    }

} // namespace Desert::Graphic::System
