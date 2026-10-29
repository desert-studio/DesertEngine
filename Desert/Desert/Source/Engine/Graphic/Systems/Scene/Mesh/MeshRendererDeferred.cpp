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
        BuildStaticDraws( m_GBufferDraws );
        BuildSkinnedDraws( m_GBufferDraws );
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

    void MeshRenderer::DeclareGBufferPostOcclusionDraws( RDG::PassBuilder& pass, const SceneViewInputs& view )
    {
        m_GBufferPostDraws.Clear();
        const FrameBinnedView* const binned = FrameBinned( GpuDriven::MeshPass::GBuffer, kBinnedMainView );
        if ( binned == nullptr || !binned->PostOcclusion )
            return;
        // Only bins: every CPU record of the view was drawn (or dropped for its phase-1 bin) by "Deferred: GBuffer".
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
                m_SkinnedGBufferPipeline = skinned.GetValue();
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
