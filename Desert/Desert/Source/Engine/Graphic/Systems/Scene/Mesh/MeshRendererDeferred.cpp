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

        // The RSM renders the same shader, layout and attachment set from the sun's POV, with a DEDICATED
        // material — but NOT the same pipeline, because it is drawn through a CASCADE matrix, and the
        // cascades are standard-Z (SetupShadowPass says why). Sharing the G-buffer pipeline would test
        // reversed-Z depth against standard-Z fragments, which is not "slightly wrong": it keeps the
        // FARTHEST surface per texel, so every VPL would be a back face and the bounce light would come
        // out of the wrong geometry. One extra pipeline is the price of the two conventions coexisting,
        // and the cache hands back a shared object anyway if some other pass ever asks for the same state.
        //
        // And not the same PROGRAM: the RSM draws the G-buffer cell's DESERT_GBUFFER_RSM permutation, which writes
        // no shading word (Pass_GBuffer.glslh) — GI takes VPL positions from the RSM depth. The define lives in
        // this variant only; the pipeline references the program compiled under it and carries no define list.
        // Its descriptor layout is the G-buffer cell's (the macro gates an output, never a binding), so the
        // RSM material below, allocated from the cell by name, binds against this pipeline unchanged.
        m_RSMShader = DefaultSurfaceProgramVariant( MeshVertexPath::Static, MeshPass::GBuffer,
                                                    ShaderVariant{ .Defines = { "DESERT_GBUFFER_RSM" } } );
        if ( !m_RSMShader )
        {
            LOG_ERROR( "[MeshRenderer] the deferred path is off, the forward one still draws: the G-buffer cell "
                       "did not compile under DESERT_GBUFFER_RSM." );
            return false;
        }
        GraphicsPipelineSpecification rsmSpec = spec;
        rsmSpec.DebugName                     = "StaticMeshRSM";
        rsmSpec.Shader                        = m_RSMShader;
        // Built against the RSM's own colour slots, not the G-buffer framebuffer: its slot 2 is an UNUSED slot
        // (VK_ATTACHMENT_UNUSED), which no render pass with an image in that slot is compatible with.
        rsmSpec.Framebuffer.reset();
        rsmSpec.TargetLayout = RenderTargetLayout{
             .ColorFormats = std::vector<std::optional<Core::Formats::ImageFormat>>(
                  ViewTargetFormats::kRSMColourSlots.begin(), ViewTargetFormats::kRSMColourSlots.end() ),
             .DepthFormat = ViewTargetFormats::kRSMDepth };
        rsmSpec.DepthCompareOp                = CompareOp::LessOrEqual;
        const auto rsmPipeline                = m_SceneRenderer->GetPipelineCache().GetOrCreate( rsmSpec );
        if ( !rsmPipeline )
        {
            LOG_ERROR( "[MeshRenderer] the deferred path is off, the forward one still draws: {}",
                       rsmPipeline.GetError() );
            return false;
        }
        m_RSMPipeline = rsmPipeline.GetValue();

        // (Static x GBuffer): the RSM reuses the G-buffer shader and its pipeline, rasterized from the
        // sun. A DEDICATED material (rather than the objects' own) because this pass writes a camera UB
        // holding the SUN's matrices, and two writes to one per-frame UB in a frame is the hazard the
        // glass pass was split out to avoid.
        m_RSMMaterial = CreateCellMaterial( MeshVertexPath::Static, MeshPass::GBuffer );
        if ( !m_RSMMaterial )
            return false;
        m_RSMInstance = m_RSMMaterial->CreateInstance();

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
        return true;
    }

} // namespace Desert::Graphic::System
