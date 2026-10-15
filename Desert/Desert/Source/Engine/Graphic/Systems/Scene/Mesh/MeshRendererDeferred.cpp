// MeshRenderer's deferred half: the manual G-buffer pass and its pipeline.
#include "MeshRendererInternal.hpp"
#include <Engine/Graphic/ViewTargetLayouts.hpp>

namespace Desert::Graphic::System
{
    void MeshRenderer::DeclareGBufferDraws( RDG::PassBuilder& pass )
    {
        m_GBufferDraws.Clear();
        if ( !m_StaticGBufferPipeline )
            return;
        const auto& gbuffer = m_SceneRenderer != nullptr ? m_SceneRenderer->GetGBuffer() : nullptr;
        if ( !gbuffer || m_SceneRenderer->GetMainCamera() == nullptr )
            return;

        // The graph opens the render pass and clears it to ZERO (SceneRenderer::AddFrameGBuffer). The G-buffer
        // shaders sample no scene input: every block is the material's alone.
        m_DeferredGeometry = true;
        BuildStaticDraws( m_GBufferDraws );
        m_DeferredGeometry = false;
        m_GBufferDraws.Declare( pass, std::nullopt );
    }

    Common::BoolResultStr MeshRenderer::RenderGBufferManual( const RDG::PassContext& context ) const
    {
        return m_GBufferDraws.Record( context );
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

        // (Skinned x GBuffer): the pass state the surface cache captures a skinned mesh's cards with (the scene
        // still draws skinned meshes forward, RenderSkinnedManual). Optional like the rest of this pass: without
        // it skinned meshes are not in the Lumen scene, and the capture says so.
        m_SkinnedGBufferShader = DefaultSurfaceProgram( MeshVertexPath::Skinned, MeshPass::GBuffer );
        if ( m_SkinnedGBufferShader )
        {
            GraphicsPipelineSpecification sspec;
            sspec.DebugName      = "SkinnedMeshGBuffer";
            sspec.Layout         = MeshVertexLayout( MeshVertexPath::Skinned );
            sspec.DepthCompareOp = DepthCompare::CloserOrEqual;
            sspec.CullMode       = CullMode::Back;
            sspec.Shader         = m_SkinnedGBufferShader;
            sspec.TargetLayout   = GBufferLayout();
            if ( const auto skinned = m_SceneRenderer->GetPipelineCache().GetOrCreate( sspec ) )
                m_SkinnedGBufferPipeline = skinned.GetValue();
            else
                LOG_ERROR( "[MeshRenderer] skinned meshes are not in the surface cache: {}", skinned.GetError() );
        }
        else
        {
            LOG_ERROR( "[MeshRenderer] shader '{}' is missing; skinned meshes are not in the surface cache.",
                       DefaultSurfaceShaderName( MeshVertexPath::Skinned, MeshPass::GBuffer ).value_or( "?" ) );
        }
        return true;
    }

} // namespace Desert::Graphic::System
