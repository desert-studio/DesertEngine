#pragma once

#include <Engine/Graphic/Systems/RenderSystem.hpp>

#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/Image.hpp>
#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/ShaderBindingLayoutCache.hpp>
#include <Engine/Graphic/Systems/Scene/PostProcessing/TonemapRenderer.hpp>

#include <algorithm>
#include <memory>
#include <optional>

namespace Desert::Graphic::System
{
    // Bloom via a compute mip-chain (Call of Duty / Jimenez): progressive 13-tap downsample of the HDR
    // scene color (with a Karis average + bright-pass on the first mip), then a tent-filtered additive
    // upsample back to mip 0. The chain is a transient of each frame's graph (FrameTextures::Transients.Bloom),
    // whose mip 0 the tonemap adds in. Every dispatch is its own compute node (SceneRendererFramePostFX.cpp
    // "PostFX: Bloom*"), which declares the mip it samples and the mip it writes and places every barrier.
    // The Downsample shader's parameters: the one spelling the pass binds and Initialize checks.
    struct BloomDownsampleShaderParameters
    {
        DESERT_SHADER_PARAMETER( Texture, SourceMipTexture );
        DESERT_SHADER_PARAMETER( Sampler, LinearClampSampler );
        DESERT_SHADER_PARAMETER( Sampler, PointClampSampler );
        DESERT_SHADER_PARAMETER( Texture, AdaptedLuminanceTexture );
        DESERT_SHADER_PARAMETER( StorageTexture, DestinationMipTexture );
        DESERT_SHADER_PARAMETER( Texture, LumBilateralGrid );
        DESERT_SHADER_PARAMETER( Texture, BlurredLogLuminance );
        static constexpr std::array kAll{
             RDG::Erase( SourceMipTexture ),      RDG::Erase( LinearClampSampler ),
             RDG::Erase( PointClampSampler ),     RDG::Erase( AdaptedLuminanceTexture ),
             RDG::Erase( DestinationMipTexture ), RDG::Erase( LumBilateralGrid ),
             RDG::Erase( BlurredLogLuminance ) };
    };

    // The Upsample shader's parameters: the one spelling the pass binds and Initialize checks.
    struct BloomUpsampleShaderParameters
    {
        DESERT_SHADER_PARAMETER( Texture, SourceMipTexture );
        DESERT_SHADER_PARAMETER( Sampler, LinearClampSampler );
        DESERT_SHADER_PARAMETER( StorageTexture, DestinationMipTexture );
        static constexpr std::array kAll{ RDG::Erase( SourceMipTexture ), RDG::Erase( LinearClampSampler ), RDG::Erase( DestinationMipTexture ) };
    };

    class BloomRenderer final : public RenderSystem
    {
    public:
        using RenderSystem::RenderSystem;

        virtual Common::BoolResultStr Initialize() override;

        // The chain this frame needs, from this frame's scene colour size: half resolution, mips capped so the
        // smallest stays a sane size. Nullopt when there is nothing to record (no scene colour or pipelines).
        // The texture is a transient of the frame graph (Builder::CreateTexture); the renderer keeps no image.
        [[nodiscard]] std::optional<RDG::TextureDesc> GetChainDesc() const;
        // Downsample into @p mip of @p chain: mip 0 samples @p sceneColor (Karis + threshold), mip i samples
        // mip i-1, both LinearClamp. SETUP declares that block (block 0); the EXEC dispatches from it.
        // @p adaptedLuminance is this frame's auto-exposure adapted luminance (1x1), or the system white when
        // there is none: with auto-exposure on, the threshold is stated after exposure, as it is in manual.
        // @p localExposureGrid / @p blurredLogLuminance: FrameTransients' when the local exposure ran, else
        // System.Black3D / System.Black (SetLocalExposure's frame then says not produced).
        void DeclareDownsampleBindings( RDG::PassBuilder& pass, RDG::TextureRef sceneColor, RDG::TextureRef chain,
                                        RDG::TextureRef adaptedLuminance, RDG::TextureRef localExposureGrid,
                                        RDG::TextureRef blurredLogLuminance, uint32_t mip ) const;
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
        // The scale the scene colour is stored at (ViewFrame::PreExposure): the bright pass compares the STORED
        // scene, so the threshold (raw radiance, or exposed units under auto-exposure) is multiplied by it.
        void SetPreExposure( float preExposure )
        {
            m_PreExposure = preExposure;
        }
        // Auto-exposure on: the threshold is the authored one (exposed units) and the shader divides it by
        // this frame's exposure, key / adapted luminance — the composite's own formula.
        void SetAutoExposure( bool enabled, float key )
        {
            m_AutoExposure = enabled;
            m_ExposureKey  = key;
        }
        // The chain's mip count cap, PostProcess.BloomMips (Scalability; High = 6, the former constant). Read per
        // frame by GetChainDesc, so a change applies on the next frame's graph.
        // This frame's local exposure (UE r.Bloom.ApplyLocalExposure): mip 0 multiplies each tap by it, with the
        // manual @p exposure the composite uses when auto-exposure is off.
        void SetLocalExposure( const TonemapRenderer::LocalExposureFrame& frame, float exposure )
        {
            m_LocalExposure  = frame;
            m_ManualExposure = exposure;
        }

        void SetMaxMips( uint32_t mips )
        {
            m_MaxMips = std::max( 1u, mips );
        }

    private:
        bool CreatePipelines();

        static constexpr float    kFilterRadius = 1.0f; // tent radius (source texels) for upsampling

        std::shared_ptr<ComputePipeline>  m_DownsamplePipeline;
        std::shared_ptr<ComputePipeline>  m_UpsamplePipeline;
        mutable ShaderBindingLayoutCache  m_DownsampleLayout; // the two shaders' layouts, kept between frames
        mutable ShaderBindingLayoutCache  m_UpsampleLayout;

        float    m_Threshold = 1.0f;
        float    m_PreExposure  = 1.0f;
        bool     m_AutoExposure = false;
        float    m_ExposureKey  = 0.18f;
        TonemapRenderer::LocalExposureFrame m_LocalExposure;
        float                               m_ManualExposure = 1.0f;
        uint32_t m_MaxMips   = 6; // until the first SetMaxMips: the High table value
    };
} // namespace Desert::Graphic::System
