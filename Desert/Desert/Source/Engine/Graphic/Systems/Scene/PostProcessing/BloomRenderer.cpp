#include "BloomRenderer.hpp"
#include <Engine/Graphic/ViewTargetFormats.hpp>

#include <Engine/Runtime/ResourceRegistry.hpp>

#include <glm/glm.hpp>
#include <algorithm>

namespace Desert::Graphic::System
{
    namespace
    {
        constexpr Core::Formats::ImageFormat kBloomFormat = ViewTargetFormats::kBloom;
        constexpr uint32_t                   kGroupSize   = 16; // must match local_size_* in the shaders

        // Push-constant blocks — must match BloomDownsample/BloomUpsample.glsl.comp exactly.
        struct DownsamplePush
        {
            glm::vec2 SrcTexelSize;
            int32_t   SrcMip;
            int32_t   FirstPass;
            float     Threshold;
        };

        struct UpsamplePush
        {
            glm::vec2 SrcTexelSize;
            int32_t   SrcMip;
            float     FilterRadius;
        };

        inline uint32_t MipSize( uint32_t base, uint32_t mip )
        {
            return std::max( 1u, base >> mip );
        }

        inline uint32_t GroupCount( uint32_t dim )
        {
            return ( dim + kGroupSize - 1 ) / kGroupSize;
        }
    } // namespace

    Common::BoolResultStr BloomRenderer::Initialize()
    {
        const auto& target = m_TargetFramebuffer.lock();
        if ( !target )
            return Common::MakeError( "BloomRenderer: target framebuffer is not available" );

        if ( !CreateImage( target->GetFramebufferWidth(), target->GetFramebufferHeight() ) )
            return Common::MakeError( "BloomRenderer: failed to create bloom image" );

        if ( !CreatePipelines() )
            return Common::MakeError( "BloomRenderer: failed to create compute pipelines" );

        return BOOLSUCCESS;
    }

    bool BloomRenderer::CreateImage( uint32_t width, uint32_t height )
    {
        // Half-resolution chain (mip 0 = scene / 2), capped so the smallest mip stays usable.
        const uint32_t bw = std::max( 1u, width / 2 );
        const uint32_t bh = std::max( 1u, height / 2 );
        m_MipLevels       = std::min( kMaxBloomMips, Utils::CalculateMipCount( bw, bh ) );

        Core::Formats::Image2DSpecification spec = {
             .Tag        = "BloomChain",
             .Width      = bw,
             .Height     = bh,
             .Format     = kBloomFormat,
             .Mips       = m_MipLevels,
             .Usage      = Core::Formats::Image2DUsage::Image2D,
             .Properties = Core::Formats::Storage | Core::Formats::Sample,
             // Sampled by the tonemap in frames this effect is off (intensity 0), so it must not be garbage.
             .InitialContent = Core::Formats::ImageInitialContent::Zero,
        };

        m_BloomImage = Image2D::Create( spec );
        return m_BloomImage != nullptr;
    }

    bool BloomRenderer::CreatePipelines()
    {
        const auto shaderService = Runtime::ResourceRegistry::GetShaderService();

        const auto make = [&]( const std::string& shaderName ) -> std::shared_ptr<ComputePipeline>
        {
            const auto shader = shaderService->GetByName( shaderName );
            if ( !shader )
            {
                LOG_ERROR( "BloomRenderer: missing compute shader '{}'", shaderName );
                return nullptr;
            }
            const auto built = ComputePipeline::Create( { .Shader = shader, .DebugName = shaderName } );
            if ( !built )
            {
                LOG_ERROR( "BloomRenderer: {}", built.GetError() );
                return nullptr;
            }
            return built.GetValue();
        };

        m_DownsamplePipeline = make( "BloomDownsample" );
        m_UpsamplePipeline   = make( "BloomUpsample" );

        return m_DownsamplePipeline && m_UpsamplePipeline;
    }

    void BloomRenderer::Resize( uint32_t width, uint32_t height )
    {
        if ( width == 0 || height == 0 )
            return;
        // Image2D has no in-place resize; recreate the chain (SceneRenderer::Resize already idled the GPU).
        CreateImage( width, height );
    }

    bool BloomRenderer::Prepare() const
    {
        const auto& scene = m_TargetFramebuffer.lock();
        return scene && scene->GetColorAttachmentImage() && m_BloomImage && m_DownsamplePipeline &&
               m_UpsamplePipeline;
    }

    void BloomRenderer::RecordDownsample( uint32_t mip )
    {
        const auto& scene = m_TargetFramebuffer.lock();
        if ( !scene )
            return;
        const bool     first  = ( mip == 0 );
        Image2D*       src    = first ? scene->GetColorAttachmentImage().get() : m_BloomImage.get();
        const uint32_t srcMip = first ? 0u : mip - 1;
        const uint32_t bw     = m_BloomImage->GetWidth();
        const uint32_t bh     = m_BloomImage->GetHeight();
        const uint32_t srcW   = first ? scene->GetFramebufferWidth() : MipSize( bw, mip - 1 );
        const uint32_t srcH   = first ? scene->GetFramebufferHeight() : MipSize( bh, mip - 1 );

        DownsamplePush push{ glm::vec2( 1.0f / static_cast<float>( srcW ), 1.0f / static_cast<float>( srcH ) ),
                             static_cast<int32_t>( srcMip ), first ? 1 : 0, m_Threshold };

        m_DownsamplePipeline->SetInput( 0, src );
        m_DownsamplePipeline->SetOutput( 1, m_BloomImage.get(), mip );
        m_DownsamplePipeline->SetPushConstants( &push, sizeof( push ) );
        Renderer::GetInstance().DispatchComputeInFrame(
             m_DownsamplePipeline.get(), GroupCount( MipSize( bw, mip ) ), GroupCount( MipSize( bh, mip ) ), 1 );
    }

    void BloomRenderer::RecordUpsample( uint32_t mip )
    {
        if ( mip == 0 )
            return;
        const uint32_t bw   = m_BloomImage->GetWidth();
        const uint32_t bh   = m_BloomImage->GetHeight();
        const uint32_t srcW = MipSize( bw, mip );
        const uint32_t srcH = MipSize( bh, mip );

        UpsamplePush push{ glm::vec2( 1.0f / static_cast<float>( srcW ), 1.0f / static_cast<float>( srcH ) ),
                           static_cast<int32_t>( mip ), kFilterRadius };

        m_UpsamplePipeline->SetInput( 0, m_BloomImage.get() );
        m_UpsamplePipeline->SetOutput( 1, m_BloomImage.get(), mip - 1 );
        m_UpsamplePipeline->SetPushConstants( &push, sizeof( push ) );
        Renderer::GetInstance().DispatchComputeInFrame( m_UpsamplePipeline.get(),
                                                        GroupCount( MipSize( bw, mip - 1 ) ),
                                                        GroupCount( MipSize( bh, mip - 1 ) ), 1 );
    }
} // namespace Desert::Graphic::System
