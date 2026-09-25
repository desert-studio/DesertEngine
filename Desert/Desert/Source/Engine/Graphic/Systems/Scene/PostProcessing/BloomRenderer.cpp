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

    void BloomRenderer::Execute()
    {
        const auto& scene = m_TargetFramebuffer.lock();
        if ( !scene || !m_BloomImage || !m_DownsamplePipeline || !m_UpsamplePipeline )
            return;

        Image2D* sceneColor = scene->GetColorAttachmentImage().get();
        Image2D* bloom      = m_BloomImage.get();
        if ( !sceneColor )
            return;

        const uint32_t sceneW = scene->GetFramebufferWidth();
        const uint32_t sceneH = scene->GetFramebufferHeight();
        const uint32_t bw     = m_BloomImage->GetWidth();
        const uint32_t bh     = m_BloomImage->GetHeight();

        auto& renderer = Renderer::GetInstance();

        // Keep the whole chain in GENERAL; the renderer inserts a graphics->compute barrier here and a
        // compute->compute|fragment barrier after each dispatch.
        renderer.ComputeImageBeginWrite( bloom );

        // --- Downsample: scene -> mip0 (Karis + threshold), then mip(i-1) -> mip(i). ---
        for ( uint32_t i = 0; i < m_MipLevels; ++i )
        {
            const bool     first  = ( i == 0 );
            Image2D*       src    = first ? sceneColor : bloom;
            const uint32_t srcMip = first ? 0u : i - 1;
            const uint32_t srcW   = first ? sceneW : MipSize( bw, i - 1 );
            const uint32_t srcH   = first ? sceneH : MipSize( bh, i - 1 );

            DownsamplePush push{ glm::vec2( 1.0f / static_cast<float>( srcW ), 1.0f / static_cast<float>( srcH ) ),
                                 static_cast<int32_t>( srcMip ), first ? 1 : 0, m_Threshold };

            m_DownsamplePipeline->SetInput( 0, src );
            m_DownsamplePipeline->SetOutput( 1, bloom, i );
            m_DownsamplePipeline->SetPushConstants( &push, sizeof( push ) );
            renderer.DispatchComputeInFrame( m_DownsamplePipeline.get(), GroupCount( MipSize( bw, i ) ),
                                             GroupCount( MipSize( bh, i ) ), 1 );
        }

        // --- Upsample (additive): mip(i) -> mip(i-1), walking back to mip0. ---
        for ( uint32_t i = m_MipLevels - 1; i >= 1; --i )
        {
            const uint32_t srcW = MipSize( bw, i );
            const uint32_t srcH = MipSize( bh, i );

            UpsamplePush push{ glm::vec2( 1.0f / static_cast<float>( srcW ), 1.0f / static_cast<float>( srcH ) ),
                               static_cast<int32_t>( i ), kFilterRadius };

            m_UpsamplePipeline->SetInput( 0, bloom );
            m_UpsamplePipeline->SetOutput( 1, bloom, i - 1 );
            m_UpsamplePipeline->SetPushConstants( &push, sizeof( push ) );
            renderer.DispatchComputeInFrame( m_UpsamplePipeline.get(), GroupCount( MipSize( bw, i - 1 ) ),
                                             GroupCount( MipSize( bh, i - 1 ) ), 1 );
        }

        // Back to SHADER_READ_ONLY so tonemap can sample mip 0.
        renderer.ComputeImageEndWrite( bloom );
    }
} // namespace Desert::Graphic::System
