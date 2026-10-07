#pragma once

#include <Engine/Graphic/Systems/RenderSystem.hpp>

#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/Image.hpp>
#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>

#include <glm/glm.hpp>

#include <memory>
#include <optional>

namespace Desert::Graphic::System
{
    // Light shafts from the atmosphere sun — UE's "Light Shaft Bloom": a bright-pass mask of the HDR
    // scene colour around the sun's screen position, radially blurred toward it (Mitchell, GPU Gems 3
    // ch.13), added back in by the tonemap exactly the way bloom is. Occlusion comes for free: whatever
    // are composited into the scene colour with their real transmittance before this runs, so the
    // streaks exist only where the sun actually breaks through.
    //
    // The parameters are the SUN LIGHT's, not a scene setting: DirectionalLightData's Light Shafts
    // category (UE parity), carried here by the ProceduralSkyCommand alongside the sun direction —
    // shafts without a sun to cast them are not a thing.
    class LightShaftRenderer final : public RenderSystem
    {
    public:
        using RenderSystem::RenderSystem;

        struct Params
        {
            bool      Enabled       = false;
            float     BloomScale    = 0.2f;   // UE default
            float     Threshold     = 0.0f;   // UE default
            float     MaxBrightness = 100.0f; // UE default
            glm::vec3 BloomTint     = glm::vec3( 1.0f );
        };

        virtual Common::BoolResultStr Initialize() override;

        void RegisterPasses( RenderGraphBuilder& /*builder*/ ) override
        {
        }

        // The mask and every blur pass are compute nodes of the frame graph (SceneRendererFramePostFX.cpp
        // "PostFX: LightShaft*"). Their two half-resolution images are transients of that graph
        // (Builder::CreateTexture from GetTargetDesc()); the renderer keeps no image and has no Resize.
        // The last blur pass's target is FrameTextures::Transients.LightShafts, which the tonemap adds in.
        // Nullopt: nothing to record (no scene colour or pipelines).
        [[nodiscard]] std::optional<RDG::TextureDesc> GetTargetDesc() const;

        // @p screenFade is the CPU-computed fade for a sun leaving the view (0 = fully off-screen or behind):
        // when this is false the graph gets no shaft nodes and the tonemap reads the system black texture.
        [[nodiscard]] bool IsActive( float screenFade ) const
        {
            return m_Params.Enabled && screenFade > 0.0f && m_Params.BloomScale > 0.0f;
        }

        // Mask: samples @p sceneColor, writes @p mask. @p sunScreenUv is the sun's position in [0,1] screen UV.
        // Called from the exec of the pass that declared exactly those two uses.
        [[nodiscard]] Common::BoolResultStr RecordMask( const RDG::PassContext& context,
                                                        RDG::TextureRef sceneColor, RDG::TextureRef mask,
                                                        const RDG::TextureDesc& desc,
                                                        const glm::vec2&        sunScreenUv );
        // Radial blur pass @p pass: samples @p source, writes @p target; the reach grows per pass.
        [[nodiscard]] Common::BoolResultStr RecordBlur( const RDG::PassContext& context, RDG::TextureRef source,
                                                        RDG::TextureRef target, const RDG::TextureDesc& desc,
                                                        uint32_t pass, const glm::vec2& sunScreenUv );

        static constexpr uint32_t GetBlurPassCount()
        {
            return kBlurPasses;
        }

        void SetParams( const Params& params )
        {
            m_Params = params;
        }
        [[nodiscard]] const Params& GetParams() const
        {
            return m_Params;
        }

    private:
        bool CreatePipelines();

        static constexpr uint32_t kBlurPasses = 3;

        std::shared_ptr<ComputePipeline> m_MaskPipeline;
        std::shared_ptr<ComputePipeline> m_BlurPipeline;

        Params m_Params;
    };
} // namespace Desert::Graphic::System
