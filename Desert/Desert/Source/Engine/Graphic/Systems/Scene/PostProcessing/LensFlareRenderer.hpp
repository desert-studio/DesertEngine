#pragma once

#include <Engine/Graphic/Systems/RenderSystem.hpp>

#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/Image.hpp>
#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <optional>

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
        // @p screenFade is SunScreen::Fade: 0 when the sun is behind the camera or far past the edge, in which
        // case nothing is dispatched, the graph gets no flare nodes and the tonemap reads the system black
        // texture.
        [[nodiscard]] bool Prepare( float screenFade ) const;

        // The two images of this frame, transients of its graph (Builder::CreateTexture): the half-resolution
        // source chain and the quarter-resolution feature image (FrameTextures::Transients.LensFlare, which the
        // tonemap adds in). The renderer keeps no image and has no Resize. Nullopt: no scene colour.
        [[nodiscard]] std::optional<RDG::TextureDesc> GetSourceDesc() const;
        [[nodiscard]] std::optional<RDG::TextureDesc> GetFlareDesc() const;

        // Bright pass into @p source @p mip: mip 0 samples @p sceneColor (thresholded), mip i samples mip i-1.
        // Called from the exec of the node that declares exactly those two subresources.
        [[nodiscard]] Common::BoolResultStr RecordBrightPass( const RDG::PassContext& context,
                                                              RDG::TextureRef sceneColor, RDG::TextureRef source,
                                                              const RDG::TextureDesc& sourceDesc, uint32_t mip );
        // Features: the whole @p source chain -> @p flare, placed about @p sunScreenUv ([0,1] screen UV).
        [[nodiscard]] Common::BoolResultStr RecordFeatures( const RDG::PassContext& context,
                                                            RDG::TextureRef source, RDG::TextureRef flare,
                                                            const RDG::TextureDesc& flareDesc,
                                                            const glm::vec2&        sunScreenUv );

        void SetParams( const Params& params )
        {
            m_Params = params;
        }
        const Params& GetParams() const
        {
            return m_Params;
        }

    private:
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

        std::shared_ptr<ComputePipeline> m_BrightPassPipeline;
        std::shared_ptr<ComputePipeline> m_FeaturesPipeline;

        Params m_Params;
    };
} // namespace Desert::Graphic::System
