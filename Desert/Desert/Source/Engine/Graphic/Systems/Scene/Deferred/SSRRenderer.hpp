#pragma once

#include <Common/Core/Profiler.hpp>
#include <Engine/Graphic/ViewTargetFormats.hpp>
#include <Engine/Graphic/Systems/RenderSystem.hpp>

#include <Engine/Graphic/Image.hpp>
#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/Materials/Deferred/MaterialSSR.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <glm/glm.hpp>

namespace Desert::Graphic::System
{
    // Screen-space reflections. A tile classification, then three passes drawn over the marked tiles only:
    //  0) CLASSIFY (compute, one thread per pixel, one workgroup per tile): one texel per kTileSize x
    //     kTileSize screen tile, 1 where any pixel passes the trace's G-buffer gate (sky / too rough -
    //     Common/SSRGate.glslh). The three passes below draw a grid of tile quads whose vertex stage drops
    //     unmarked tiles (Common/SSRTiles.glslh), so their cost follows the reflecting share of the screen.
    //     UE does the same with its roughness-classified SSR tiles.
    //  1) TRACE at HALF resolution (as UE's SSR at its lower qualities): one jittered ray per 2x2 block of
    //     G-buffer pixels, from a block pixel that rotates every frame so the temporal resolve sees all four
    //     (rgb = reflected colour, a = reflectance). The ray jitter seed changes EVERY frame too.
    //  2) RESOLVE (the denoiser), at full resolution, reading the half-resolution trace bilinearly - the
    //     upscale: 5x5 alpha-weighted spatial filter + TEMPORAL accumulation — this
    //     pixel's world position is reprojected through last frame's camera and the previous resolved
    //     result is blended in (AABB-clamped against the current neighbourhood so it can't ghost).
    //     Ping-pongs between two accumulation targets.
    //  3) COMPOSITE: roughness-scaled blur of the resolved buffer, blended over the scene target.
    // Runs in the manual chain after deferred lighting + the scene-colour copy.
    class SSRRenderer final : public RenderSystem
    {
    public:
        using RenderSystem::RenderSystem;

        virtual Common::BoolResultStr Initialize() override
        {
            m_TraceShader     = Runtime::ResourceRegistry::GetShaderService()->GetByName( "SSR" );
            m_ResolveShader   = Runtime::ResourceRegistry::GetShaderService()->GetByName( "SSRResolveTiled" );
            m_CompositeShader = Runtime::ResourceRegistry::GetShaderService()->GetByName( "SSRComposite" );
            m_ClassifyShader  = Runtime::ResourceRegistry::GetShaderService()->GetByName( "SSRTileClassify" );
            if ( !m_TraceShader || !m_ResolveShader || !m_CompositeShader || !m_ClassifyShader )
                return Common::MakeError(
                     "SSR shaders not found (SSR, SSRResolveTiled, SSRComposite, SSRTileClassify)" );

            const auto& target = m_TargetFramebuffer.lock();
            if ( !target )
                return Common::MakeError( "SSR target framebuffer missing" );
            const uint32_t fullW = target->GetFramebufferWidth();
            const uint32_t fullH = target->GetFramebufferHeight();

            FramebufferSpecification traceSpecFB;
            traceSpecFB.DebugName = "SSRTrace";
            traceSpecFB.Attachments.Attachments.emplace_back( ViewTargetFormats::kSSRTrace );
            m_TraceBuffer = Framebuffer::Create( traceSpecFB );
            m_TraceBuffer->Resize( HalfRes( fullW ), HalfRes( fullH ) );

            // Two accumulation targets (ping-pong: one is this frame's output, the other is history).
            for ( uint32_t i = 0; i < 2; ++i )
            {
                FramebufferSpecification accumSpec;
                accumSpec.DebugName = "SSRAccum" + std::to_string( i );
                accumSpec.Attachments.Attachments.emplace_back( ViewTargetFormats::kSSRAccum );
                m_AccumFB[i] = Framebuffer::Create( accumSpec );
                m_AccumFB[i]->Resize( fullW, fullH );
            }

            if ( !CreateTileMask( TileGrid( fullW ), TileGrid( fullH ) ) )
                return Common::MakeError( "SSR tile mask image could not be created" );

            const auto classifyPipeline =
                 ComputePipeline::Create( { .Shader = m_ClassifyShader, .DebugName = "SSRTileClassify" } );
            if ( !classifyPipeline )
                return Common::MakeError( classifyPipeline.GetError() );
            m_ClassifyPipeline = classifyPipeline.GetValue();

            GraphicsPipelineSpecification traceSpec;
            traceSpec.DebugName         = "SSRTrace";
            traceSpec.Framebuffer       = m_TraceBuffer;
            traceSpec.Shader            = m_TraceShader;
            traceSpec.DepthTestEnabled  = false;
            traceSpec.DepthWriteEnabled = false;
            const auto tracePipeline    = Graphic::GraphicsPipeline::Create( traceSpec );
            if ( !tracePipeline )
                return Common::MakeError( tracePipeline.GetError() );
            m_TracePipeline = tracePipeline.GetValue();

            GraphicsPipelineSpecification resolveSpec;
            resolveSpec.DebugName         = "SSRResolveTiled";
            resolveSpec.Framebuffer       = m_AccumFB[0]; // render-pass compatible with both accum targets
            resolveSpec.Shader            = m_ResolveShader;
            resolveSpec.DepthTestEnabled  = false;
            resolveSpec.DepthWriteEnabled = false;
            const auto resolvePipeline    = Graphic::GraphicsPipeline::Create( resolveSpec );
            if ( !resolvePipeline )
                return Common::MakeError( resolvePipeline.GetError() );
            m_ResolvePipeline = resolvePipeline.GetValue();

            GraphicsPipelineSpecification compSpec;
            compSpec.DebugName         = "SSRComposite";
            compSpec.Framebuffer       = target;
            compSpec.Shader            = m_CompositeShader;
            compSpec.DepthTestEnabled  = false;
            compSpec.DepthWriteEnabled = false;
            compSpec.BlendEnable       = true; // src-alpha: reflection replaces the scene by reflectance
            compSpec.UseLoadRenderPass = true; // composite over the lit scene
            const auto compPipeline    = Graphic::GraphicsPipeline::Create( compSpec );
            if ( !compPipeline )
                return Common::MakeError( compPipeline.GetError() );
            m_CompositePipeline = compPipeline.GetValue();

            m_Material          = std::make_unique<MaterialSSR>();
            m_ResolveMaterial   = std::make_unique<MaterialSSRResolve>( SSRResolveVariant::Tiled );
            m_CompositeMaterial = std::make_unique<MaterialSSRComposite>();
            return BOOLSUCCESS;
        }

        void RegisterPasses( RenderGraphBuilder& ) override
        {
        }

        // The accumulated reflection is a reprojection of the previous frame, and the previous frame is now
        // a different world — see IRenderSystem::OnSceneReplaced, kind 1. Same argument as GIResolveRenderer
        // next door, at a blend weight of 0.88.
        void OnSceneReplaced() override
        {
            m_HistoryValid = false;
        }

        // gbuffer = the camera G-buffer (albedo/normal/worldpos at 0/1/2); sceneColor = snapshot of the lit
        // opaque scene; viewProj/cameraPos = the camera; maxDistance and thickness are WORLD distances,
        // and a world unit is a centimetre - callers passing literature values convert through Common::Units.
        void Execute( const std::shared_ptr<Framebuffer>& gbuffer, const std::shared_ptr<Image2D>& sceneColor,
                      const glm::mat4& viewProj, const glm::vec4& cameraPos, int maxSteps, float maxDistance,
                      float intensity, float thickness )
        {
            const auto& target = m_TargetFramebuffer.lock();
            if ( !target || !gbuffer || !sceneColor || !m_TracePipeline || !m_ResolvePipeline ||
                 !m_CompositePipeline || !m_Material || !m_ResolveMaterial || !m_CompositeMaterial ||
                 !m_TraceBuffer || !m_AccumFB[0] || !m_AccumFB[1] || !m_ClassifyPipeline || !m_TileMask )
                return;

            auto& renderer = Renderer::GetInstance();

            // Every SSR target follows the scene target (the trace at half its size); a resize invalidates
            // the history.
            const uint32_t w = target->GetFramebufferWidth();
            const uint32_t h = target->GetFramebufferHeight();
            if ( m_AccumFB[0]->GetFramebufferWidth() != w || m_AccumFB[0]->GetFramebufferHeight() != h )
            {
                m_AccumFB[0]->Resize( w, h );
                m_AccumFB[1]->Resize( w, h );
                m_TraceBuffer->Resize( HalfRes( w ), HalfRes( h ) );
                m_HistoryValid = false;
            }
            const uint32_t gridW = TileGrid( w );
            const uint32_t gridH = TileGrid( h );
            if ( ( m_TileMask->GetWidth() != gridW || m_TileMask->GetHeight() != gridH ) &&
                 !CreateTileMask( gridW, gridH ) )
                return;
            // Six vertices per tile; SSRTiles.glslh collapses the unmarked ones.
            const uint32_t tileVertices = gridW * gridH * 6u;
            const auto&    tileMask     = m_TileMask;

            // --- Pass 0: which tiles hold a pixel the trace can write (one workgroup per tile). ---
            {
                DESERT_PROFILE_PASS( "SSR: Classify" );
                renderer.ComputeImageBeginWrite( m_TileMask.get() );
                m_ClassifyPipeline->SetInput( 0, gbuffer->GetColorAttachmentImage( 1 ).get() );
                m_ClassifyPipeline->SetOutput( 1, m_TileMask.get() );
                renderer.DispatchComputeInFrame( m_ClassifyPipeline.get(), gridW, gridH, 1 );
                renderer.ComputeImageEndWrite( m_TileMask.get() );
            }

            // --- Pass 1: jittered trace into the trace buffer (cleared to 0 = "no reflection"). ---
            {
                // Each pass is timed on its own because EnableSSR's default is a budget decision
                // (SceneSettings.hpp), and the whole-pass line cannot say which part to cut.
                DESERT_PROFILE_PASS( "SSR: Trace" );
                RenderPassSpecification rp;
                rp.TargetFramebuffer = m_TraceBuffer;
                rp.DebugName         = "SSRTracePass";
                rp.ClearColor.Color  = glm::vec4( 0.0f );
                auto pass            = RenderPass::Create( rp );

                renderer.BeginRenderPass( pass.get() );
                m_Material->BindInputs(
                     gbuffer->GetColorAttachmentImage( 0 ), gbuffer->GetColorAttachmentImage( 1 ),
                     gbuffer->GetColorAttachmentImage( 2 ), sceneColor, viewProj, cameraPos, maxSteps, maxDistance,
                     intensity, thickness, static_cast<float>( m_FrameIndex % 1024u ) );
                m_Material->BindTileMask( tileMask );
                renderer.SubmitVertices( m_TracePipeline.get(), tileVertices, m_Material->GetMaterialExecutor() );
                renderer.EndRenderPass();
            }

            // --- Pass 2: spatial + temporal resolve into this frame's accumulation target. ---
            const uint32_t  cur = m_AccumIndex;
            const uint32_t  prv = 1u - m_AccumIndex;
            const glm::vec2 texel( 1.0f / static_cast<float>( w ), 1.0f / static_cast<float>( h ) );
            {
                DESERT_PROFILE_PASS( "SSR: Resolve" );
                RenderPassSpecification rp;
                rp.TargetFramebuffer = m_AccumFB[cur];
                rp.DebugName         = "SSRResolvePass";
                rp.ClearColor.Color  = glm::vec4( 0.0f );
                auto pass            = RenderPass::Create( rp );

                renderer.BeginRenderPass( pass.get() );
                m_ResolveMaterial->BindInputs(
                     m_TraceBuffer->GetColorAttachmentImage( 0 ), m_AccumFB[prv]->GetColorAttachmentImage( 0 ),
                     gbuffer->GetColorAttachmentImage( 2 ), m_PrevViewProj, texel, m_HistoryValid ? 0.88f : 0.0f );
                m_ResolveMaterial->BindTileMask( tileMask );
                renderer.SubmitVertices( m_ResolvePipeline.get(), tileVertices,
                                         m_ResolveMaterial->GetMaterialExecutor() );
                renderer.EndRenderPass();
            }

            // --- Pass 3: roughness-scaled blur of the RESOLVED buffer, blended over the scene. ---
            {
                DESERT_PROFILE_PASS( "SSR: Composite" );
                m_CompositeMaterial->BindInputs( m_AccumFB[cur]->GetColorAttachmentImage( 0 ),
                                                 gbuffer->GetColorAttachmentImage( 1 ), texel );
                m_CompositeMaterial->BindTileMask( tileMask );

                RenderPassSpecification rp;
                rp.TargetFramebuffer = target;
                rp.DebugName         = "SSRCompositePass";
                auto pass            = RenderPass::Create( rp );

                renderer.BeginRenderPass( pass.get(), false ); // LOAD: blend over the scene
                renderer.SubmitVertices( m_CompositePipeline.get(), tileVertices,
                                         m_CompositeMaterial->GetMaterialExecutor() );
                renderer.EndRenderPass();
            }

            m_PrevViewProj = viewProj;
            m_HistoryValid = true;
            m_AccumIndex   = prv;
            ++m_FrameIndex;
        }

        // The current resolved (denoised) result / the raw trace — for the editor's debug dumps.
        std::shared_ptr<Image2D> GetResolvedImage() const
        {
            const uint32_t last = 1u - m_AccumIndex; // Execute flipped the index after writing
            return m_AccumFB[last] ? m_AccumFB[last]->GetColorAttachmentImage( 0 ) : nullptr;
        }
        std::shared_ptr<Image2D> GetTraceImage() const
        {
            return m_TraceBuffer ? m_TraceBuffer->GetColorAttachmentImage( 0 ) : nullptr;
        }

    private:
        // Screen pixels per tile edge. The grid is ceil(size / kTileSize) and SSRTiles.glslh stretches the
        // tiles over the target, so every pixel belongs to exactly one tile.
        static constexpr uint32_t kTileSize = 8;
        static uint32_t           TileGrid( uint32_t pixels )
        {
            return ( pixels + kTileSize - 1u ) / kTileSize;
        }
        // The trace's size: one texel per 2x2 block, rounded up so an odd edge column still has a block.
        static uint32_t HalfRes( uint32_t pixels )
        {
            return ( pixels + 1u ) / 2u;
        }

        // Storage (the classify writes it) + sampled (the tiled passes' vertex stage reads it).
        bool CreateTileMask( uint32_t gridW, uint32_t gridH )
        {
            const Core::Formats::Image2DSpecification spec = {
                 .Tag        = "SSRTileMask",
                 .Width      = gridW,
                 .Height     = gridH,
                 .Format     = ViewTargetFormats::kSSRTileMask,
                 .Mips       = 1,
                 .Usage      = Core::Formats::Image2DUsage::Image2D,
                 .Properties = Core::Formats::Storage | Core::Formats::Sample,
            };
            m_TileMask = Image2D::Create( spec );
            return m_TileMask != nullptr;
        }

        std::shared_ptr<Shader>               m_ClassifyShader;
        std::shared_ptr<ComputePipeline>      m_ClassifyPipeline;
        std::shared_ptr<Image2D>              m_TileMask;
        std::shared_ptr<Shader>               m_TraceShader;
        std::shared_ptr<Shader>               m_ResolveShader;
        std::shared_ptr<Shader>               m_CompositeShader;
        std::shared_ptr<GraphicsPipeline>     m_TracePipeline;
        std::shared_ptr<GraphicsPipeline>     m_ResolvePipeline;
        std::shared_ptr<GraphicsPipeline>     m_CompositePipeline;
        std::unique_ptr<MaterialSSR>          m_Material;
        std::unique_ptr<MaterialSSRResolve>   m_ResolveMaterial;
        std::unique_ptr<MaterialSSRComposite> m_CompositeMaterial;
        std::shared_ptr<Framebuffer>          m_TraceBuffer;
        std::shared_ptr<Framebuffer>          m_AccumFB[2];

        glm::mat4 m_PrevViewProj{ 1.0f };
        bool      m_HistoryValid = false;
        uint32_t  m_AccumIndex   = 0;
        uint32_t  m_FrameIndex   = 0;
    };
} // namespace Desert::Graphic::System
