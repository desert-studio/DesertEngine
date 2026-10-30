#include "TonemapRenderer.hpp"
#include <Engine/Graphic/ViewTargetFormats.hpp>
#include <Engine/Graphic/DisplayOutput.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>

#include <Engine/Runtime/ResourceRegistry.hpp>

namespace Desert::Graphic::System
{
    Common::BoolResultStr TonemapRenderer::Initialize()
    {
        const auto& targetFramebuffer = m_TargetFramebuffer.lock();
        if ( !targetFramebuffer )
        {
            DESERT_VERIFY( false );
        }

        constexpr std::string_view debugName = "SceneToneMap";

        // Framebuffer
        FramebufferSpecification fbSpec;
        fbSpec.DebugName = debugName;
        fbSpec.Attachments.Attachments.emplace_back( ViewTargetFormats::kTonemap );

        m_Framebuffer = Graphic::Framebuffer::Create( fbSpec );
        m_Framebuffer->Resize( targetFramebuffer->GetFramebufferWidth(),
                               targetFramebuffer->GetFramebufferHeight() );

        // Pipeline
        const auto shader = Runtime::ResourceRegistry::GetShaderService()->GetByName( "SceneComposite" );

        Graphic::GraphicsPipelineSpecification pipeSpec;
        pipeSpec.DebugName   = debugName;
        pipeSpec.Framebuffer = m_Framebuffer;
        pipeSpec.Shader      = shader;

        // shader is whatever GetByName returned, INCLUDING nullptr — this site never checked, and a
        // null shader used to be dereferenced inside the backend. Create's rule names it now.
        const auto pipeline = Graphic::GraphicsPipeline::Create( pipeSpec );
        if ( !pipeline )
            return Common::MakeError( pipeline.GetError() );
        m_Pipeline = pipeline.GetValue();

        m_MaterialTonemap = std::make_unique<MaterialTonemap>();

        if ( const auto valid = m_BindingLayout.Validate( m_Pipeline->GetSpecification().Shader, TonemapShaderParameters::kAll );
             !valid )
            return valid;
        return BOOLSUCCESS;
    }

    void TonemapRenderer::Resize( uint32_t width, uint32_t height )
    {
        if ( m_Framebuffer )
            m_Framebuffer->Resize( width, height );
        if ( m_HdrFramebuffer )
            m_HdrFramebuffer->Resize( width, height );
    }

    bool TonemapRenderer::PrepareHdrRendition()
    {
        const DisplayOutputState& display = CurrentDisplayOutput();
        m_HdrRendition =
             m_Framebuffer && SdrCaptureRenditionRequested() && display.IsHDR() && display.Headroom > 1.0f;
        if ( !m_HdrRendition )
            return false;
        if ( !m_HdrFramebuffer )
        {
            // The output's twin: same format (the one pipeline draws both), same size.
            FramebufferSpecification fbSpec;
            fbSpec.DebugName = "SceneToneMap.HdrRendition";
            fbSpec.Attachments.Attachments.emplace_back( ViewTargetFormats::kTonemap );
            m_HdrFramebuffer = Graphic::Framebuffer::Create( fbSpec );
            m_HdrFramebuffer->Resize( m_Framebuffer->GetFramebufferWidth(),
                                      m_Framebuffer->GetFramebufferHeight() );
            m_MaterialTonemapHdr = std::make_unique<MaterialTonemap>();
        }
        return true;
    }

    void TonemapRenderer::DeclareBindings( RDG::PassBuilder& pass, const GraphInputs& inputs,
                                           const bool hdrRendition ) const
    {
        const MaterialTonemap* material = hdrRendition ? m_MaterialTonemapHdr.get() : m_MaterialTonemap.get();
        if ( !m_Pipeline || material == nullptr )
            return;
        // The samplers the material route sampled these with before: the scene colour and the luminance with the
        // image's own (linear, REPEAT), the three effect images with LinearClamp at mip 0. Any of the three may
        // be the same System.Black ref: three read entries of one ref in one state are one read of the pass
        // (RenderGraphCompile TwoBlockEntriesReadingOneImageInOneState...).
        pass.Bindings( m_BindingLayout.Get( m_Pipeline->GetSpecification().Shader ),
                       material->GetMaterialExecutor()->GetRouteFill() )
             .Texture( TonemapShaderParameters::GeometryTexture, inputs.Source, RDG::Access::SampledGraphics,
                       RDG::SubresourceRange::All() )
             .Sampler( TonemapShaderParameters::LinearRepeatSampler, RDG::SamplerDesc::LinearRepeat() )
             .Texture( TonemapShaderParameters::AverageLuminanceTexture, inputs.AvgLuminance,
                       RDG::Access::SampledGraphics, RDG::SubresourceRange::All() )
             .Texture( TonemapShaderParameters::BloomTexture, inputs.Bloom, RDG::Access::SampledGraphics,
                       RDG::SubresourceRange::Mip( 0 ) )
             .Sampler( TonemapShaderParameters::LinearClampSampler, RDG::SamplerDesc::LinearClamp() )
             .Texture( TonemapShaderParameters::LightShaftTexture, inputs.LightShafts,
                       RDG::Access::SampledGraphics, RDG::SubresourceRange::Mip( 0 ) )
             .Texture( TonemapShaderParameters::LensFlareTexture, inputs.LensFlare, RDG::Access::SampledGraphics,
                       RDG::SubresourceRange::Mip( 0 ) )
             .Texture( TonemapShaderParameters::LumBilateralGrid, inputs.LocalExposureGrid,
                       RDG::Access::SampledGraphics, RDG::SubresourceRange::All() )
             .Texture( TonemapShaderParameters::BlurredLogLuminance, inputs.BlurredLogLuminance,
                       RDG::Access::SampledGraphics, RDG::SubresourceRange::All() )
             .Texture( TonemapShaderParameters::ExposureFusionTexture, inputs.ExposureFusion,
                       RDG::Access::SampledGraphics, RDG::SubresourceRange::All() );
    }

    void TonemapRenderer::FillMaterial( const GraphInputs& inputs )
    {
        if ( !m_MaterialTonemap )
            return;
        // An effect whose nodes did not run this frame reads System.Black and adds nothing.
        const float bloomIntensity      = inputs.BloomProduced ? m_BloomIntensity : 0.0f;
        const float lightShaftIntensity = inputs.LightShaftsProduced ? m_LightShaftIntensity : 0.0f;
        const float lensFlareIntensity  = inputs.LensFlareProduced ? m_LensFlareIntensity : 0.0f;

        MaterialTonemap::Params params{
             m_TonemapOperator,     m_Exposure,         bloomIntensity, m_ExposureKey,
             m_AutoExposureEnabled, m_ChromaticBloom,   m_WhitePoint,   lightShaftIntensity,
             m_LightShaftTint,      lensFlareIntensity, m_LensFlareTint,    m_PreExposure };
        // Local exposure: identity values + enabled 0 when its nodes did not run (the inputs are then black).
        const LocalExposureFrame& le = m_LocalExposure;
        const LocalExposureParams  p  = le.Produced ? le.Params : LocalExposureParams{};
        params.LocalExposureContrast  = glm::vec4( p.HighlightContrast, p.ShadowContrast, p.DetailStrength, p.BlurredLumBlend );
        params.LocalExposureThreshold = glm::vec4( p.HighlightThreshold, p.ShadowThreshold, p.HighlightThresholdStrength,
                                                   p.ShadowThresholdStrength );
        params.LocalExposureUV        = glm::vec4( le.GridUVScale, le.BlurUVScale );
        // w: 0 off, 1 applied, 2 applied + visualised, 3 visualised without the passes (identity: exposure 1).
        const float mode = le.Produced ? ( le.Visualize ? 2.0f : 1.0f ) : ( le.Visualize ? 3.0f : 0.0f );
        params.LocalExposureSlice = glm::vec4( le.LogMiddleGrey, le.SliceLogMin, 1.0f / kLocalExposureSliceEV, mode );
        // 1: the exposure comes from ExposureFusionTexture (Fusion), 0: from the grid and the blur (Bilateral).
        params.LocalExposureMethod = le.Produced && IsLocalExposureFusion( le.Params ) ? 1.0f : 0.0f;

        // The display the frame reaches (the swapchain's last build): its output transform and headroom.
        const DisplayOutputState& display = CurrentDisplayOutput();
        params.DisplayOutput              = display.Output;
        params.DisplayHeadroom            = display.Headroom;

        // HDR-OUTPUT p3: with the HDR rendition this frame (PrepareHdrRendition) the output carries the SDR
        // transform — headroom 1, SceneComposite's SDR arm, the very values the SDR output writes — and the
        // rendition the HDR one; the SAME params otherwise, so the two are one frame in two transforms.
        if ( m_HdrRendition && m_MaterialTonemapHdr )
        {
            m_MaterialTonemapHdr->BindValues( params );
            params.DisplayHeadroom = 1.0f;
        }
        // A display-referred buffer view: no effect is composited over the stored value (UE buffer visualization).
        if ( m_VisualizeBuffer )
        {
            params.VisualizeBuffer      = true;
            params.BloomIntensity       = 0.0f;
            params.LightShaftIntensity  = 0.0f;
            params.LensFlareIntensity   = 0.0f;
            params.LocalExposureSlice.w = 0.0f;
        }
        m_MaterialTonemap->BindValues( params );
    }

    Common::BoolResultStr TonemapRenderer::Record( const RDG::PassContext& context, const bool hdrRendition )
    {
        MaterialTonemap* material = hdrRendition ? m_MaterialTonemapHdr.get() : m_MaterialTonemap.get();
        if ( !m_Pipeline || material == nullptr )
            return Common::MakeError( "PostFX: Tonemap: the tonemap pipeline is not initialised" );
        const auto& framebuffer =
             m_TargetFramebuffer.lock(); // We call lock internally to avoid cyclic dependencies.
        if ( !framebuffer )
            return Common::MakeError( "TonemapRenderer: the source framebuffer was destroyed or wasn't set up" );

        const RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
        return Renderer::GetInstance().DrawFullscreen( bindings, *m_Pipeline, material->GetMaterialExecutor() );
    }
} // namespace Desert::Graphic::System
