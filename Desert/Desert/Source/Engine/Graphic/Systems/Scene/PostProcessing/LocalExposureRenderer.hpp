#pragma once

#include <Engine/Graphic/Systems/RenderSystem.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/ShaderBindingLayoutCache.hpp>
#include <Engine/Graphic/PostProcessing/LocalExposureRules.hpp>

#include <array>
#include <memory>

namespace Desert::Graphic::System
{
    // The Setup shader's parameters: the one spelling the pass binds and Initialize checks.
    struct LocalExposureSetupShaderParameters
    {
        DESERT_SHADER_PARAMETER( Texture, SceneColorTexture );
        DESERT_SHADER_PARAMETER( Sampler, LinearClampSampler );
        DESERT_SHADER_PARAMETER( StorageTexture, OutBilateralGrid );
        DESERT_SHADER_PARAMETER( StorageTexture, OutLogLuminance );
        static constexpr std::array kAll{ RDG::Erase( SceneColorTexture ), RDG::Erase( LinearClampSampler ),
                                          RDG::Erase( OutBilateralGrid ), RDG::Erase( OutLogLuminance ) };
    };

    // The Blur shader's parameters (texelFetch with mirrored integer coordinates: the sampler is never filtered).
    struct LocalExposureBlurShaderParameters
    {
        DESERT_SHADER_PARAMETER( Texture, InputTexture );
        DESERT_SHADER_PARAMETER( Sampler, PointClampSampler );
        DESERT_SHADER_PARAMETER( StorageTexture, OutputTexture );
        static constexpr std::array kAll{ RDG::Erase( InputTexture ), RDG::Erase( PointClampSampler ),
                                          RDG::Erase( OutputTexture ) };
    };

    // UE's Local Exposure, Bilateral method (Graphic/PostProcessing/LocalExposureRules.hpp): compute nodes of the
    // frame graph after the auto exposure (SceneRendererFramePostFX.cpp "PostFX: LocalExposure*"). Setup writes
    // the bilateral grid (sum log luminance, count per 128 px cell x 32 one-EV slices) and the 1/32 log luminance;
    // BlurH/BlurV blur the latter. The tonemap and the bloom's first downsample read both. Nothing persists: the
    // grid and the blur are graph transients rebuilt from each frame's colour, as in UE.
    class LocalExposureRenderer final : public RenderSystem
    {
    public:
        using RenderSystem::RenderSystem;

        Common::BoolResultStr Initialize() override;

        // The grid: 3D, ceil(w/128) x ceil(h/128) x 32, (sum logLum, count). UE uses PF_G32R32F; the engine has
        // no two-channel 32-bit float format, so RGBA32F (69 KiB at 1080p).
        [[nodiscard]] static RDG::TextureDesc GetGridDesc( uint32_t width, uint32_t height );
        // The log luminance at 1/32 (UE PF_R16F; R32F here, the engine's one single-channel float format).
        [[nodiscard]] static RDG::TextureDesc GetLogLumDesc( uint32_t width, uint32_t height );

        // This frame's resolved values: the slice window is built from @p lastExposure (ViewInputs::LastExposure,
        // the CPU exposure ScenePreExposure also uses) — the setup and every reader see the same number.
        void SetParams( const LocalExposureParams& params, float logMiddleGrey, float lastExposure )
        {
            m_Params        = params;
            m_LogMiddleGrey = logMiddleGrey;
            m_SliceLogMin   = LocalExposureSliceLogMin( logMiddleGrey, lastExposure );
        }
        // This frame's ViewFrame::PreExposure (> 0): the scene colour the setup reads is stored multiplied by it.
        void SetPreExposure( float preExposure )
        {
            m_PreExposure = preExposure;
        }
        [[nodiscard]] const LocalExposureParams& GetParams() const
        {
            return m_Params;
        }
        [[nodiscard]] float GetLogMiddleGrey() const
        {
            return m_LogMiddleGrey;
        }
        [[nodiscard]] float GetSliceLogMin() const
        {
            return m_SliceLogMin;
        }
        [[nodiscard]] bool IsReady() const
        {
            return m_SetupPipeline && m_BlurPipeline;
        }

        void DeclareSetupBindings( RDG::PassBuilder& pass, RDG::TextureRef scene, RDG::TextureRef grid,
                                   RDG::TextureRef logLum ) const;
        [[nodiscard]] Common::BoolResultStr RecordSetup( const RDG::PassContext& context, uint32_t width,
                                                         uint32_t height );
        void DeclareBlurBindings( RDG::PassBuilder& pass, RDG::TextureRef input, RDG::TextureRef output ) const;
        [[nodiscard]] Common::BoolResultStr RecordBlur( const RDG::PassContext& context, bool horizontal,
                                                        float radius, uint32_t blurWidth, uint32_t blurHeight );

        void Resize( uint32_t, uint32_t )
        {
        } // everything is a graph transient

    private:
        std::shared_ptr<ComputePipeline> m_SetupPipeline;
        std::shared_ptr<ComputePipeline> m_BlurPipeline;
        mutable ShaderBindingLayoutCache m_SetupLayout;
        mutable ShaderBindingLayoutCache m_BlurLayout;

        LocalExposureParams m_Params;
        float               m_LogMiddleGrey = -2.4739312f; // log2(0.18)
        float               m_SliceLogMin   = -2.4739312f - kLocalExposureSliceHalfWindowEV;
        float               m_PreExposure   = 1.0f;
    };
} // namespace Desert::Graphic::System
