#pragma once

#include <Engine/Graphic/Systems/RenderSystem.hpp>

#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Core/Camera.hpp>

#include <Engine/Graphic/Materials/PostProcessing/MaterialTonemap.hpp>

namespace Desert::Graphic::System
{
    class TonemapRenderer final : public RenderSystem
    {
    public:
        using RenderSystem::RenderSystem;

        virtual Common::BoolResultStr Initialize() override;

        // Tonemap is a raster node of the frame graph (SceneRendererFramePostFX.cpp "PostFX: Tonemap"), after
        // the Jump Flood outline; it is not registered through RegisterPasses.
        void RegisterPasses( RenderGraphBuilder& /*builder*/ ) override
        {
        }

        // Every image the tonemap samples, as bound this frame: the frame graph declares each one as a read of
        // the tonemap node. A null image is one no pass has handed over (yet).
        struct Inputs
        {
            std::shared_ptr<Image2D> Source; // the configured source framebuffer's colour 0
            std::shared_ptr<Image2D> Bloom;
            std::shared_ptr<Image2D> AutoExposure;
            std::shared_ptr<Image2D> LightShafts;
            std::shared_ptr<Image2D> LensFlare;
        };
        Inputs GetInputs() const
        {
            const auto source = m_TargetFramebuffer.lock();
            return { source ? source->GetColorAttachmentImage() : nullptr, m_BloomImage.lock(),
                     m_AutoExposureImage.lock(), m_LightShaftImage.lock(), m_LensFlareImage.lock() };
        }
        // The tonemapped image, colour 0 of GetSystemFramebuffer(): the node's ColorTarget.
        std::shared_ptr<Image2D> GetOutputImage() const
        {
            return m_Framebuffer ? m_Framebuffer->GetColorAttachmentImage( 0 ) : nullptr;
        }

        // Records the fullscreen tonemap inside the render pass the frame graph opens on GetOutputImage().
        void Record();

        void Resize( uint32_t width, uint32_t height );

        // Tonemap parameters, refreshed from SceneSettings each frame (SceneRenderer::BeginScene).
        void SetParams( float exposure, float gamma )
        {
            m_Exposure = exposure;
            m_Gamma    = gamma;
        }

        // The scene's tonemapping curve. ACES is the default and the one the sky programme measures
        // against a UE frame; Reinhard is what every scene authored before 2026-08-19 was graded on.
        void SetTonemapOperator( Core::TonemapOperator op )
        {
            m_TonemapOperator = op;
        }

        // The luminance that maps to pure white — REINHARD only. At 1 that operator is the identity and
        // every HDR value above 1 clips flat; see the note in SceneComposite.shader. The ACES branch
        // never reads it, which is why the editor hides the slider in that mode.
        void SetWhitePoint( float whitePoint )
        {
            m_WhitePoint = whitePoint;
        }

        // The bloom result image (mip-0 of the compute bloom chain) and its strength (0 disables bloom).
        // Tonemap samples it and adds it to the scene before tonemapping. Recreated on resize, so it is
        // re-set by SceneRenderer after a resize.
        void SetBloomImage( const std::shared_ptr<Image2D>& bloom )
        {
            m_BloomImage = bloom;
        }
        void SetBloomIntensity( float intensity )
        {
            m_BloomIntensity = intensity;
        }
        // Lens dispersion (chromatic fringe) strength on the bloom halo (0 = off).
        void SetChromaticBloom( float strength )
        {
            m_ChromaticBloom = strength;
        }

        // Auto-exposure (eye adaptation): the 1x1 adapted-luminance image + key, set per-frame. When
        // disabled, the manual Exposure is used instead.
        void SetAutoExposureImage( const std::shared_ptr<Image2D>& image )
        {
            m_AutoExposureImage = image;
        }
        void SetAutoExposure( bool enabled, float key )
        {
            m_AutoExposureEnabled = enabled;
            m_ExposureKey         = key;
        }

        // The sun light's radial streaks (LightShaftRenderer) and their strength — the intensity is
        // Bloom Scale x the sun's screen-edge fade, computed by SceneRenderer from the SAME params that
        // decided whether the shaft dispatches ran, so a zero here always means the image is inert.
        void SetLightShaftImage( const std::shared_ptr<Image2D>& shafts )
        {
            m_LightShaftImage = shafts;
        }
        void SetLightShafts( float intensity, const glm::vec3& tint )
        {
            m_LightShaftIntensity = intensity;
            m_LightShaftTint      = tint;
        }

        // The lens flare (LensFlareRenderer) and its strength — Intensity x the sun's screen-edge fade,
        // on the same contract as the shafts above: zero here always means the image contributes nothing.
        void SetLensFlareImage( const std::shared_ptr<Image2D>& flare )
        {
            m_LensFlareImage = flare;
        }
        void SetLensFlare( float intensity, const glm::vec3& tint )
        {
            m_LensFlareIntensity = intensity;
            m_LensFlareTint      = tint;
        }

    private:
        std::shared_ptr<GraphicsPipeline> m_Pipeline;
        std::shared_ptr<Shader>   m_Shader;

        std::unique_ptr<MaterialTonemap> m_MaterialTonemap;

        Core::TonemapOperator m_TonemapOperator = Core::TonemapOperator::ACES;
        float                 m_Exposure        = 1.0f;
        float                 m_Gamma           = 2.2f;
        float                 m_WhitePoint      = 8.0f;

        std::weak_ptr<Image2D> m_BloomImage;
        float                  m_BloomIntensity = 0.0f;
        float                  m_ChromaticBloom = 0.0f;

        std::weak_ptr<Image2D> m_AutoExposureImage;
        bool                   m_AutoExposureEnabled = false;
        float                      m_ExposureKey         = 0.18f;

        std::weak_ptr<Image2D> m_LightShaftImage;
        float                  m_LightShaftIntensity = 0.0f;
        glm::vec3              m_LightShaftTint      = glm::vec3( 1.0f );

        std::weak_ptr<Image2D> m_LensFlareImage;
        float                  m_LensFlareIntensity = 0.0f;
        glm::vec3              m_LensFlareTint      = glm::vec3( 1.0f );
    };
} // namespace Desert::Graphic::System