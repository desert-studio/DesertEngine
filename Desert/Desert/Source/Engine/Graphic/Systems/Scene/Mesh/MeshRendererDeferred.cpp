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
        m_DeferredGeometry = true;
        BuildStaticDraws( m_GBufferDraws );
        BuildSkinnedDraws( m_GBufferDraws );
        m_DeferredGeometry = false;
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
                                                       const bool rayTracedSun, const bool localShadowMasks )
    {
        m_DepthNormalDraws.Clear();
        // This frame's admissions start empty here, whatever returns below: "Deferred: Generic" reads only a set
        // stamped with its own frame.
        m_GenericPrepassed.clear();
        const ViewFrame* const frame = m_SceneRenderer != nullptr ? m_SceneRenderer->GetViewFrame() : nullptr;
        m_GenericPrepassFrame = frame != nullptr ? std::optional<uint64_t>( frame->FrameIndex ) : std::nullopt;
        m_GenericPrepassRayTracedSun = rayTracedSun;
        m_GenericPrepassLocalShadowMasks = localShadowMasks;
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
        // under m_DeferredGeometry), never forward over the composite, and the surface cache captures a skinned
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
