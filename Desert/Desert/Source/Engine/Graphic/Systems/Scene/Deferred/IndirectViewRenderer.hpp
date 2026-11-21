#pragma once

#include <Engine/Graphic/Systems/RenderSystem.hpp>

#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/ShaderBindingLayoutCache.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <map>
#include <utility>
#include <Engine/Runtime/Services/Shader/ShaderService.hpp>

namespace Desert::Graphic::System
{
    // The slot names this pass binds, spelled once (RDGShaderParameters.hpp).
    struct IndirectViewBindingShaderParameters
    {
        DESERT_SHADER_PARAMETER( Texture, GiDiffuseTexture );
        DESERT_SHADER_PARAMETER( Texture, GBufferATexture );
        DESERT_SHADER_PARAMETER( Sampler, LinearClampSampler );
        DESERT_SHADER_PARAMETER( StorageBuffer, IndirectViewUB );
    };

    // The Indirect Lighting view mode (DeferredDebugMode::GI; UE's indirect-diffuse visualisation drawn as a
    // buffer view after the temporal resolve): "Debug: Indirect" multiplies the GI method's diffuse indirect
    // (FrameTransients::GIDiffuse) by the surface's diffuse colour (GBufferA: base colour x (1 - metallic), UE
    // DiffuseIndirectComposite) and writes it over the view's post input, pre-exposed.
    //
    // WHY NOT IN THE COMPOSITE. The scene colour history is copied from the composite's output and the next
    // frame's screen traces (SSGI, SSR) read it back as last frame's lit scene. A view drawn by the composite
    // hands that history a picture of the view: SSGI then bounced its own indirect-only output and the view
    // decayed to black. Drawn here, the composite draws the Lit frame in this mode (DrawsLitComposite) and the
    // history stays lit.
    //
    // The post input's format and sample count are the view's (the MSAA scene target without a temporal method,
    // the single-sample overlay colour with one), so the pipeline is made per (format, samples) on first use.
    class IndirectViewRenderer final : public RenderSystem
    {
    public:
        using RenderSystem::RenderSystem;

        virtual Common::BoolResultStr Initialize() override
        {
            m_Shader = Runtime::ResourceRegistry::GetShaderService()->GetByName( "IndirectView" );
            if ( !m_Shader )
                return Common::MakeError( "IndirectView shader not found" );
            return BOOLSUCCESS;
        }

        // The pipeline that draws into a target of @p format at @p samples, made the first time it is asked for.
        [[nodiscard]] Common::BoolResultStr Prepare( const Core::Formats::ImageFormat format,
                                                     const uint32_t                   samples )
        {
            m_Current = nullptr;
            if ( !m_Shader )
                return Common::MakeError( "Debug: Indirect: the IndirectView shader is not loaded" );
            const auto key = std::make_pair( static_cast<int>( format ), samples );
            if ( const auto it = m_Pipelines.find( key ); it != m_Pipelines.end() )
            {
                m_Current = it->second;
                return BOOLSUCCESS;
            }
            GraphicsPipelineSpecification spec;
            spec.DebugName = "IndirectView";
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

        // SETUP of "Debug: Indirect": block 0 - the GI's diffuse, GBufferA and @p params
        // (IndirectViewUB, one uploaded vec4: x = ViewFrame::PreExposure).
        void DeclareBindings( RDG::PassBuilder& pass, RDG::TextureRef diffuseIndirect, RDG::TextureRef gbufferA,
                              RDG::BufferRef params ) const
        {
            if ( !m_Current )
                return;
            pass.Bindings( m_BindingLayout.Get( m_Shader ), RDG::NoOtherRouteFill() )
                 .Texture( IndirectViewBindingShaderParameters::GiDiffuseTexture, diffuseIndirect,
                           RDG::Access::SampledGraphics, RDG::SubresourceRange::All() )
                 .Texture( IndirectViewBindingShaderParameters::GBufferATexture, gbufferA,
                           RDG::Access::SampledGraphics, RDG::SubresourceRange::All() )
                 .Sampler( IndirectViewBindingShaderParameters::LinearClampSampler,
                           RDG::SamplerDesc::LinearClamp() )
                 .Storage( IndirectViewBindingShaderParameters::IndirectViewUB, params, RDG::Access::StorageRead );
        }

        [[nodiscard]] Common::BoolResultStr Record( const RDG::PassContext& context ) const
        {
            if ( !m_Current )
                return Common::MakeError( "Debug: Indirect: no pipeline was prepared for the target" );
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
