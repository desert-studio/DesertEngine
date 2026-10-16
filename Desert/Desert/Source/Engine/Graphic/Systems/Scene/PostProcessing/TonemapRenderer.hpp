#pragma once

#include <Engine/Graphic/Systems/RenderSystem.hpp>

#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/ShaderBindingLayoutCache.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Core/Camera.hpp>

#include <Engine/Core/PostProcessSettings.hpp>
#include <Engine/Graphic/Materials/PostProcessing/MaterialTonemap.hpp>

namespace Desert::Graphic::System
{
    class TonemapRenderer final : public RenderSystem
    {
    public:
        using RenderSystem::RenderSystem;

        virtual Common::BoolResultStr Initialize() override;

        // Tonemap is a raster node of the frame graph (SceneRendererFramePostFX.cpp "PostFX: Tonemap"). It keeps
        // no image: every input is a ref of the frame graph (GraphInputs), and its target is the ref the frame
        // hands it - the view's output when it is the last pass of the post chain, otherwise a transient the
        // spatial AA reads (UE: FScreenPassTexture in, FScreenPassRenderTarget out). The pipeline is built against
        // the graph's render pass for one ViewTargetFormats::kTonemap colour target.

        // The graph resources one tonemap exec binds by shader name. Bloom is FrameTextures::Transients.Bloom,
        // or the graph's FrameTextures::System.Black when the bloom chain did not run this frame; then
        // BloomProduced is false and the bloom intensity is 0 for this draw, whatever SetBloomIntensity said.
        // LightShafts and LensFlare follow the same rule: FrameTextures::Transients.LightShafts / .LensFlare when
        // their nodes ran this frame, otherwise System.Black with that effect's intensity 0 for this draw.
        struct GraphInputs
        {
            RDG::TextureRef Source;       // the scene colour (u_GeometryTexture), imported
            RDG::TextureRef AvgLuminance; // the adapted luminance (u_AvgLuminance); System.White without one
            RDG::TextureRef ColorGradingLUT; // the baked grade (u_ColorGradingLUT), CombineLUTsRenderer's table
            RDG::TextureRef Bloom;
            bool            BloomProduced = false;
            RDG::TextureRef LightShafts;
            bool            LightShaftsProduced = false;
            RDG::TextureRef LensFlare;
            bool            LensFlareProduced = false;
        };
        // SETUP of "PostFX: Tonemap", first: the material's values, from this frame's settings and @p inputs'
        // effect flags (an effect that did not run draws with intensity 0). Filled before DeclareBindings so the
        // setup validates the block against the route fill the draw will use.
        void FillMaterial( const GraphInputs& inputs );
        // SETUP of "PostFX: Tonemap": the node's one block (block 0) - every input as a SampledGraphics entry.
        void DeclareBindings( RDG::PassBuilder& pass, const GraphInputs& inputs ) const;
        // Records the fullscreen tonemap inside the render pass the frame graph opens on the node's target, from
        // block 0 that DeclareBindings declared and the material FillMaterial filled.
        [[nodiscard]] Common::BoolResultStr Record( const RDG::PassContext& context );

        // The view's grade (Graphic::ResolveViewSettings), refreshed each frame by SceneRenderer::BeginScene. The
        // tonemap reads only its exposure (Graphic::ViewExposureOf); the rest of the grade reaches the pixel
        // through the colour-grading table CombineLUTsRenderer bakes from the same settings.
        void SetGrade( const Core::PostProcessSettings& grade )
        {
            m_Grade = grade;
        }

        // The bloom strength (0 disables bloom). The bloom image itself is a graph transient (GraphInputs).
        void SetBloomIntensity( float intensity )
        {
            m_BloomIntensity = intensity;
        }
        // Lens dispersion (chromatic fringe) strength on the bloom halo (0 = off).
        void SetChromaticBloom( float strength )
        {
            m_ChromaticBloom = strength;
        }

        // The sun light's radial streaks (LightShaftRenderer) and their strength — the intensity is
        // Bloom Scale x the sun's screen-edge fade, computed by SceneRenderer from the SAME params that
        // decided whether the shaft dispatches ran. The image is a graph transient (GraphInputs).
        void SetLightShafts( float intensity, const glm::vec3& tint )
        {
            m_LightShaftIntensity = intensity;
            m_LightShaftTint      = tint;
        }

        // The lens flare (LensFlareRenderer) and its strength — Intensity x the sun's screen-edge fade,
        // on the same contract as the shafts above. The image is a graph transient (GraphInputs).
        void SetLensFlare( float intensity, const glm::vec3& tint )
        {
            m_LensFlareIntensity = intensity;
            m_LensFlareTint      = tint;
        }

    private:
        std::shared_ptr<GraphicsPipeline> m_Pipeline;
        mutable ShaderBindingLayoutCache  m_BindingLayout; // the tonemap shader's layout, kept between frames

        std::unique_ptr<MaterialTonemap> m_MaterialTonemap;

        Core::PostProcessSettings m_Grade;

        float                  m_BloomIntensity = 0.0f;
        float                  m_ChromaticBloom = 0.0f;

        float                  m_LightShaftIntensity = 0.0f;
        glm::vec3              m_LightShaftTint      = glm::vec3( 1.0f );

        float                  m_LensFlareIntensity = 0.0f;
        glm::vec3              m_LensFlareTint      = glm::vec3( 1.0f );
    };
} // namespace Desert::Graphic::System