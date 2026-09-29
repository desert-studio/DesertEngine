#pragma once

#include <Engine/Graphic/Systems/RenderSystem.hpp>

#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/Materials/Deferred/MaterialCopy.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

namespace Desert::Graphic::System
{
    // Full-screen copy: snapshots a source image into this system's target framebuffer. Used to copy the
    // composited scene colour into a separate texture the glass pass samples (screen-space refraction), so the
    // glass pass never reads + writes the same attachment (feedback loop).
    class CopyRenderer final : public RenderSystem
    {
    public:
        using RenderSystem::RenderSystem;

        virtual Common::BoolResultStr Initialize() override
        {
            m_Shader = Runtime::ResourceRegistry::GetShaderService()->GetByName( "Copy" );
            if ( !m_Shader )
                return Common::MakeError( "Copy shader not found" );

            const auto& target = m_TargetFramebuffer.lock();
            if ( !target )
                return Common::MakeError( "Copy target framebuffer missing" );

            GraphicsPipelineSpecification spec;
            spec.DebugName         = "Copy";
            spec.Framebuffer       = target;
            spec.Shader            = m_Shader;
            spec.DepthTestEnabled  = false;
            spec.DepthWriteEnabled = false;
            const auto pipeline    = Graphic::GraphicsPipeline::Create( spec );
            if ( !pipeline )
                return Common::MakeError( pipeline.GetError() );
            m_Pipeline = pipeline.GetValue();

            m_Material = std::make_unique<MaterialCopy>();
            return BOOLSUCCESS;
        }

        void RegisterPasses( RenderGraphBuilder& ) override
        {
        }

        void Execute( const std::shared_ptr<Image2D>& src )
        {
            const auto& target = m_TargetFramebuffer.lock();
            if ( !target || !src || !m_Pipeline || !m_Material )
                return;

            // Inside the render pass the frame graph opens on GetImage() ("Deferred: SceneCopy").
            auto& renderer = Renderer::GetInstance();
            m_Material->BindInputs( src );
            renderer.SubmitFullscreenQuad( m_Pipeline.get(), m_Material->GetMaterialExecutor() );
        }

        std::shared_ptr<Image2D> GetImage() const
        {
            const auto& target = m_TargetFramebuffer.lock();
            return target ? target->GetColorAttachmentImage( 0 ) : nullptr;
        }

    private:
        std::shared_ptr<Shader>           m_Shader;
        std::shared_ptr<GraphicsPipeline> m_Pipeline;
        std::unique_ptr<MaterialCopy>     m_Material;
    };
} // namespace Desert::Graphic::System
