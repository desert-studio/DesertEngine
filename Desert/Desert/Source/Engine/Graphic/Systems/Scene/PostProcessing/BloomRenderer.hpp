#pragma once

#include <Engine/Graphic/Systems/RenderSystem.hpp>

#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/Image.hpp>
#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/ShaderBindingLayoutCache.hpp>

#include <memory>
#include <optional>

namespace Desert::Graphic::System
{
    // Bloom via a compute mip-chain (Call of Duty / Jimenez): progressive 13-tap downsample of the HDR
    // scene color (with a Karis average + bright-pass on the first mip), then a tent-filtered additive
    // upsample back to mip 0. The chain is a transient of each frame's graph (FrameTextures::Transients.Bloom),
    // whose mip 0 the tonemap adds in. Every dispatch is its own compute node (SceneRendererFramePostFX.cpp
    // "PostFX: Bloom*"), which declares the mip it samples and the mip it writes and places every barrier.
    class BloomRenderer final : public RenderSystem
    {
    public:
        using RenderSystem::RenderSystem;

        virtual Common::BoolResultStr Initialize() override;

        void RegisterPasses( RenderGraphBuilder& /*builder*/ ) override
        {
        }

        // The chain this frame needs, from this frame's scene colour size: half resolution, mips capped so the
        // smallest stays a sane size. Nullopt when there is nothing to record (no scene colour or pipelines).
        // The texture is a transient of the frame graph (Builder::CreateTexture); the renderer keeps no image.
        std::optional<RDG::TextureDesc> GetChainDesc() const;
        // Downsample into @p mip of @p chain: mip 0 samples @p sceneColor (Karis + threshold), mip i samples
        // mip i-1, both LinearClamp. SETUP declares that block (block 0); the EXEC dispatches from it.
        void DeclareDownsampleBindings( RDG::PassBuilder& pass, RDG::TextureRef sceneColor, RDG::TextureRef chain,
                                        uint32_t mip ) const;
        [[nodiscard]] Common::BoolResultStr RecordDownsample( const RDG::PassContext& context,
                                                              const RDG::TextureDesc& chainDesc, uint32_t mip );
        // Additive upsample: samples @p mip (>= 1) of @p chain (LinearClamp) and accumulates into mip - 1.
        void DeclareUpsampleBindings( RDG::PassBuilder& pass, RDG::TextureRef chain, uint32_t mip ) const;
        [[nodiscard]] Common::BoolResultStr RecordUpsample( const RDG::PassContext& context,
                                                            const RDG::TextureDesc& chainDesc, uint32_t mip );

        void SetThreshold( float threshold )
        {
            m_Threshold = threshold;
        }

    private:
        bool CreatePipelines();

        // Half-resolution chain; capped so the smallest mip stays a sane size.
        static constexpr uint32_t kMaxBloomMips = 6;
        static constexpr float    kFilterRadius = 1.0f; // tent radius (source texels) for upsampling

        std::shared_ptr<ComputePipeline>  m_DownsamplePipeline;
        std::shared_ptr<ComputePipeline>  m_UpsamplePipeline;
        mutable ShaderBindingLayoutCache  m_DownsampleLayout; // the two shaders' layouts, kept between frames
        mutable ShaderBindingLayoutCache  m_UpsampleLayout;

        float m_Threshold = 1.0f;
    };
} // namespace Desert::Graphic::System
