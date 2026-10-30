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
        AttachBinned( m_GBufferDraws, GpuDriven::MeshPass::GBuffer, kBinnedMainView );
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
        // This frame's admissions start empty here, whatever returns below: the G-buffer reads only a set stamped
        // with its own frame (PrepassCellDraw), and only when the static / skinned halves below ran.
        m_PrepassCellDraws.clear();
        m_PrepassGpuSections.clear();
        m_PrepassFrame.reset();
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
        m_MaskedDepthDraws.Declare( pass, view );
        // The skin cache's streams the (Skinned x Depth) cell fetches by address (Vertex_Skinned.glslh).
        for ( const RDG::BufferRef& streams : m_SkinCacheReads )
            pass.Read( streams, RDG::Access::StorageRead );
    }

    bool MeshRenderer::EarlyZAdmits( const DataDrivenMaterial* surface ) const
    {
        using Mode = ::Common::Scalability::EarlyZPassMode;
        if ( surface == nullptr || IsTranslucentSurface( surface ) )
            return false;
        // A `.Parallax` surface writes its pixel depth offset in the G-buffer (Pass_GBuffer.glslh gl_FragDepth); the
        // (path x Depth) cells write the geometric depth, so an EQUAL test against them would reject it. It keeps
        // its own depth in the G-buffer until the Depth cells write the same offset (REMAINDER GRASS-VIS-10).
        if ( IsParallaxCell( surface->GetShaderName() ) )
            return false;
        switch ( m_EarlyZPass )
        {
            case Mode::All:
                return true;
            case Mode::Masked:
                return IsMaskedSurface( surface );
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
                                                           const RDG::BufferRef resolveArgs, const bool postOcclusion )
    {
        MeshRendererDetail::MeshDrawList& list =
             postOcclusion ? m_GrassVisibilityResolvePostDraws : m_GrassVisibilityResolveDraws;
        list.Clear();
        m_GrassResolveArgs = resolveArgs;
        if ( m_GrassVisibilitySections.empty() || !visibility.IsValid() || !resolveArgs.IsValid() ||
             !m_InstancedVisibilityResolvePipeline )
            return;
        BuildGpuInstancedDraws( list, m_InstancedVisibilityResolvePipeline.get(), MeshPass::VisibilityResolve,
                                CaptureFrameState( m_SceneRenderer->GetViewFrame() ), nullptr, postOcclusion );
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

    Common::BoolResultStr MeshRenderer::RenderGrassVisibilityResolveManual( const RDG::PassContext& context,
                                                                            const bool postOcclusion ) const
    {
        return ( postOcclusion ? m_GrassVisibilityResolvePostDraws : m_GrassVisibilityResolveDraws ).Record( context );
    }

    void MeshRenderer::DeclareGrassGBufferPostOcclusionDraws( RDG::PassBuilder& pass, const SceneViewInputs& view )
    {
        m_GrassGBufferPostDraws.Clear();
        if ( !m_InstancedGBufferPipeline || m_SceneRenderer == nullptr || m_SceneRenderer->GetMainCamera() == nullptr )
            return;
        // Phase 2's grass that the G-buffer path draws (a mesh variety, a blade off the visibility buffer): the
        // section's (Instanced x GBuffer) cell with its own clip and depth writes, phase 2's records.
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
        // GBuffer".
        m_GBufferPostDraws.SetBinned( *binned->PostOcclusion );
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
