#pragma once

#include <Engine/Graphic/Systems/RenderSystem.hpp>

#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/Image.hpp>
#include <Engine/Graphic/Pipeline.hpp>

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>

namespace Desert::Graphic::System
{
    // The camera's own response to a very bright source in frame: ghosts reflected along the sun->centre
    // axis, a halo ring about the sun, and an anamorphic streak. Two quarter-resolution compute passes —
    // a thresholded box-down of the HDR scene (the "flare source"), then one gather that places every
    // feature — added back in by the tonemap exactly the way bloom and the light shafts are.
    //
    // Every feature is an IMAGE of the source, never a drawn sprite: a ghost is the source rescaled about
    // its own centre, the halo reads the source at the pixel's bearing from the sun, the streak is a
    // 16-tap gather along the authored axis. So an occluded sun dims and shortens its own flare with
    // no occlusion code, and the flare deforms with the scene instead of sitting on top of it.
    //
    // The parameters are a POST-PROCESS setting, not the sun light's: this is a property of the lens the
    // scene is photographed through, and it lives beside Bloom and Exposure on Core::SceneSettings
    // ("Lens Flare" category) — the same place UE keeps Lens Flares, in its Bloom group on the
    // post-process volume. The light shafts went the other way (onto the light) because a shaft is
    // scattering in the world, not in the glass.
    class LensFlareRenderer final : public RenderSystem
    {
    public:
        using RenderSystem::RenderSystem;

        struct Params
        {
            bool  Enabled   = false;
            float Intensity = 0.0f;
            float Threshold = 6.0f;

            // Cap on the energy one texel may inject. Not authored: it exists so a single blown-out texel
            // cannot own the whole flare, the same guard the light-shaft mask has. It also sets the scale
            // the authored intensities work against — the physical sun disc reaches the sky pass's own
            // luminance clamp (1000), and un-capped, a ghost of it blew every authored intensity below
            // 0.02 straight to white.
            float MaxBrightness = 12.0f;

            int       GhostCount     = 4;
            float     GhostSpacing   = 0.35f;
            float     GhostSizeNear  = 0.45f;
            float     GhostSizeFar   = 1.1f;
            glm::vec3 GhostTintInner = glm::vec3( 1.0f );
            glm::vec3 GhostTintOuter = glm::vec3( 1.0f );

            float HaloIntensity = 0.0f;
            float HaloRadius    = 0.32f;

            float StreakIntensity = 0.0f;
            float StreakLength    = 0.35f;
            float StreakAngle     = 0.0f; // degrees, 0 = horizontal

            float ChromaShift = 0.0f;
        };

        virtual Common::BoolResultStr Initialize() override;

        void RegisterPasses( RenderGraphBuilder& /*builder*/ ) override
        {
        }

        // Whether the flare is dispatched this frame, decided when the frame graph is built.
        // @p sunScreenUv is the sun's position in [0,1] screen UV; @p screenFade is SunScreen::Fade — 0
        // when the sun is behind the camera or far past the edge, in which case nothing is dispatched (and
        // the graph gets no flare nodes). Keeps the sun position the features pass places the flare at.
        bool Prepare( const glm::vec2& sunScreenUv, float screenFade );
        // Bright pass into source @p mip: mip 0 samples the scene colour (thresholded), mip i samples mip i-1.
        // Records one dispatch, into the frame graph node that declares those two subresources.
        void RecordBrightPass( uint32_t mip );
        // Features: the whole source chain -> the flare image. One dispatch.
        void RecordFeatures();

        uint32_t GetSourceMipLevels() const
        {
            return m_SourceMipLevels;
        }
        const std::shared_ptr<Image2D>& GetSourceImage() const
        {
            return m_SourceImage;
        }
        std::shared_ptr<Image2D> GetSceneColorImage() const;
        void Resize( uint32_t width, uint32_t height );

        void SetParams( const Params& params )
        {
            m_Params = params;
        }

        const Params& GetParams() const
        {
            return m_Params;
        }

        // The flare image the tonemap adds in. When the effect contributes nothing this frame its
        // contents are STALE, and that is safe for the same reason the bloom and shaft images are: the
        // tonemap's flare intensity is derived from the same numbers that gate the dispatches, so it is
        // exactly zero in exactly those frames.
        const std::shared_ptr<Image2D>& GetFlareImage() const
        {
            return m_FlareImage;
        }

    private:
        bool CreateImages( uint32_t width, uint32_t height );
        bool CreatePipelines();

        // The two resolutions are deliberately different, because the two passes want opposite things.
        //
        // The SOURCE is what every feature is an image OF, and the ghosts magnify it — at quarter
        // resolution the sun disc is about seven texels, and magnifying seven texels showed them. Half
        // resolution gives the disc enough substance to survive being enlarged.
        //
        // The FEATURE image is where the cost is: 64 streak taps plus three fetches per ghost, per texel.
        // A flare is a low-frequency, out-of-focus thing that the tonemap upsamples bilinearly, so
        // quarter resolution costs a quarter of the fetches and looks the same.
        static constexpr uint32_t kSourceDivisor  = 2;
        static constexpr uint32_t kFeatureDivisor = 4;

        // Levels of the source chain. A ghost magnified 16x on screen reads mip 4, so five levels cover
        // the whole authored magnification range (Ghost Size tops out at 16).
        static constexpr uint32_t kMaxSourceMips = 5;

        std::shared_ptr<Image2D> m_SourceImage;
        std::shared_ptr<Image2D> m_FlareImage;
        uint32_t                 m_SourceMipLevels = 1;

        std::shared_ptr<ComputePipeline> m_BrightPassPipeline;
        std::shared_ptr<ComputePipeline> m_FeaturesPipeline;

        Params    m_Params;
        glm::vec2 m_SunScreenUv{ 0.5f };
    };
} // namespace Desert::Graphic::System
