#pragma once

#include <Engine/Graphic/Systems/RenderSystem.hpp>

#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/Materials/Deferred/MaterialDepthExpand.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <format>

namespace Desert::Graphic::System
{
    // "Deferred: DepthExpand": the scene target is multisampled while the G-buffer is not, and a copy between
    // different sample counts does not exist. A full-screen triangle samples the single-sample G-buffer depth
    // and writes it through gl_FragDepth into every sample of the multisampled scene depth (depth test ALWAYS,
    // write on), so the forward passes and depth-tested overlays that follow are occluded by the deferred
    // geometry. The node declares the multisampled depth as its only target; the pipeline is built against the
    // render graph's canonical render pass for exactly that target (TargetLayout: no colour, the scene depth
    // format, the scene's sample count), which is the render pass the graph opens for the node. A single-sample
    // scene target needs no expansion (the graph copies the depth), so nothing is built for it.
    class DepthExpandRenderer final : public RenderSystem
    {
    public:
        using RenderSystem::RenderSystem;

        virtual Common::BoolResultStr Initialize() override
        {
            const auto& target = m_TargetFramebuffer.lock();
            if ( !target )
                return Common::MakeError( "DepthExpand: scene target framebuffer missing" );
            const FramebufferSpecification spec = target->GetSpecification();
            if ( spec.Samples <= 1 || target->GetDepthAttachmentCount() == 0 )
                return BOOLSUCCESS;

            m_Shader = Runtime::ResourceRegistry::GetShaderService()->GetByName( "DepthExpand" );
            if ( !m_Shader )
                return Common::MakeError( "DepthExpand shader not found" );

            GraphicsPipelineSpecification pipelineSpec;
            pipelineSpec.DebugName    = "DepthExpand";
            pipelineSpec.TargetLayout = RenderTargetLayout{
                 .ColorFormats = {},
                 .DepthFormat  = target->GetDepthAttachmentImage()->GetImageSpecification().Format,
                 .Samples      = spec.Samples };
            pipelineSpec.Shader            = m_Shader;
            pipelineSpec.DepthTestEnabled  = true;
            pipelineSpec.DepthCompareOp    = CompareOp::Always;
            pipelineSpec.DepthWriteEnabled = true;
            const auto pipeline            = Graphic::GraphicsPipeline::Create( pipelineSpec );
            if ( !pipeline )
                return Common::MakeError( std::format( "DepthExpand pipeline: {}", pipeline.GetError() ) );
            m_Pipeline = pipeline.GetValue();
            m_Material = std::make_unique<MaterialDepthExpand>();
            return BOOLSUCCESS;
        }

        void RegisterPasses( RenderGraphBuilder& ) override
        {
        }

        // Whether the scene target needs an expansion and one was built for it.
        bool IsReady() const
        {
            return m_Pipeline && m_Material;
        }

        // Inside the render pass the frame graph opens on the multisampled scene depth ("Deferred: DepthExpand").
        Common::BoolResultStr Record( const std::shared_ptr<Image2D>& gbufferDepth )
        {
            if ( !IsReady() || !gbufferDepth )
                return Common::MakeError( "DepthExpand recorded without its pipeline or the G-buffer depth" );
            m_Material->BindInputs( gbufferDepth );
            Renderer::GetInstance().SubmitFullscreenQuad( m_Pipeline.get(), m_Material->GetMaterialExecutor() );
            return BOOLSUCCESS;
        }

    private:
        std::shared_ptr<Shader>              m_Shader;
        std::shared_ptr<GraphicsPipeline>    m_Pipeline;
        std::unique_ptr<MaterialDepthExpand> m_Material;
    };
} // namespace Desert::Graphic::System
