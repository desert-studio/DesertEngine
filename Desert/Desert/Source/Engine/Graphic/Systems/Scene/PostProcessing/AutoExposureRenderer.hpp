#pragma once

#include <Engine/Graphic/Systems/RenderSystem.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/Image.hpp>
#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>

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

        // The histogram's bins: one uint per bin, 256 bins (AEHistogram*.shader `uint u_Bins[256]`).
        static constexpr uint32_t kBins           = 256;
        static constexpr uint64_t kHistogramBytes = kBins * sizeof( uint32_t );
        // The histogram is a transient of each frame graph (Builder::CreateBuffer with this desc): it is written
        // and read within one frame (Clear -> Histogram -> Average) and nothing reads it the next frame.
        static RDG::BufferDesc GetHistogramDesc()
        {
            return RDG::BufferDesc{ .Bytes = kHistogramBytes };
        }

        // Three compute nodes of the frame graph (SceneRendererFramePostFX.cpp "PostFX: AutoExposure*"). Prepare
        // runs when the graph is built: it advances the 1x1 ping-pong, so GetAdaptedLuminanceImage() is the image
        // this frame's RecordAverage writes and GetPreviousLuminanceImage() the one it adapts from. False:
        // nothing to record (no scene colour or pipelines), and the ping-pong does not move.
        bool Prepare();
        // 1) Zero the histogram (the node declares Write(histogram, StorageWrite)); binds "Histogram" by name.
        [[nodiscard]] Common::BoolResultStr RecordClear( const RDG::PassContext& context, RDG::BufferRef histogram );
        // 2) Histogram of @p scene (texelFetch, so PointClamp), atomic adds into @p histogram. @p width x @p height
        //    is the scene's size this frame (one thread per texel).
        [[nodiscard]] Common::BoolResultStr RecordHistogram( const RDG::PassContext& context, RDG::TextureRef scene,
                                                             RDG::BufferRef histogram, uint32_t width,
                                                             uint32_t height );
        // 3) Percentile-clipped average + temporal adaptation: reads @p histogram, samples @p previous (the
        //    imported GetPreviousLuminanceImage()), writes @p adapted (the imported GetAdaptedLuminanceImage()).
        [[nodiscard]] Common::BoolResultStr RecordAverage( const RDG::PassContext& context, RDG::BufferRef histogram,
                                                           RDG::TextureRef previous, RDG::TextureRef adapted );

        std::shared_ptr<Image2D> GetSceneColorImage() const
        {
            const auto scene = m_TargetFramebuffer.lock();
            return scene ? scene->GetColorAttachmentImage() : nullptr;
        }
        const std::shared_ptr<Image2D>& GetPreviousLuminanceImage() const
        {
            return m_LumImage[1 - m_ReadIndex];
        }
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

        // Camera cut — see IRenderSystem::OnTemporalHistoryReset. The same one-dispatch snap: the first
        // frame after the cut is exposed for what it shows, not ramped from what the frames before it did.
        void OnTemporalHistoryReset() override
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

        // The 1x1 image holding the latest adapted luminance (sampled by tonemap); after Prepare, the one this
        // frame writes.
        const std::shared_ptr<Image2D>& GetAdaptedLuminanceImage() const
        {
            return m_LumImage[m_ReadIndex];
        }

    private:
        bool CreateResources();

        // History (read by the next frame's Average and by the tonemap): stays external, imported every frame.
        std::array<std::shared_ptr<Image2D>, 2> m_LumImage; // ping-pong 1x1

        std::shared_ptr<ComputePipeline> m_ClearPipeline;
        std::shared_ptr<ComputePipeline> m_HistogramPipeline;
        std::shared_ptr<ComputePipeline> m_AveragePipeline;

        int m_ReadIndex = 0; // holds the latest adapted luminance; Prepare points it at this frame's write
        // Set by OnSceneReplaced, consumed and cleared by the next RecordAverage — see that override.
        bool  m_SnapNextAdaptation = false;
        float m_AdaptSpeed         = 1.5f;
        float m_DeltaSeconds       = 0.0f;
        float m_MinLuma    = 0.02f;
        float m_MaxLuma    = 8.0f;
    };
} // namespace Desert::Graphic::System
