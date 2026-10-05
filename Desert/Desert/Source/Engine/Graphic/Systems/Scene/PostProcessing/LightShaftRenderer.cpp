#include "LightShaftRenderer.hpp"
#include <Engine/Graphic/ViewTargetFormats.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>

#include <Engine/Runtime/ResourceRegistry.hpp>

#include <algorithm>

namespace Desert::Graphic::System
{
    namespace
    {
        constexpr Core::Formats::ImageFormat kShaftFormat = ViewTargetFormats::kLightShaft;
        constexpr uint32_t                   kGroupSize   = 16; // must match LocalSize in the shaders

        // Push-constant blocks — must match LightShaftMask/LightShaftBlur exactly.
        struct MaskPush
        {
            glm::vec2 SunUv;
            float     Threshold;
            float     MaxBrightness;
            float     WindowRadius;
        };

        struct BlurPush
        {
            glm::vec2 SunUv;
            float     Reach;
            float     Decay;
        };

        // How much of the pixel->sun distance each blur pass walks — UE's own schedule
        // (LightShaftRendering.cpp): first pass 0.1 of the way, each subsequent pass scaled by
        // 0.4 x NumSamples = 4.8, the last clamped so a tap never reads past the sun.
        constexpr float kTapCount   = 12.0f; // matches kTaps in LightShaftBlur and r.LightShaftNumSamples
        constexpr float kBlurDecay  = 0.93f;
        constexpr float kBaseReach  = 0.1f;             // r.LightShaftFirstPassDistance
        constexpr float kPassScale  = 0.4f * kTapCount; // UE's per-pass reach growth
        constexpr float kMaskWindow = 0.65f;            // UV radius around the sun the mask admits

        inline uint32_t GroupCount( uint32_t dim )
        {
            return ( dim + kGroupSize - 1 ) / kGroupSize;
        }
    } // namespace

    Common::BoolResultStr LightShaftRenderer::Initialize()
    {
        if ( !CreatePipelines() )
            return Common::MakeError( "LightShaftRenderer: failed to create compute pipelines" );
        return BOOLSUCCESS;
    }

    bool LightShaftRenderer::CreatePipelines()
    {
        const auto shaderService = Runtime::ResourceRegistry::GetShaderService();

        const auto make = [&]( const std::string& shaderName ) -> std::shared_ptr<ComputePipeline>
        {
            const auto shader = shaderService->GetByName( shaderName );
            if ( !shader )
            {
                LOG_ERROR( "LightShaftRenderer: missing compute shader '{}'", shaderName );
                return nullptr;
            }
            const auto built = ComputePipeline::Create( { .Shader = shader, .DebugName = shaderName } );
            if ( !built )
            {
                LOG_ERROR( "LightShaftRenderer: {}", built.GetError() );
                return nullptr;
            }
            return built.GetValue();
        };

        m_MaskPipeline = make( "LightShaftMask" );
        m_BlurPipeline = make( "LightShaftBlur" );

        return m_MaskPipeline && m_BlurPipeline;
    }

    std::optional<RDG::TextureDesc> LightShaftRenderer::GetTargetDesc() const
    {
        const auto scene = m_TargetFramebuffer.lock();
        if ( !scene || !scene->GetColorAttachmentImage() || !m_MaskPipeline || !m_BlurPipeline )
            return std::nullopt;
        // Half resolution: shafts are low-frequency by construction and the blur reads this image
        // kTaps times per pixel per pass.
        return RDG::TextureDesc{ .Size   = { .Width  = std::max( 1u, scene->GetFramebufferWidth() / 2 ),
                                             .Height = std::max( 1u, scene->GetFramebufferHeight() / 2 ) },
                                 .Format = kShaftFormat,
                                 .Mips   = 1 };
    }

    Common::BoolResultStr LightShaftRenderer::RecordMask( const RDG::PassContext& context,
                                                          RDG::TextureRef sceneColor, RDG::TextureRef mask,
                                                          const RDG::TextureDesc& desc,
                                                          const glm::vec2&        sunScreenUv )
    {
        const MaskPush    maskPush{ sunScreenUv, m_Params.Threshold, m_Params.MaxBrightness, kMaskWindow };
        RDG::PassBindings bindings( context );
        bindings
             .Sampled( "u_SceneColor", sceneColor, RDG::Access::SampledCompute, RDG::SubresourceRange::All(),
                       RDG::SamplerDesc::LinearClamp() )
             .Storage( "u_Mask", mask, RDG::Access::StorageWrite, 0 )
             .PushConstants( &maskPush, sizeof( maskPush ) );
        return Renderer::GetInstance().DispatchCompute( bindings, *m_MaskPipeline, GroupCount( desc.Size.Width ),
                                                        GroupCount( desc.Size.Height ), 1 );
    }

    // Radial blur pass @p pass of the ping-pong; the reach grows kPassScale-fold per pass.
    Common::BoolResultStr LightShaftRenderer::RecordBlur( const RDG::PassContext& context, RDG::TextureRef source,
                                                          RDG::TextureRef target, const RDG::TextureDesc& desc,
                                                          uint32_t pass, const glm::vec2& sunScreenUv )
    {
        if ( pass >= kBlurPasses )
            return Common::MakeError( "LightShaftRenderer: blur pass out of range" );
        float reach = kBaseReach; // grown by repeated multiplication, as the single loop did
        for ( uint32_t i = 0; i < pass; ++i )
            reach *= kPassScale;
        const BlurPush    blurPush{ sunScreenUv, std::min( reach, 1.0f ), kBlurDecay };
        RDG::PassBindings bindings( context );
        bindings
             .Sampled( "u_Source", source, RDG::Access::SampledCompute, RDG::SubresourceRange::All(),
                       RDG::SamplerDesc::LinearClamp() )
             .Storage( "u_Output", target, RDG::Access::StorageWrite, 0 )
             .PushConstants( &blurPush, sizeof( blurPush ) );
        return Renderer::GetInstance().DispatchCompute( bindings, *m_BlurPipeline, GroupCount( desc.Size.Width ),
                                                        GroupCount( desc.Size.Height ), 1 );
    }
} // namespace Desert::Graphic::System
