#include "LightShaftRenderer.hpp"
#include <Engine/Graphic/ViewTargetFormats.hpp>

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
        const auto& target = m_TargetFramebuffer.lock();
        if ( !target )
            return Common::MakeError( "LightShaftRenderer: target framebuffer is not available" );

        if ( !CreateImages( target->GetFramebufferWidth(), target->GetFramebufferHeight() ) )
            return Common::MakeError( "LightShaftRenderer: failed to create shaft images" );

        if ( !CreatePipelines() )
            return Common::MakeError( "LightShaftRenderer: failed to create compute pipelines" );

        return BOOLSUCCESS;
    }

    bool LightShaftRenderer::CreateImages( uint32_t width, uint32_t height )
    {
        // Half resolution: shafts are low-frequency by construction and the blur reads this image
        // kTaps times per pixel per pass.
        const uint32_t sw = std::max( 1u, width / 2 );
        const uint32_t sh = std::max( 1u, height / 2 );

        const auto make = [&]( const char* tag ) -> std::shared_ptr<Image2D>
        {
            Core::Formats::Image2DSpecification spec = {
                 .Tag        = tag,
                 .Width      = sw,
                 .Height     = sh,
                 .Format     = kShaftFormat,
                 .Mips       = 1,
                 .Usage      = Core::Formats::Image2DUsage::Image2D,
                 .Properties = Core::Formats::Storage | Core::Formats::Sample,
                 // Sampled by the tonemap in frames this effect is off (intensity 0), so it must not be garbage.
                 .InitialContent = Core::Formats::ImageInitialContent::Zero,
            };
            return Image2D::Create( spec );
        };

        m_PingImage  = make( "LightShaftPing" );
        m_PongImage  = make( "LightShaftPong" );
        m_ShaftImage = GetBlurTarget( kBlurPasses - 1 ); // mask->ping, ping->pong, pong->ping, ping->pong

        return m_PingImage && m_PongImage;
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

    void LightShaftRenderer::Resize( uint32_t width, uint32_t height )
    {
        if ( width == 0 || height == 0 )
            return;
        CreateImages( width, height );
    }

    bool LightShaftRenderer::Prepare() const
    {
        return GetSceneColorImage() && m_PingImage && m_PongImage && m_MaskPipeline && m_BlurPipeline;
    }

    // When the effect contributes nothing this frame the dispatches are skipped entirely; the tonemap's shaft
    // INTENSITY is zero in exactly the same frames (SceneRenderer derives both from the same params), so the
    // stale image contents are multiplied away — the bloom image works the same way when bloom is off.
    void LightShaftRenderer::RecordMask( const glm::vec2& sunScreenUv, float screenFade )
    {
        if ( !IsActive( screenFade ) )
            return;
        const auto sceneColor = GetSceneColorImage();
        if ( !sceneColor )
            return;
        MaskPush maskPush{ sunScreenUv, m_Params.Threshold, m_Params.MaxBrightness, kMaskWindow };
        m_MaskPipeline->SetInput( 0, sceneColor.get(), RDG::Access::SampledCompute, RDG::SubresourceRange::All() );
        m_MaskPipeline->SetOutput( 1, m_PingImage.get() );
        m_MaskPipeline->SetPushConstants( &maskPush, sizeof( maskPush ) );
        Renderer::GetInstance().DispatchComputeInFrame( m_MaskPipeline.get(),
                                                        GroupCount( m_PingImage->GetWidth() ),
                                                        GroupCount( m_PingImage->GetHeight() ), 1 );
    }

    // Radial blur pass @p pass of the ping-pong; the reach grows kPassScale-fold per pass.
    void LightShaftRenderer::RecordBlur( uint32_t pass, const glm::vec2& sunScreenUv, float screenFade )
    {
        if ( !IsActive( screenFade ) || pass >= kBlurPasses )
            return;
        float reach = kBaseReach; // grown by repeated multiplication, as the single loop did
        for ( uint32_t i = 0; i < pass; ++i )
            reach *= kPassScale;
        BlurPush blurPush{ sunScreenUv, std::min( reach, 1.0f ), kBlurDecay };
        m_BlurPipeline->SetInput( 0, GetBlurSource( pass ).get(), RDG::Access::SampledCompute,
                                  RDG::SubresourceRange::All() );
        m_BlurPipeline->SetOutput( 1, GetBlurTarget( pass ).get() );
        m_BlurPipeline->SetPushConstants( &blurPush, sizeof( blurPush ) );
        Renderer::GetInstance().DispatchComputeInFrame( m_BlurPipeline.get(),
                                                        GroupCount( m_PingImage->GetWidth() ),
                                                        GroupCount( m_PingImage->GetHeight() ), 1 );
    }
} // namespace Desert::Graphic::System
