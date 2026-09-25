#pragma once

#include <Engine/Graphic/Systems/RenderSystem.hpp>
#include <Engine/Graphic/ViewTargetFormats.hpp>

#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/Materials/Deferred/MaterialGIResolve.hpp>
#include <Engine/Graphic/Materials/Deferred/MaterialSSR.hpp> // MaterialSSRResolve (shared temporal resolve)
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <glm/glm.hpp>

namespace Desert::Graphic::System
{
    // One-bounce RSM GI, two passes:
    //  1) GATHER: fullscreen jittered VPL gather into the raw GI buffer (the system's target framebuffer).
    //     The jitter seed changes EVERY frame.
    //  2) RESOLVE: the SAME temporal+spatial denoiser SSR uses (SSRResolve shader) — 5x5 alpha-weighted
    //     spatial filter + reprojected, AABB-clamped exponential history (ping-pong targets). Close-up
    //     camera angles make the per-pixel VPL variance huge; only accumulation over frames converges it.
    // The deferred lighting then blur-reads the RESOLVED buffer.
    class GIResolveRenderer final : public RenderSystem
    {
    public:
        using RenderSystem::RenderSystem;

        virtual Common::BoolResultStr Initialize() override
        {
            m_Shader        = Runtime::ResourceRegistry::GetShaderService()->GetByName( "GIResolve" );
            m_ResolveShader = Runtime::ResourceRegistry::GetShaderService()->GetByName( "SSRResolve" );
            if ( !m_Shader || !m_ResolveShader )
                return Common::MakeError( "GIResolve/SSRResolve shader not found" );

            const auto& target = m_TargetFramebuffer.lock();
            if ( !target )
                return Common::MakeError( "GI buffer missing" );

            for ( uint32_t i = 0; i < 2; ++i )
            {
                FramebufferSpecification accumSpec;
                accumSpec.DebugName = "GIAccum" + std::to_string( i );
                accumSpec.Attachments.Attachments.emplace_back( ViewTargetFormats::kGIAccum );
                m_AccumFB[i] = Framebuffer::Create( accumSpec );
                m_AccumFB[i]->Resize( target->GetFramebufferWidth(), target->GetFramebufferHeight() );
            }

            GraphicsPipelineSpecification spec;
            spec.DebugName         = "GIResolve";
            spec.Framebuffer       = target;
            spec.Shader            = m_Shader;
            spec.DepthTestEnabled  = false;
            spec.DepthWriteEnabled = false;
            const auto pipeline    = Graphic::GraphicsPipeline::Create( spec );
            if ( !pipeline )
                return Common::MakeError( pipeline.GetError() );
            m_Pipeline = pipeline.GetValue();

            GraphicsPipelineSpecification resolveSpec;
            resolveSpec.DebugName         = "GITemporalResolve";
            resolveSpec.Framebuffer       = m_AccumFB[0]; // render-pass compatible with both accum targets
            resolveSpec.Shader            = m_ResolveShader;
            resolveSpec.DepthTestEnabled  = false;
            resolveSpec.DepthWriteEnabled = false;
            const auto resolvePipeline    = Graphic::GraphicsPipeline::Create( resolveSpec );
            if ( !resolvePipeline )
                return Common::MakeError( resolvePipeline.GetError() );
            m_ResolvePipeline = resolvePipeline.GetValue();

            m_Material        = std::make_unique<MaterialGIResolve>();
            m_ResolveMaterial = std::make_unique<MaterialSSRResolve>();
            return BOOLSUCCESS;
        }

        void RegisterPasses( RenderGraphBuilder& ) override
        {
        }

        // The accumulated indirect light is a reprojection of the previous frame, and the previous frame is
        // now a different world — see IRenderSystem::OnSceneReplaced, kind 1. The blend weight is 0.92, so
        // without this the first scene's bounce light survives in the second for tens of frames wherever the
        // neighbourhood clamp does not reject it. A resize invalidates it for the same reason; this is the
        // other event that does.
        void OnSceneReplaced() override
        {
            m_HistoryValid = false;
        }

        void Execute( const std::shared_ptr<Framebuffer>& gbuffer, const std::shared_ptr<Image2D>& rsmAlbedo,
                      const std::shared_ptr<Image2D>& rsmNormal, const std::shared_ptr<Image2D>& rsmWorldPos,
                      const glm::mat4& rsmViewProj, const glm::mat4& cameraViewProj,
                      const glm::vec4& sunColorIntensity, float giIntensity )
        {
            const auto& target = m_TargetFramebuffer.lock();
            if ( !target || !gbuffer || !m_Pipeline || !m_ResolvePipeline || !m_Material ||
                 !m_ResolveMaterial || !m_AccumFB[0] || !m_AccumFB[1] )
                return;

            auto& renderer = Renderer::GetInstance();

            const uint32_t w = target->GetFramebufferWidth();
            const uint32_t h = target->GetFramebufferHeight();
            if ( m_AccumFB[0]->GetFramebufferWidth() != w || m_AccumFB[0]->GetFramebufferHeight() != h )
            {
                m_AccumFB[0]->Resize( w, h );
                m_AccumFB[1]->Resize( w, h );
                m_HistoryValid = false;
            }

            // --- Pass 1: jittered VPL gather into the raw GI buffer. ---
            {
                RenderPassSpecification rp;
                rp.TargetFramebuffer = target;
                rp.DebugName         = "GIResolvePass";
                rp.ClearColor.Color  = glm::vec4( 0.0f );
                auto pass            = RenderPass::Create( rp );

                renderer.BeginRenderPass( pass.get() );
                m_Material->BindInputs( gbuffer->GetColorAttachmentImage( 1 ),
                                        gbuffer->GetColorAttachmentImage( 2 ), rsmAlbedo, rsmNormal, rsmWorldPos,
                                        rsmViewProj, sunColorIntensity, giIntensity,
                                        static_cast<float>( m_FrameIndex % 1024u ) );
                renderer.SubmitFullscreenQuad( m_Pipeline.get(), m_Material->GetMaterialExecutor() );
                renderer.EndRenderPass();
            }

            // --- Pass 2: temporal accumulation (shared SSRResolve denoiser) into the ping-pong target. ---
            const uint32_t  cur = m_AccumIndex;
            const uint32_t  prv = 1u - m_AccumIndex;
            const glm::vec2 texel( 1.0f / static_cast<float>( w ), 1.0f / static_cast<float>( h ) );
            {
                RenderPassSpecification rp;
                rp.TargetFramebuffer = m_AccumFB[cur];
                rp.DebugName         = "GITemporalResolvePass";
                rp.ClearColor.Color  = glm::vec4( 0.0f );
                auto pass            = RenderPass::Create( rp );

                renderer.BeginRenderPass( pass.get() );
                m_ResolveMaterial->BindInputs(
                     target->GetColorAttachmentImage( 0 ), m_AccumFB[prv]->GetColorAttachmentImage( 0 ),
                     gbuffer->GetColorAttachmentImage( 2 ), m_PrevViewProj, texel, m_HistoryValid ? 0.92f : 0.0f );
                renderer.SubmitFullscreenQuad( m_ResolvePipeline.get(),
                                               m_ResolveMaterial->GetMaterialExecutor() );
                renderer.EndRenderPass();
            }

            m_PrevViewProj = cameraViewProj;
            m_HistoryValid = true;
            m_AccumIndex   = prv;
            ++m_FrameIndex;
        }

        // The temporally-resolved (denoised) indirect light — what the lighting pass should read.
        std::shared_ptr<Image2D> GetGIImage() const
        {
            const uint32_t last = 1u - m_AccumIndex; // Execute flipped the index after writing
            return m_AccumFB[last] ? m_AccumFB[last]->GetColorAttachmentImage( 0 ) : nullptr;
        }

    private:
        std::shared_ptr<Shader>             m_Shader;
        std::shared_ptr<Shader>             m_ResolveShader;
        std::shared_ptr<GraphicsPipeline>   m_Pipeline;
        std::shared_ptr<GraphicsPipeline>   m_ResolvePipeline;
        std::unique_ptr<MaterialGIResolve>  m_Material;
        std::unique_ptr<MaterialSSRResolve> m_ResolveMaterial;
        std::shared_ptr<Framebuffer>        m_AccumFB[2];

        glm::mat4 m_PrevViewProj{ 1.0f };
        bool      m_HistoryValid = false;
        uint32_t  m_AccumIndex   = 0;
        uint32_t  m_FrameIndex   = 0;
    };
} // namespace Desert::Graphic::System
