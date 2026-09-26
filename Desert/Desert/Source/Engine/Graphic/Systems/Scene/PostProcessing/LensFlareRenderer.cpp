#include "LensFlareRenderer.hpp"
#include <Engine/Graphic/ViewTargetFormats.hpp>

#include <Engine/Runtime/ResourceRegistry.hpp>

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>

namespace Desert::Graphic::System
{
    namespace
    {
        constexpr Core::Formats::ImageFormat kFlareFormat = ViewTargetFormats::kLensFlare;
        constexpr uint32_t                   kGroupSize   = 16; // must match LocalSize in both shaders

        // Push-constant blocks — must match LensFlareBrightPass / LensFlareFeatures exactly.
        struct BrightPassPush
        {
            int32_t SrcMip;
            int32_t FirstPass;
            float   Threshold;
            float   MaxBrightness;
        };

        struct FeaturesPush
        {
            glm::vec4 SunUvHalo;   // xy = sun uv, z = halo intensity, w = halo radius
            glm::vec4 GhostParams; // x = count, y = spacing, z = size near, w = size far
            glm::vec4 TintInner;   // xyz = inner tint, w = chromatic shift
            glm::vec4 TintOuter;   // xyz = outer tint, w = unused
            glm::vec4 Streak;      // x = intensity, y = length, z = axis.x, w = axis.y
        };

        inline uint32_t GroupCount( uint32_t dim )
        {
            return ( dim + kGroupSize - 1 ) / kGroupSize;
        }

        inline uint32_t MipSize( uint32_t base, uint32_t mip )
        {
            return std::max( 1u, base >> mip );
        }
    } // namespace

    Common::BoolResultStr LensFlareRenderer::Initialize()
    {
        const auto& target = m_TargetFramebuffer.lock();
        if ( !target )
            return Common::MakeError( "LensFlareRenderer: target framebuffer is not available" );

        if ( !CreateImages( target->GetFramebufferWidth(), target->GetFramebufferHeight() ) )
            return Common::MakeError( "LensFlareRenderer: failed to create flare images" );

        if ( !CreatePipelines() )
            return Common::MakeError( "LensFlareRenderer: failed to create compute pipelines" );

        return BOOLSUCCESS;
    }

    bool LensFlareRenderer::CreateImages( uint32_t width, uint32_t height )
    {
        const uint32_t sw = std::max( 1u, width / kSourceDivisor );
        const uint32_t sh = std::max( 1u, height / kSourceDivisor );
        const uint32_t fw = std::max( 1u, width / kFeatureDivisor );
        const uint32_t fh = std::max( 1u, height / kFeatureDivisor );

        // The source carries a chain because the ghosts MAGNIFY it (see the shader's SourceLodForScale);
        // the feature image is one level — the tonemap only ever reads it at full screen.
        m_SourceMipLevels = std::min( kMaxSourceMips, Utils::CalculateMipCount( sw, sh ) );

        const auto make = [&]( const char* tag, uint32_t w, uint32_t h, uint32_t mips ) -> std::shared_ptr<Image2D>
        {
            Core::Formats::Image2DSpecification spec = {
                 .Tag        = tag,
                 .Width      = w,
                 .Height     = h,
                 .Format     = kFlareFormat,
                 .Mips       = mips,
                 .Usage      = Core::Formats::Image2DUsage::Image2D,
                 .Properties = Core::Formats::Storage | Core::Formats::Sample,
                 // Sampled by the tonemap in frames this effect is off (intensity 0), so it must not be garbage.
                 .InitialContent = Core::Formats::ImageInitialContent::Zero,
            };
            return Image2D::Create( spec );
        };

        m_SourceImage = make( "LensFlareSource", sw, sh, m_SourceMipLevels );
        m_FlareImage  = make( "LensFlareFeatures", fw, fh, 1 );

        return m_SourceImage && m_FlareImage;
    }

    bool LensFlareRenderer::CreatePipelines()
    {
        const auto shaderService = Runtime::ResourceRegistry::GetShaderService();

        const auto make = [&]( const std::string& shaderName ) -> std::shared_ptr<ComputePipeline>
        {
            const auto shader = shaderService->GetByName( shaderName );
            if ( !shader )
            {
                LOG_ERROR( "LensFlareRenderer: missing compute shader '{}'", shaderName );
                return nullptr;
            }
            const auto built = ComputePipeline::Create( { .Shader = shader, .DebugName = shaderName } );
            if ( !built )
            {
                LOG_ERROR( "LensFlareRenderer: {}", built.GetError() );
                return nullptr;
            }
            return built.GetValue();
        };

        m_BrightPassPipeline = make( "LensFlareBrightPass" );
        m_FeaturesPipeline   = make( "LensFlareFeatures" );

        return m_BrightPassPipeline && m_FeaturesPipeline;
    }

    void LensFlareRenderer::Resize( uint32_t width, uint32_t height )
    {
        if ( width == 0 || height == 0 )
            return;
        CreateImages( width, height );
    }

    void LensFlareRenderer::Execute( const glm::vec2& sunScreenUv, float screenFade )
    {
        // Nothing to add this frame: the sun is behind the camera or off screen, or the effect is off.
        // The dispatches are skipped and the (stale) flare image stays inert because SceneRenderer
        // derives the tonemap's flare intensity from these same numbers — the contract bloom has.
        if ( !m_Params.Enabled || screenFade <= 0.0f || m_Params.Intensity <= 0.0f )
            return;

        const auto& scene = m_TargetFramebuffer.lock();
        if ( !scene || !m_SourceImage || !m_FlareImage || !m_BrightPassPipeline || !m_FeaturesPipeline )
            return;

        Image2D* sceneColor = scene->GetColorAttachmentImage().get();
        if ( !sceneColor )
            return;

        const uint32_t sw = m_SourceImage->GetWidth();
        const uint32_t sh = m_SourceImage->GetHeight();
        const uint32_t fw = m_FlareImage->GetWidth();
        const uint32_t fh = m_FlareImage->GetHeight();

        auto& renderer = Renderer::GetInstance();

        // --- Bright pass: scene HDR -> quarter-res thresholded source, then down the chain ---------
        // One shader run per mip, exactly as BloomRenderer does it; the threshold applies on the first
        // pass only, so the deeper levels are honest averages of the energy the first level admitted.
        renderer.ComputeImageBeginWrite( m_SourceImage.get() );

        for ( uint32_t mip = 0; mip < m_SourceMipLevels; ++mip )
        {
            const bool first = ( mip == 0 );

            BrightPassPush brightPush{ first ? 0 : static_cast<int32_t>( mip - 1 ), first ? 1 : 0,
                                       m_Params.Threshold, m_Params.MaxBrightness };

            m_BrightPassPipeline->SetInput( 0, first ? sceneColor : m_SourceImage.get() );
            m_BrightPassPipeline->SetOutput( 1, m_SourceImage.get(), mip );
            m_BrightPassPipeline->SetPushConstants( &brightPush, sizeof( brightPush ) );
            renderer.DispatchComputeInFrame( m_BrightPassPipeline.get(), GroupCount( MipSize( sw, mip ) ),
                                             GroupCount( MipSize( sh, mip ) ), 1 );
        }

        renderer.ComputeImageEndWrite( m_SourceImage.get() );

        // --- Features: source -> ghosts + halo + streak --------------------------------------------
        const float angle = glm::radians( m_Params.StreakAngle );

        FeaturesPush featuresPush{};
        featuresPush.SunUvHalo =
             glm::vec4( sunScreenUv.x, sunScreenUv.y, m_Params.HaloIntensity, m_Params.HaloRadius );
        featuresPush.GhostParams =
             glm::vec4( static_cast<float>( std::max( 0, m_Params.GhostCount ) ), m_Params.GhostSpacing,
                        m_Params.GhostSizeNear, m_Params.GhostSizeFar );
        featuresPush.TintInner = glm::vec4( m_Params.GhostTintInner, m_Params.ChromaShift );
        // The features pass needs to know the source is a REDUCED image to pick each ghost's mip: a
        // ghost authored at scale 3 is magnified 3 x kSourceDivisor on screen.
        featuresPush.TintOuter = glm::vec4( m_Params.GhostTintOuter, static_cast<float>( kSourceDivisor ) );
        featuresPush.Streak =
             glm::vec4( m_Params.StreakIntensity, m_Params.StreakLength, std::cos( angle ), std::sin( angle ) );

        renderer.ComputeImageBeginWrite( m_FlareImage.get() );

        m_FeaturesPipeline->SetInput( 0, m_SourceImage.get() );
        m_FeaturesPipeline->SetOutput( 1, m_FlareImage.get() );
        m_FeaturesPipeline->SetPushConstants( &featuresPush, sizeof( featuresPush ) );
        renderer.DispatchComputeInFrame( m_FeaturesPipeline.get(), GroupCount( fw ), GroupCount( fh ), 1 );

        renderer.ComputeImageEndWrite( m_FlareImage.get() );
    }
} // namespace Desert::Graphic::System
