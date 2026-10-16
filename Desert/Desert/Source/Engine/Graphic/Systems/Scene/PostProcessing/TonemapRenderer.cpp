#include "TonemapRenderer.hpp"
#include <Engine/Graphic/ViewTargetFormats.hpp>
#include <Engine/Graphic/PostProcessing/ViewExposure.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>

#include <Engine/Runtime/ResourceRegistry.hpp>

namespace Desert::Graphic::System
{
    Common::BoolResultStr TonemapRenderer::Initialize()
    {
        constexpr std::string_view debugName = "SceneToneMap";

        const auto shader = Runtime::ResourceRegistry::GetShaderService()->GetByName( "SceneComposite" );

        // No framebuffer: the target is a graph texture (the view's output or a transient of the same format),
        // and the pipeline is built against the graph's render pass for that one colour target.
        Graphic::GraphicsPipelineSpecification pipeSpec;
        pipeSpec.DebugName = debugName;
        pipeSpec.TargetLayout =
             RenderTargetLayout{ .ColorFormats = { ViewTargetFormats::kTonemap }, .DepthFormat = std::nullopt };
        pipeSpec.Shader = shader;

        // shader is whatever GetByName returned, INCLUDING nullptr — this site never checked, and a
        // null shader used to be dereferenced inside the backend. Create's rule names it now.
        const auto pipeline = Graphic::GraphicsPipeline::Create( pipeSpec );
        if ( !pipeline )
            return Common::MakeError( pipeline.GetError() );
        m_Pipeline = pipeline.GetValue();

        m_MaterialTonemap = std::make_unique<MaterialTonemap>();

        return BOOLSUCCESS;
    }

    void TonemapRenderer::DeclareBindings( RDG::PassBuilder& pass, const GraphInputs& inputs ) const
    {
        if ( !m_Pipeline || !m_MaterialTonemap )
            return;
        // The samplers the material route sampled these with before: the scene colour and the luminance with the
        // image's own (linear, REPEAT), the three effect images with LinearClamp at mip 0. Any of the three may
        // be the same System.Black ref: three read entries of one ref in one state are one read of the pass
        // (RenderGraphCompile TwoBlockEntriesReadingOneImageInOneState...).
        pass.Bindings( m_BindingLayout.Get( m_Pipeline->GetSpecification().Shader ),
                       m_MaterialTonemap->GetMaterialExecutor()->GetRouteFill() )
             .Sampled( "u_GeometryTexture", inputs.Source, RDG::Access::SampledGraphics,
                       RDG::SubresourceRange::All(), RDG::SamplerDesc::LinearRepeat() )
             .Sampled( "u_AvgLuminance", inputs.AvgLuminance, RDG::Access::SampledGraphics,
                       RDG::SubresourceRange::All(), RDG::SamplerDesc::LinearRepeat() )
             .Sampled( "u_BloomTexture", inputs.Bloom, RDG::Access::SampledGraphics,
                       RDG::SubresourceRange::Mip( 0 ), RDG::SamplerDesc::LinearClamp() )
             .Sampled( "u_LightShaftTexture", inputs.LightShafts, RDG::Access::SampledGraphics,
                       RDG::SubresourceRange::Mip( 0 ), RDG::SamplerDesc::LinearClamp() )
             .Sampled( "u_LensFlareTexture", inputs.LensFlare, RDG::Access::SampledGraphics,
                       RDG::SubresourceRange::Mip( 0 ), RDG::SamplerDesc::LinearClamp() )
             // Trilinear and clamped, as UE samples its table: the encoding puts black and the brightest code on
             // the outer texel centres.
             .Sampled( "u_ColorGradingLUT", inputs.ColorGradingLUT, RDG::Access::SampledGraphics,
                       RDG::SubresourceRange::All(), RDG::SamplerDesc::LinearClamp() );
    }

    void TonemapRenderer::FillMaterial( const GraphInputs& inputs )
    {
        if ( !m_MaterialTonemap )
            return;
        // An effect whose nodes did not run this frame reads System.Black and adds nothing.
        const float bloomIntensity      = inputs.BloomProduced ? m_BloomIntensity : 0.0f;
        const float lightShaftIntensity = inputs.LightShaftsProduced ? m_LightShaftIntensity : 0.0f;
        const float lensFlareIntensity  = inputs.LensFlareProduced ? m_LensFlareIntensity : 0.0f;

        const MaterialTonemap::Params params{ .Exposure            = ViewExposureOf( m_Grade ),
                                              .BloomIntensity      = bloomIntensity,
                                              .ChromaticBloom      = m_ChromaticBloom,
                                              .LightShaftIntensity = lightShaftIntensity,
                                              .LightShaftTint      = m_LightShaftTint,
                                              .LensFlareIntensity  = lensFlareIntensity,
                                              .LensFlareTint       = m_LensFlareTint };

        m_MaterialTonemap->BindValues( params );
    }

    Common::BoolResultStr TonemapRenderer::Record( const RDG::PassContext& context )
    {
        if ( !m_Pipeline || !m_MaterialTonemap )
            return Common::MakeError( "PostFX: Tonemap: the tonemap pipeline is not initialised" );

        const RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
        return Renderer::GetInstance().DrawFullscreen( bindings, *m_Pipeline,
                                                       m_MaterialTonemap->GetMaterialExecutor() );
    }
} // namespace Desert::Graphic::System
