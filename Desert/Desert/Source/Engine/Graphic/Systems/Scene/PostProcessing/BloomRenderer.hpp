#pragma once

#include <Engine/Graphic/Systems/RenderSystem.hpp>

#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/Image.hpp>
#include <Engine/Graphic/Pipeline.hpp>

#include <memory>

namespace Desert::Graphic::System
{
    // Bloom via a compute mip-chain (Call of Duty / Jimenez): progressive 13-tap downsample of the HDR
    // scene color (with a Karis average + bright-pass on the first mip), then a tent-filtered additive
    // upsample back to mip 0. The mip-0 result (GetBloomImage()) is added in during tonemapping. Every
    // dispatch is its own compute node of the frame graph (SceneRendererFramePostFX.cpp "PostFX: Bloom"),
    // which declares the mip it samples and the mip it writes and places every barrier between them.
    class BloomRenderer final : public RenderSystem
    {
    public:
        using RenderSystem::RenderSystem;

        virtual Common::BoolResultStr Initialize() override;

        void RegisterPasses( RenderGraphBuilder& /*builder*/ ) override
        {
        }

        // False: nothing to record this frame (no scene colour, chain or pipelines).
        bool Prepare() const;
        // Downsample into @p mip: mip 0 samples the scene colour (Karis + threshold), mip i samples mip i-1.
        void RecordDownsample( uint32_t mip );
        // Additive upsample: samples @p mip (>= 1) and accumulates into mip - 1 (read-modify-write).
        void     RecordUpsample( uint32_t mip );
        uint32_t GetMipLevels() const
        {
            return m_MipLevels;
        }
        void Resize( uint32_t width, uint32_t height );

        void SetThreshold( float threshold )
        {
            m_Threshold = threshold;
        }

        // The mip-0 bloom result, sampled by the tonemap pass. Recreated on resize.
        const std::shared_ptr<Image2D>& GetBloomImage() const
        {
            return m_BloomImage;
        }

    private:
        bool CreateImage( uint32_t width, uint32_t height );
        bool CreatePipelines();

        // Half-resolution chain; capped so the smallest mip stays a sane size.
        static constexpr uint32_t kMaxBloomMips = 6;
        static constexpr float    kFilterRadius = 1.0f; // tent radius (source texels) for upsampling

        std::shared_ptr<Image2D>          m_BloomImage;
        std::shared_ptr<ComputePipeline>  m_DownsamplePipeline;
        std::shared_ptr<ComputePipeline>  m_UpsamplePipeline;

        uint32_t m_MipLevels = 1;
        float    m_Threshold = 1.0f;
    };
} // namespace Desert::Graphic::System
