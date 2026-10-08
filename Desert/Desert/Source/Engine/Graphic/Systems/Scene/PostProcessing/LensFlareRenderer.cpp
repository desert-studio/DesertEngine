#include "LensFlareRenderer.hpp"
#include <Engine/Graphic/ViewTargetFormats.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>

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
        if ( !CreatePipelines() )
            return Common::MakeError( "LensFlareRenderer: failed to create compute pipelines" );
        return BOOLSUCCESS;
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

    std::optional<RDG::TextureDesc> LensFlareRenderer::GetSourceDesc() const
    {
        const auto scene = m_TargetFramebuffer.lock();
        if ( !scene || !scene->GetColorAttachmentImage() )
            return std::nullopt;
        const uint32_t sw = std::max( 1u, scene->GetFramebufferWidth() / kSourceDivisor );
        const uint32_t sh = std::max( 1u, scene->GetFramebufferHeight() / kSourceDivisor );
        // The source carries a chain because the ghosts MAGNIFY it (see the shader's SourceLodForScale).
        return RDG::TextureDesc{ .Size   = { .Width = sw, .Height = sh },
                                 .Format = kFlareFormat,
                                 .Mips   = std::min( kMaxSourceMips, Utils::CalculateMipCount( sw, sh ) ) };
    }

    std::optional<RDG::TextureDesc> LensFlareRenderer::GetFlareDesc() const
    {
        const auto scene = m_TargetFramebuffer.lock();
        if ( !scene || !scene->GetColorAttachmentImage() )
            return std::nullopt;
        // One level: the tonemap only ever reads it at full screen.
        const uint32_t fw = std::max( 1u, scene->GetFramebufferWidth() / kFeatureDivisor );
        const uint32_t fh = std::max( 1u, scene->GetFramebufferHeight() / kFeatureDivisor );
        return RDG::TextureDesc{ .Size = { .Width = fw, .Height = fh }, .Format = kFlareFormat, .Mips = 1 };
    }

    bool LensFlareRenderer::Prepare( float screenFade ) const
    {
        // Nothing to add this frame: the sun is behind the camera or off screen, or the effect is off.
        if ( !m_Params.Enabled || screenFade <= 0.0f || m_Params.Intensity <= 0.0f )
            return false;
        return m_BrightPassPipeline && m_FeaturesPipeline;
    }

    void LensFlareRenderer::DeclareBrightPassBindings( RDG::PassBuilder& pass, RDG::TextureRef sceneColor,
                                                       RDG::TextureRef source, uint32_t mip ) const
    {
        if ( !m_BrightPassPipeline )
            return; // RecordBrightPass refuses by name
        auto block = pass.Bindings( m_BrightPassLayout.Get( m_BrightPassPipeline->GetSpecification().Shader ),
                                    Renderer::GetPipelineRouteFill( *m_BrightPassPipeline ) );
        if ( mip == 0 )
        {
            block.Sampled( "u_Source", sceneColor, RDG::Access::SampledCompute, RDG::SubresourceRange::All(),
                           RDG::SamplerDesc::LinearClamp() );
        }
        else
        {
            block.Sampled( "u_Source", source, RDG::Access::SampledCompute, RDG::SubresourceRange::Mip( mip - 1 ),
                           RDG::SamplerDesc::LinearClamp() );
        }
        block.Storage( "u_Output", source, RDG::Access::StorageWrite, mip )
             .PushConstantBytes( static_cast<uint32_t>( sizeof( BrightPassPush ) ) );
    }

    Common::BoolResultStr LensFlareRenderer::RecordBrightPass( const RDG::PassContext& context,
                                                               const RDG::TextureDesc& sourceDesc, uint32_t mip )
    {
        if ( !m_BrightPassPipeline )
            return Common::MakeError( "LensFlareRenderer: the bright-pass pipeline is not initialised" );
        // One shader run per mip, exactly as BloomRenderer does it; the threshold applies on the first
        // pass only, so the deeper levels are honest averages of the energy the first level admitted.
        const bool           first = ( mip == 0 );
        const BrightPassPush brightPush{ first ? 1 : 0, m_Params.Threshold, m_Params.MaxBrightness };
        RDG::PassBindings    bindings( context, context.GetBindingBlock( 0 ) );
        bindings.PushConstants( &brightPush, sizeof( brightPush ) );
        return Renderer::GetInstance().DispatchCompute( bindings, *m_BrightPassPipeline,
                                                        GroupCount( MipSize( sourceDesc.Size.Width, mip ) ),
                                                        GroupCount( MipSize( sourceDesc.Size.Height, mip ) ), 1 );
    }

    void LensFlareRenderer::DeclareFeaturesBindings( RDG::PassBuilder& pass, RDG::TextureRef source,
                                                     RDG::TextureRef flare ) const
    {
        if ( !m_FeaturesPipeline )
            return; // RecordFeatures refuses by name
        pass.Bindings( m_FeaturesLayout.Get( m_FeaturesPipeline->GetSpecification().Shader ),
                       Renderer::GetPipelineRouteFill( *m_FeaturesPipeline ) )
             .Sampled( "u_FlareSource", source, RDG::Access::SampledCompute, RDG::SubresourceRange::All(),
                       RDG::SamplerDesc::LinearClamp() )
             .Storage( "u_Flare", flare, RDG::Access::StorageWrite, 0 )
             .PushConstantBytes( static_cast<uint32_t>( sizeof( FeaturesPush ) ) );
    }

    Common::BoolResultStr LensFlareRenderer::RecordFeatures( const RDG::PassContext& context,
                                                             const RDG::TextureDesc& flareDesc,
                                                             const glm::vec2&        sunScreenUv )
    {
        if ( !m_FeaturesPipeline )
            return Common::MakeError( "LensFlareRenderer: the features pipeline is not initialised" );
        // Source -> ghosts + halo + streak.
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

        RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
        bindings.PushConstants( &featuresPush, sizeof( featuresPush ) );
        return Renderer::GetInstance().DispatchCompute( bindings, *m_FeaturesPipeline,
                                                        GroupCount( flareDesc.Size.Width ),
                                                        GroupCount( flareDesc.Size.Height ), 1 );
    }
} // namespace Desert::Graphic::System
