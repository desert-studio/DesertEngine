#pragma once

#include <Engine/Graphic/Systems/RenderSystem.hpp>

#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/ShaderBindingLayoutCache.hpp>
#include <Engine/Graphic/Materials/Material.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <format>

namespace Desert::Graphic
{
    // "Scene: DepthResolve": SceneDepthResolve.shader fetches sample 0 of the multisampled scene depth. The
    // material holds nothing of its own: u_Depth is a pass parameter (not in the shader's Properties), bound by
    // SceneDepthResolveRenderer::Record through RDG::PassBindings from the node's SampledGraphics read.
    // Header-only.
    class MaterialSceneDepthResolve final : public Material
    {
    public:
        MaterialSceneDepthResolve() : Material( "MaterialSceneDepthResolve", "SceneDepthResolve" )
        {
        }
    };
} // namespace Desert::Graphic

namespace Desert::Graphic::System
{
    // The compute passes that read scene depth (height fog, volumetric clouds) sample a SINGLE-SAMPLE depth: a
    // sampler2D over a multisampled image is invalid (VUID-RuntimeSpirv-samples-08725). At MSAA > 1 this system
    // owns SceneDepthResolved, a 1x depth-only framebuffer resized with the scene target, and records the raster
    // node that writes sample 0 of the multisampled scene depth into it (UE: ResolveSceneDepth before the
    // post-processing). The pipeline is built against the graph's canonical render pass for exactly that target
    // (TargetLayout: no colour, the scene depth format, one sample). At MSAA 1 nothing is built: the consumers
    // read the scene depth itself (SceneRenderer::GetComputeSceneDepth).
    class SceneDepthResolveRenderer final : public RenderSystem
    {
    public:
        using RenderSystem::RenderSystem;

        Common::BoolResultStr Initialize() override
        {
            const auto& target = m_TargetFramebuffer.lock();
            if ( !target )
                return Common::MakeError( "SceneDepthResolve: scene target framebuffer missing" );
            const FramebufferSpecification spec = target->GetSpecification();
            // Called again when the scene's sample count changes (SceneRenderer::ApplySceneSampleCount):
            // what was built for the previous count goes first.
            m_Resolved.reset();
            m_Pipeline.reset();
            m_Material.reset();
            if ( spec.Samples <= 1 || target->GetDepthAttachmentCount() == 0 )
                return BOOLSUCCESS;

            const auto depthFormat = target->GetDepthAttachmentImage()->GetImageSpecification().Format;
            FramebufferSpecification resolvedSpec;
            resolvedSpec.DebugName = "SceneDepthResolved";
            resolvedSpec.Samples   = 1;
            resolvedSpec.Attachments.Attachments.emplace_back( depthFormat );
            m_Resolved = Framebuffer::Create( resolvedSpec );
            if ( !m_Resolved )
                return Common::MakeError( "SceneDepthResolved framebuffer could not be created" );
            m_Resolved->Resize( target->GetFramebufferWidth(), target->GetFramebufferHeight() );

            m_Shader = Runtime::ResourceRegistry::GetShaderService()->GetByName( "SceneDepthResolve" );
            if ( !m_Shader )
                return Common::MakeError( "SceneDepthResolve shader not found" );

            GraphicsPipelineSpecification pipelineSpec;
            pipelineSpec.DebugName = "SceneDepthResolve";
            pipelineSpec.TargetLayout =
                 RenderTargetLayout{ .ColorFormats = {}, .DepthFormat = depthFormat, .Samples = 1 };
            pipelineSpec.Shader            = m_Shader;
            pipelineSpec.DepthTestEnabled  = true;
            pipelineSpec.DepthCompareOp    = CompareOp::Always;
            pipelineSpec.DepthWriteEnabled = true;
            const auto pipeline            = Graphic::GraphicsPipeline::Create( pipelineSpec );
            if ( !pipeline )
                return Common::MakeError( std::format( "SceneDepthResolve pipeline: {}", pipeline.GetError() ) );
            m_Pipeline = pipeline.GetValue();
            m_Material = std::make_unique<MaterialSceneDepthResolve>();
            return BOOLSUCCESS;
        }

        void RegisterPasses( RenderGraphBuilder& ) override
        {
        }

        // The scene target is multisampled and the resolve was built for it.
        [[nodiscard]] bool IsReady() const
        {
            return m_Resolved && m_Pipeline && m_Material;
        }

        // With the scene target (SceneRenderer::Resize).
        void Resize( uint32_t width, uint32_t height )
        {
            if ( m_Resolved )
                m_Resolved->Resize( width, height );
        }

        [[nodiscard]] const std::shared_ptr<Framebuffer>& GetFramebuffer() const
        {
            return m_Resolved;
        }

        // SETUP of "Scene: DepthResolve": the node's one block (block 0) - @p sceneDepth as u_Depth, fetched at
        // sample 0 (PointClamp), the material as the other route. Not ready: nothing declared, Record refuses.
        void DeclareBindings( RDG::PassBuilder& pass, RDG::TextureRef sceneDepth ) const
        {
            if ( !IsReady() || !sceneDepth.IsValid() )
                return;
            pass.Bindings( m_BindingLayout.Get( m_Shader ), m_Material->GetMaterialExecutor()->GetRouteFill() )
                 .Sampled( "u_Depth", sceneDepth, RDG::Access::SampledGraphics, RDG::SubresourceRange::All(),
                           RDG::SamplerDesc::PointClamp() );
        }

        // Inside the render pass the frame graph opens on SceneDepthResolved: opens block 0.
        Common::BoolResultStr Record( const RDG::PassContext& context )
        {
            if ( !IsReady() )
                return Common::MakeError( "SceneDepthResolve recorded without its pipeline" );
            const RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
            return Renderer::GetInstance().DrawFullscreen( bindings, *m_Pipeline,
                                                           m_Material->GetMaterialExecutor() );
        }

    private:
        std::shared_ptr<Framebuffer>               m_Resolved;
        std::shared_ptr<Shader>                    m_Shader;
        // The block layout, derived from m_Shader's reflection once per compile (not per frame).
        mutable ShaderBindingLayoutCache           m_BindingLayout;
        std::shared_ptr<GraphicsPipeline>          m_Pipeline;
        std::unique_ptr<MaterialSceneDepthResolve> m_Material;
    };
} // namespace Desert::Graphic::System
