#pragma once

#include <Engine/Graphic/Systems/RenderSystem.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/Image.hpp>
#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/ShaderResources/StorageBuffer.hpp>

#include <array>
#include <memory>

namespace Desert::Graphic::System
{
    // Eye-adaptation via a COMPUTE log-luminance histogram (UE/Frostbite-style): each frame clears a
    // 256-bin histogram, builds it from the full HDR scene (atomic adds), then resolves it to an average
    // luminance with percentile clipping (rejects bright/dark outliers) and temporally adapts a 1x1
    // luminance image that tonemap turns into exposure. Replaces the old 8x8-grid fragment pass. Runs in
    // the explicit post-process chain (before tonemap), outside any render pass.
    class AutoExposureRenderer final : public RenderSystem
    {
    public:
        using RenderSystem::RenderSystem;

        virtual Common::BoolResultStr Initialize() override;

        void RegisterPasses( RenderGraphBuilder& /*builder*/ ) override
        {
        }

        void Execute();
        void Resize( uint32_t, uint32_t )
        {
        } // histogram + 1x1 buffers are viewport-independent

        // The adapted luminance IS a temporal history — see IRenderSystem::OnSceneReplaced, kind 1 — and
        // the one it now holds belongs to a world that is gone. Adapting out of it would be a ~1 s ramp
        // from the old scene's brightness, which is right when a player walks out of a cave and wrong when
        // a level is loaded. Handled as a one-frame snap rather than by writing the 1x1 image from the CPU:
        // the adaptation is `mix(prev, target, 1 - exp(-dt * speed))` on the GPU, so an effectively
        // infinite speed for one dispatch lands exactly on the new scene's own measured luminance.
        void OnSceneReplaced() override
        {
            m_SnapNextAdaptation = true;
        }

        // The world's step this frame (Core::WorldTime::GetDeltaSeconds): the eye adapts in game time, so
        // it holds on pause and follows dilation, as UE's eye adaptation does.
        void SetDeltaSeconds( float seconds )
        {
            m_DeltaSeconds = seconds;
        }

        void SetParams( float adaptSpeed, float minLuma, float maxLuma )
        {
            m_AdaptSpeed = adaptSpeed;
            m_MinLuma    = minLuma;
            m_MaxLuma    = maxLuma;
        }

        // The 1x1 image holding the latest adapted luminance (sampled by tonemap).
        const std::shared_ptr<Image2D>& GetAdaptedLuminanceImage() const
        {
            return m_LumImage[m_ReadIndex];
        }

    private:
        bool CreateResources();

        std::shared_ptr<ShaderResources::StorageBuffer> m_Histogram;
        std::array<std::shared_ptr<Image2D>, 2>         m_LumImage; // ping-pong 1x1

        std::shared_ptr<ComputePipeline> m_ClearPipeline;
        std::shared_ptr<ComputePipeline> m_HistogramPipeline;
        std::shared_ptr<ComputePipeline> m_AveragePipeline;

        int m_ReadIndex = 0; // holds the latest adapted luminance after Execute
        // Set by OnSceneReplaced, consumed and cleared by the next Execute — see that override.
        bool  m_SnapNextAdaptation = false;
        float m_AdaptSpeed         = 1.5f;
        float m_DeltaSeconds       = 0.0f;
        float m_MinLuma    = 0.02f;
        float m_MaxLuma    = 8.0f;
    };
} // namespace Desert::Graphic::System
