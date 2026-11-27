#pragma once

#include <Engine/Graphic/Systems/RenderSystem.hpp>

#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/ShaderBindingLayoutCache.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Core/Camera.hpp>
#include <Engine/Assets/Common.hpp>

#include <Engine/Graphic/Materials/PostProcessing/MaterialTonemap.hpp>
#include <Engine/Graphic/PostProcessing/LocalExposureRules.hpp>

namespace Desert::Graphic::System
{
    // The shader's parameters: the one spelling the pass binds and Initialize checks.
    struct TonemapShaderParameters
    {
        DESERT_SHADER_PARAMETER( Texture, GeometryTexture );
        DESERT_SHADER_PARAMETER( Sampler, LinearRepeatSampler );
        DESERT_SHADER_PARAMETER( Texture, AverageLuminanceTexture );
        DESERT_SHADER_PARAMETER( Texture, BloomTexture );
        DESERT_SHADER_PARAMETER( Sampler, LinearClampSampler );
        DESERT_SHADER_PARAMETER( Texture, LightShaftTexture );
        DESERT_SHADER_PARAMETER( Texture, LensFlareTexture );
        DESERT_SHADER_PARAMETER( Texture, LumBilateralGrid );
        DESERT_SHADER_PARAMETER( Texture, BlurredLogLuminance );
        DESERT_SHADER_PARAMETER( Texture, ExposureFusionTexture );
        DESERT_SHADER_PARAMETER( Texture, ColorGradingLUT );
        DESERT_SHADER_PARAMETER( Texture, BloomDirtMaskTexture );
        static constexpr std::array kAll{
             RDG::Erase( GeometryTexture ),         RDG::Erase( LinearRepeatSampler ),
             RDG::Erase( AverageLuminanceTexture ), RDG::Erase( BloomTexture ),
             RDG::Erase( LinearClampSampler ),      RDG::Erase( LightShaftTexture ),
             RDG::Erase( LensFlareTexture ),        RDG::Erase( LumBilateralGrid ),
             RDG::Erase( BlurredLogLuminance ),     RDG::Erase( ExposureFusionTexture ),
             RDG::Erase( ColorGradingLUT ),         RDG::Erase( BloomDirtMaskTexture ) };
    };

    class TonemapRenderer final : public RenderSystem
    {
    public:
        using RenderSystem::RenderSystem;

        virtual Common::BoolResultStr Initialize() override;

        // Tonemap is a raster node of the frame graph (SceneRendererFramePostFX.cpp "PostFX: Tonemap"), after
        // the Jump Flood outline.

        // Every image the tonemap samples, as bound this frame: the frame graph declares each one as a read of
        // the tonemap node. A null image is one no pass has handed over (yet).
        // The source is the frame's post input (SceneRenderer::AddFrameTonemap's ref: the temporal output, or
        // the scene colour), never read from a framebuffer here.
        struct Inputs
        {
            std::shared_ptr<Image2D> AutoExposure;
        };
        [[nodiscard]] Inputs GetInputs() const
        {
            return { m_AutoExposureImage.lock() };
        }
        // The tonemapped image, colour 0 of GetSystemFramebuffer(): the node's ColorTarget.
        [[nodiscard]] std::shared_ptr<Image2D> GetOutputImage() const
        {
            return m_Framebuffer ? m_Framebuffer->GetColorAttachmentImage( 0 ) : nullptr;
        }
        // THE HDR RENDITION (HDR-OUTPUT p3, Graphic::SdrCaptureRenditionRequested): while a capture of an HDR
        // output is pending, the output image carries the SDR output transform (what the capture's PNG is) and
        // this second target the HDR transform of the same frame, where the capture's peak is measured. Created
        // on the first such frame; null before. Holds the extended-sRGB encoding SceneComposite writes.
        [[nodiscard]] std::shared_ptr<Image2D> GetHdrRenditionImage() const
        {
            return m_HdrFramebuffer ? m_HdrFramebuffer->GetColorAttachmentImage( 0 ) : nullptr;
        }
        // This frame writes the HDR rendition: a capture is pending on an HDR output with headroom above white.
        // Creates the target the first time it answers yes (the graph imports it right after).
        [[nodiscard]] bool PrepareHdrRendition();
        // The headroom the grade table of a target is baked at (CombineLUTsRenderer::GradeFor): the HDR
        // rendition's is the display's; the output's is the display's on an HDR output (1 on SDR), and 1 on a
        // frame that also writes the rendition (the output then carries the SDR transform).
        [[nodiscard]] float TargetHeadroom( bool hdrRendition ) const;

        // The graph resources one tonemap exec binds by shader name. Bloom is FrameTextures::Transients.Bloom,
        // or the graph's FrameTextures::System.Black when no bloom pass ran this frame (black adds nothing: the
        // bloom carries its own strength). BloomDirtMask is the view's dirt texture imported, or System.Black with
        // BloomDirtMaskProduced false and the mask's intensity 0 for this draw.
        // LightShafts and LensFlare follow the same rule: FrameTextures::Transients.LightShafts / .LensFlare when
        // their nodes ran this frame, otherwise System.Black with that effect's intensity 0 for this draw.
        struct GraphInputs
        {
            RDG::TextureRef Source;       // the scene colour (GeometryTexture), imported
            RDG::TextureRef AvgLuminance; // the adapted luminance (AverageLuminanceTexture); System.White without one
            RDG::TextureRef Bloom;
            bool            BloomProduced = false;
            RDG::TextureRef BloomDirtMask;
            bool            BloomDirtMaskProduced = false;
            RDG::TextureRef LightShafts;
            bool            LightShaftsProduced = false;
            RDG::TextureRef LensFlare;
            bool            LensFlareProduced = false;
            // Local exposure: FrameTransients::LocalExposureGrid / BlurredLogLuminance when the setup ran, else
            // System.Black3D / System.Black and LocalExposureFrame::Produced false (local exposure 1 this draw).
            RDG::TextureRef LocalExposureGrid;
            RDG::TextureRef BlurredLogLuminance;
            // Fusion: FrameTransients::ExposureFusion when the fusion ran, else System.Black (not read then).
            RDG::TextureRef ExposureFusion;
            RDG::TextureRef ColorGradingLUT; // the baked grade (CombineLUTsRenderer) for this pass's target
        };

        // This frame's local exposure as SceneRenderer::AddFrameLocalExposure built it.
        struct LocalExposureFrame
        {
            // the grid (and the blur, when the base uses it) ran this frame — or, under Params.Method Fusion, the
            // fusion pyramid did
            bool                Produced  = false;
            bool                Visualize = false; // DeferredDebugMode::LocalExposure: draw the exposure itself
            LocalExposureParams Params;
            float               LogMiddleGrey = 0.0f;
            float               SliceLogMin   = 0.0f;
            glm::vec2           GridUVScale{ 1.0f };
            glm::vec2           BlurUVScale{ 1.0f };
        };
        void SetLocalExposure( const LocalExposureFrame& frame )
        {
            m_LocalExposure = frame;
        }
        [[nodiscard]] const LocalExposureFrame& GetLocalExposure() const
        {
            return m_LocalExposure;
        }
        // SETUP of "PostFX: Tonemap", first: the material's values, from this frame's settings and @p inputs'
        // effect flags (an effect that did not run draws with intensity 0). Filled before DeclareBindings so the
        // setup validates the block against the route fill the draw will use.
        void FillMaterial( const GraphInputs& inputs );
        // SETUP of "PostFX: Tonemap": the node's one block (block 0) - every input as a SampledGraphics entry.
        // @p hdrRendition: the pass that writes GetHdrRenditionImage (its own material: the HDR transform).
        void DeclareBindings( RDG::PassBuilder& pass, const GraphInputs& inputs, bool hdrRendition = false ) const;
        // Records the fullscreen tonemap inside the render pass the frame graph opens on GetOutputImage(), from
        // block 0 that DeclareBindings declared and the material FillMaterial filled.
        [[nodiscard]] Common::BoolResultStr Record( const RDG::PassContext& context, bool hdrRendition = false );

        void Resize( uint32_t width, uint32_t height );

        // Tonemap parameters, refreshed from SceneSettings each frame (SceneRenderer::BeginScene).
        // The display transfer is not a parameter: the composite encodes with the sRGB OETF
        // (SceneComposite.shader).
        void SetParams( float exposure )
        {
            m_Exposure = exposure;
        }
        // The manual exposure (the bloom's local exposure uses it when auto-exposure is off).
        [[nodiscard]] float GetExposure() const
        {
            return m_Exposure;
        }
        [[nodiscard]] bool IsAutoExposureEnabled() const
        {
            return m_AutoExposureEnabled;
        }
        [[nodiscard]] Core::TonemapOperator GetTonemapOperator() const
        {
            return m_TonemapOperator;
        }

        // The scene's tonemapping curve (Filmic by default, UE's). The grade table bakes it (CombineLUTsRenderer);
        // the composite reads it only to invert Reinhard for the exposure fusion.
        void SetTonemapOperator( Core::TonemapOperator op )
        {
            m_TonemapOperator = op;
        }

        // The bloom dirt mask (UE BloomDirtMask / Intensity / Tint), the view's resolved post-process values: the
        // texture is resolved and imported by the graph (GraphInputs::BloomDirtMask), the numbers drawn here.
        void SetBloomDirtMask( Assets::AssetHandle texture, float intensity, const glm::vec3& tint )
        {
            m_BloomDirtMask          = texture;
            m_BloomDirtMaskIntensity = intensity;
            m_BloomDirtMaskTint      = tint;
        }
        [[nodiscard]] Assets::AssetHandle GetBloomDirtMask() const
        {
            return m_BloomDirtMaskIntensity > 0.0f ? m_BloomDirtMask : Assets::AssetHandle{};
        }
        // Lens dispersion (chromatic fringe) strength on the bloom halo (0 = off).
        void SetChromaticBloom( float strength )
        {
            m_ChromaticBloom = strength;
        }

        // Auto-exposure (eye adaptation): the 1x1 adapted-luminance image + key, set per-frame. When
        // disabled, the manual Exposure is used instead.
        // This frame's ViewFrame::PreExposure (> 0): the composite divides the scene colour by it.
        void SetPreExposure( float preExposure )
        {
            m_PreExposure = preExposure;
        }
        void SetAutoExposureImage( const std::shared_ptr<Image2D>& image )
        {
            m_AutoExposureImage = image;
        }
        void SetAutoExposure( bool enabled )
        {
            m_AutoExposureEnabled = enabled;
        }
        // This frame's view shows a display-referred buffer (Graphic::IsDisplayReferredView of the view's
        // DebugViewState): the composite draws the scene colour as is, unexposed and untonemapped.
        void SetVisualizeBuffer( bool enabled )
        {
            m_VisualizeBuffer = enabled;
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

        // THE LENS STAGE (LENS-POST): film grain, chromatic aberration, vignette and Panini projection, applied in
        // the composite itself (SceneComposite.shader), so the lens costs no pass of its own. The values are the
        // view's resolved post-process volume (SceneRenderer, from Core::PostProcessSettings), the only source.
        struct LensSettings
        {
            float FilmGrainIntensity             = 0.0f;
            float FilmGrainTexelSize             = 1.0f;
            float ChromaticAberrationIntensity   = 0.0f; // UE SceneFringeIntensity
            float ChromaticAberrationStartOffset = 0.0f;
            float VignetteIntensity              = 0.0f;
            float PaniniD                        = 0.0f;
            float PaniniS                        = 0.0f;
        };
        void SetLens( const LensSettings& lens )
        {
            m_Lens = lens;
        }
        // This frame's view: the unjittered projection's half-FOV tangents (Panini maps through them) and the
        // view's frame count (the grain's per-frame seed).
        void SetLensView( const glm::vec2& tanHalfFov, const uint64_t frameIndex )
        {
            m_LensTanHalfFov = tanHalfFov;
            m_LensFrameIndex = frameIndex;
        }

    private:
        // The shader's three lens vectors (MaterialTonemap::Params) from m_Lens and this frame's view.
        void FillLens( MaterialTonemap::Params& params ) const;

        std::shared_ptr<GraphicsPipeline> m_Pipeline;
        mutable ShaderBindingLayoutCache  m_BindingLayout; // the tonemap shader's layout, kept between frames

        std::unique_ptr<MaterialTonemap> m_MaterialTonemap;
        // HDR-OUTPUT p3: the HDR rendition's target and material (PrepareHdrRendition); null until first used.
        std::shared_ptr<Framebuffer>     m_HdrFramebuffer;
        std::unique_ptr<MaterialTonemap> m_MaterialTonemapHdr;
        bool                             m_HdrRendition = false; // this frame (FillMaterial's SDR transform)

        Core::TonemapOperator m_TonemapOperator = Core::TonemapOperator::ACES;
        float                 m_Exposure        = 1.0f;
        float                 m_PreExposure     = 1.0f;

        Assets::AssetHandle m_BloomDirtMask;
        float               m_BloomDirtMaskIntensity = 0.0f;
        glm::vec3           m_BloomDirtMaskTint      = glm::vec3( 0.5f );
        float               m_ChromaticBloom         = 0.0f;

        std::weak_ptr<Image2D> m_AutoExposureImage;
        bool                   m_AutoExposureEnabled = false;
        bool                       m_VisualizeBuffer     = false; // SetVisualizeBuffer

        float                  m_LightShaftIntensity = 0.0f;
        glm::vec3              m_LightShaftTint      = glm::vec3( 1.0f );

        LocalExposureFrame     m_LocalExposure;
        float                  m_LensFlareIntensity = 0.0f;
        glm::vec3              m_LensFlareTint      = glm::vec3( 1.0f );

        LensSettings m_Lens;                         // SetLens
        glm::vec2    m_LensTanHalfFov{ 1.0f, 1.0f }; // SetLensView
        uint64_t     m_LensFrameIndex = 0;           // SetLensView
    };
} // namespace Desert::Graphic::System