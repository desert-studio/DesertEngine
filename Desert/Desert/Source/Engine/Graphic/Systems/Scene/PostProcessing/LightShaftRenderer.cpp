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
            };
            return Image2D::Create( spec );
        };

        m_PingImage  = make( "LightShaftPing" );
        m_PongImage  = make( "LightShaftPong" );
        m_ShaftImage = m_PongImage; // 3 passes: mask->ping, ping->pong, pong->ping, ping->pong

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

    void LightShaftRenderer::Execute( const glm::vec2& sunScreenUv, float screenFade )
    {
        // When the effect contributes nothing this frame the dispatches are skipped entirely; the
        // tonemap's shaft INTENSITY is zero in exactly the same frames (SceneRenderer derives both from
        // the same params), so the stale image contents are multiplied away — the bloom image works the
        // same way when bloom is off.
        if ( !m_Params.Enabled || screenFade <= 0.0f || m_Params.BloomScale <= 0.0f )
            return;

        const auto& scene = m_TargetFramebuffer.lock();
        if ( !scene || !m_PingImage || !m_PongImage || !m_MaskPipeline || !m_BlurPipeline )
            return;

        Image2D* sceneColor = scene->GetColorAttachmentImage().get();
        if ( !sceneColor )
            return;

        const uint32_t sw = m_PingImage->GetWidth();
        const uint32_t sh = m_PingImage->GetHeight();

        auto& renderer = Renderer::GetInstance();

        // --- Mask: scene HDR -> ping -------------------------------------------------------------
        renderer.ComputeImageBeginWrite( m_PingImage.get() );

        MaskPush maskPush{ sunScreenUv, m_Params.Threshold, m_Params.MaxBrightness, kMaskWindow };
        m_MaskPipeline->SetInput( 0, sceneColor );
        m_MaskPipeline->SetOutput( 1, m_PingImage.get() );
        m_MaskPipeline->SetPushConstants( &maskPush, sizeof( maskPush ) );
        renderer.DispatchComputeInFrame( m_MaskPipeline.get(), GroupCount( sw ), GroupCount( sh ), 1 );

        renderer.ComputeImageEndWrite( m_PingImage.get() );

        // --- Radial blur, ping-pong, reach growing kTaps-fold per pass ---------------------------
        Image2D* src = m_PingImage.get();
        Image2D* dst = m_PongImage.get();

        float reach = kBaseReach;
        for ( uint32_t pass = 0; pass < kBlurPasses; ++pass )
        {
            renderer.ComputeImageBeginWrite( dst );

            BlurPush blurPush{ sunScreenUv, std::min( reach, 1.0f ), kBlurDecay };
            m_BlurPipeline->SetInput( 0, src );
            m_BlurPipeline->SetOutput( 1, dst );
            m_BlurPipeline->SetPushConstants( &blurPush, sizeof( blurPush ) );
            renderer.DispatchComputeInFrame( m_BlurPipeline.get(), GroupCount( sw ), GroupCount( sh ), 1 );

            renderer.ComputeImageEndWrite( dst );

            std::swap( src, dst );
            reach *= kPassScale;
        }

        // After an odd/even dance, `src` holds the last output. Three passes: mask->ping, ping->pong,
        // pong->ping, ping->pong — the loop swapped after the last write, so src points at it.
        m_ShaftImage = ( src == m_PingImage.get() ) ? m_PingImage : m_PongImage;
    }
} // namespace Desert::Graphic::System
