#pragma once

#include <Engine/Graphic/Systems/RenderSystem.hpp>

#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/ShaderBindingLayoutCache.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <map>
#include <utility>

namespace Desert::Graphic::System
{
    // The Velocity view mode (DeferredDebugMode::Velocity, UE's "Velocity" buffer visualization): "Debug:
    // Velocity" samples the view's velocity transient (FrameTransients::Velocity, NDC current - previous at the
    // render extent) and writes it as a colour over the view's post input - hue = direction, brightness = speed in
    // pixels, black where the pixel is still. Drawn after the temporal resolve, so what is shown is the motion TAA
    // reprojected with and not a history-blended picture of it.
    //
    // The post input's format and sample count are the view's (the MSAA scene target without a temporal method,
    // the single-sample overlay colour with one), so the pipeline is made per (format, samples) on first use.
    class VelocityViewRenderer final : public RenderSystem
    {
    public:
        using RenderSystem::RenderSystem;

        virtual Common::BoolResultStr Initialize() override
        {
            m_Shader = Runtime::ResourceRegistry::GetShaderService()->GetByName( "VelocityView" );
            if ( !m_Shader )
                return Common::MakeError( "VelocityView shader not found" );
            return BOOLSUCCESS;
        }


        // The pipeline that draws into a target of @p format at @p samples, made the first time it is asked for.
        [[nodiscard]] Common::BoolResultStr Prepare( const Core::Formats::ImageFormat format,
                                                     const uint32_t                   samples )
        {
            if ( !m_Shader )
                return Common::MakeError( "Debug: Velocity: the VelocityView shader is not loaded" );
            const auto key = std::make_pair( static_cast<int>( format ), samples );
            if ( const auto it = m_Pipelines.find( key ); it != m_Pipelines.end() )
            {
                m_Current = it->second;
                return BOOLSUCCESS;
            }
            GraphicsPipelineSpecification spec;
            spec.DebugName = "VelocityView";
            spec.TargetLayout =
                 RenderTargetLayout{ .ColorFormats = { format }, .DepthFormat = std::nullopt, .Samples = samples };
            spec.Shader            = m_Shader;
            spec.DepthTestEnabled  = false;
            spec.DepthWriteEnabled = false;
            const auto pipeline    = Graphic::GraphicsPipeline::Create( spec );
            if ( !pipeline )
                return Common::MakeError( pipeline.GetError() );
            m_Current = m_Pipelines.emplace( key, pipeline.GetValue() ).first->second;
            return BOOLSUCCESS;
        }

        // SETUP of "Debug: Velocity": block 0 - u_Velocity, the shader's only resource, read by texelFetch.
        void DeclareBindings( RDG::PassBuilder& pass, RDG::TextureRef velocity ) const
        {
            if ( !m_Current )
                return;
            pass.Bindings( m_BindingLayout.Get( m_Shader ), RDG::OtherRouteFill{} )
                 .Sampled( "u_Velocity", velocity, RDG::Access::SampledGraphics, RDG::SubresourceRange::All(),
                           RDG::SamplerDesc::LinearRepeat() );
        }

        [[nodiscard]] Common::BoolResultStr Record( const RDG::PassContext& context ) const
        {
            if ( !m_Current )
                return Common::MakeError( "Debug: Velocity: no pipeline was prepared for the target" );
            const RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
            return Renderer::GetInstance().DrawFullscreen( bindings, *m_Current, nullptr );
        }

    private:
        std::shared_ptr<Shader>                                               m_Shader;
        mutable ShaderBindingLayoutCache                                      m_BindingLayout;
        std::map<std::pair<int, uint32_t>, std::shared_ptr<GraphicsPipeline>> m_Pipelines;
        std::shared_ptr<GraphicsPipeline>                                     m_Current;
    };
} // namespace Desert::Graphic::System
