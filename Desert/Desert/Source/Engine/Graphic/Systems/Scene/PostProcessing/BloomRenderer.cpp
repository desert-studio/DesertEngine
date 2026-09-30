#include "BloomRenderer.hpp"
#include <Engine/Graphic/ViewTargetFormats.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>

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
            int32_t   FirstPass;
            float     Threshold;
        };

        struct UpsamplePush
        {
            glm::vec2 SrcTexelSize;
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
        if ( !CreatePipelines() )
            return Common::MakeError( "BloomRenderer: failed to create compute pipelines" );

        return BOOLSUCCESS;
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

    std::optional<RDG::TextureDesc> BloomRenderer::GetChainDesc() const
    {
        const auto& scene = m_TargetFramebuffer.lock();
        if ( !scene || !scene->GetColorAttachmentImage() || !m_DownsamplePipeline || !m_UpsamplePipeline )
            return std::nullopt;
        // Half-resolution chain (mip 0 = scene / 2), capped so the smallest mip stays usable.
        const uint32_t bw = std::max( 1u, scene->GetFramebufferWidth() / 2 );
        const uint32_t bh = std::max( 1u, scene->GetFramebufferHeight() / 2 );
        return RDG::TextureDesc{ .Size   = { .Width = bw, .Height = bh },
                                 .Format = kBloomFormat,
                                 .Mips   = std::min( kMaxBloomMips, Utils::CalculateMipCount( bw, bh ) ) };
    }

    Common::BoolResultStr BloomRenderer::RecordDownsample( const RDG::PassContext& context,
                                                           RDG::TextureRef sceneColor, RDG::TextureRef chain,
                                                           const RDG::TextureDesc& chainDesc, uint32_t mip )
    {
        const bool     first  = ( mip == 0 );
        const uint32_t bw     = chainDesc.Size.Width;
        const uint32_t bh     = chainDesc.Size.Height;
        uint32_t       srcW   = 0;
        uint32_t       srcH   = 0;
        if ( first )
        {
            // Mip 0 samples the full-resolution scene colour.
            const auto& scene = m_TargetFramebuffer.lock();
            if ( !scene )
                return Common::MakeError( "BloomRenderer: the scene framebuffer is gone" );
            srcW = scene->GetFramebufferWidth();
            srcH = scene->GetFramebufferHeight();
        }
        else
        {
            srcW = MipSize( bw, mip - 1 );
            srcH = MipSize( bh, mip - 1 );
        }

        const DownsamplePush push{ glm::vec2( 1.0f / static_cast<float>( srcW ), 1.0f / static_cast<float>( srcH ) ),
                                   first ? 1 : 0, m_Threshold };

        RDG::PassBindings bindings( context );
        if ( first )
            bindings.Sampled( "u_Source", sceneColor, RDG::Access::SampledCompute );
        else
            bindings.Sampled( "u_Source", chain, RDG::Access::SampledCompute, RDG::SubresourceRange::Mip( mip - 1 ) );
        bindings.Storage( "u_Output", chain, RDG::Access::StorageWrite, mip )
             .PushConstants( &push, sizeof( push ) );
        return Renderer::GetInstance().DispatchCompute( bindings, *m_DownsamplePipeline,
                                                        GroupCount( MipSize( bw, mip ) ),
                                                        GroupCount( MipSize( bh, mip ) ), 1 );
    }

    Common::BoolResultStr BloomRenderer::RecordUpsample( const RDG::PassContext& context, RDG::TextureRef chain,
                                                         const RDG::TextureDesc& chainDesc, uint32_t mip )
    {
        if ( mip == 0 )
            return Common::MakeError( "BloomRenderer: an upsample reads mip >= 1" );
        const uint32_t bw = chainDesc.Size.Width;
        const uint32_t bh = chainDesc.Size.Height;

        const UpsamplePush push{ glm::vec2( 1.0f / static_cast<float>( MipSize( bw, mip ) ),
                                            1.0f / static_cast<float>( MipSize( bh, mip ) ) ),
                                 kFilterRadius };

        RDG::PassBindings bindings( context );
        bindings.Sampled( "u_Source", chain, RDG::Access::SampledCompute, RDG::SubresourceRange::Mip( mip ) )
             .Storage( "u_Output", chain, RDG::Access::StorageWrite, mip - 1 )
             .PushConstants( &push, sizeof( push ) );
        return Renderer::GetInstance().DispatchCompute( bindings, *m_UpsamplePipeline,
                                                        GroupCount( MipSize( bw, mip - 1 ) ),
                                                        GroupCount( MipSize( bh, mip - 1 ) ), 1 );
    }
} // namespace Desert::Graphic::System
