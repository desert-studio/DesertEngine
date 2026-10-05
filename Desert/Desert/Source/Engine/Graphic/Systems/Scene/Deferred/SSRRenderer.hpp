#pragma once

#include <Common/Core/Profiler.hpp>
#include <Engine/Graphic/ViewTargetFormats.hpp>
#include <Engine/Graphic/Systems/RenderSystem.hpp>

#include <Engine/Graphic/Image.hpp>
#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/Materials/Deferred/MaterialSSR.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <glm/glm.hpp>

#include <optional>

namespace Desert::Graphic::System
{
    // Screen-space reflections: one compute dispatch that classifies and traces, then two passes drawn over the
    // marked tiles only.
    //  1) CLASSIFY + TRACE (SSR.shader, compute, one workgroup per kTileSize x kTileSize screen tile): the tile
    //     is marked in the tile mask when any pixel passes the trace's G-buffer gate (sky / too rough -
    //     Common/SSRGate.glslh), and the same workgroup then traces at HALF resolution (as UE's SSR at its lower
    //     qualities): one jittered ray per 2x2 block of G-buffer pixels, from a block pixel that rotates every
    //     frame so the temporal resolve sees all four (rgb = reflected colour, a = reflectance). The passes
    //     below draw a grid of tile quads whose vertex stage drops unmarked tiles (Common/SSRTiles.glslh), so
    //     their cost follows the reflecting share of the screen - UE's roughness-classified SSR tiles.
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
            if ( !m_TraceShader || !m_ResolveShader || !m_CompositeShader )
                return Common::MakeError( "SSR shaders not found (SSR, SSRResolveTiled, SSRComposite)" );

            const auto& target = m_TargetFramebuffer.lock();
            if ( !target )
                return Common::MakeError( "SSR target framebuffer missing" );
            const uint32_t fullW = target->GetFramebufferWidth();
            const uint32_t fullH = target->GetFramebufferHeight();

            // Two accumulation targets (ping-pong: one is this frame's output, the other is history).
            for ( uint32_t i = 0; i < 2; ++i )
            {
                FramebufferSpecification accumSpec;
                accumSpec.DebugName = "SSRAccum" + std::to_string( i );
                accumSpec.Attachments.Attachments.emplace_back( ViewTargetFormats::kSSRAccum );
                m_AccumFB[i] = Framebuffer::Create( accumSpec );
                m_AccumFB[i]->Resize( fullW, fullH );
            }

            const auto tracePipeline =
                 ComputePipeline::Create( { .Shader = m_TraceShader, .DebugName = "SSRTrace" } );
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

        // Camera cut — see IRenderSystem::OnTemporalHistoryReset. The frame index is the jitter/noise seed
        // and the history ping-pong parity, so it restarts with the history rather than carrying the count
        // of whatever frames came before the cut.
        void OnTemporalHistoryReset() override
        {
            m_FrameIndex   = 0;
            m_HistoryValid = false;
        }

        // The two images the trace writes and the tiled passes read, both transients of this frame's graph
        // (Builder::CreateTexture): the half-resolution trace and the tile mask (one texel per kTileSize tile).
        struct TraceTargets
        {
            RDG::TextureDesc Trace;
            RDG::TextureDesc TileMask;
        };

        // Called while the frame graph is built, before any pass runs. The accumulation pair is history (read next
        // frame) and stays this renderer's: it follows the scene target here and a resize invalidates it. The
        // trace targets are sized from this frame's view and kept by no one. Nullopt: nothing to draw.
        std::optional<TraceTargets> Prepare()
        {
            const auto& target = m_TargetFramebuffer.lock();
            if ( !target || !m_TracePipeline || !m_ResolvePipeline || !m_CompositePipeline || !m_ResolveMaterial ||
                 !m_CompositeMaterial || !m_AccumFB[0] || !m_AccumFB[1] )
                return std::nullopt;
            const uint32_t w = target->GetFramebufferWidth();
            const uint32_t h = target->GetFramebufferHeight();
            if ( w == 0u || h == 0u )
                return std::nullopt;
            if ( m_AccumFB[0]->GetFramebufferWidth() != w || m_AccumFB[0]->GetFramebufferHeight() != h )
            {
                m_AccumFB[0]->Resize( w, h );
                m_AccumFB[1]->Resize( w, h );
                m_HistoryValid = false;
            }
            return TraceTargets{
                 .Trace    = RDG::TextureDesc{ .Size   = { .Width = HalfRes( w ), .Height = HalfRes( h ) },
                                               .Format = ViewTargetFormats::kSSRTrace },
                 .TileMask = RDG::TextureDesc{ .Size   = { .Width = TileGrid( w ), .Height = TileGrid( h ) },
                                               .Format = ViewTargetFormats::kSSRTileMask },
            };
        }

        // Pass 1, from the exec of the compute node that declares @p trace and @p tiles as storage writes:
        // classify
        // + half-resolution trace, one dispatch (one workgroup per tile). gbuffer = the camera G-buffer
        // (albedo/normal/worldpos at 0/1/2); sceneCopy = this frame's snapshot of the lit opaque scene
        // (FrameTransients::SceneColorCopy, declared as a SampledCompute read of the node, bound by name);
        // maxDistance and thickness are WORLD distances (a world unit is a centimetre - convert through
        // Common::Units).
        [[nodiscard]] Common::BoolResultStr RecordTrace( const RDG::PassContext& context, RDG::TextureRef trace,
                                                         RDG::TextureRef                     tiles,
                                                         const std::shared_ptr<Framebuffer>& gbuffer,
                                                         RDG::TextureRef sceneCopy, const glm::mat4& viewProj,
                                                         const glm::vec4& cameraPos, int maxSteps,
                                                         float maxDistance, float intensity, float thickness )
        {
            // Each pass is timed on its own because EnableSSR's default is a budget decision
            // (SceneSettings.hpp), and the whole-pass line cannot say which part to cut.
            DESERT_PROFILE_PASS( "SSR: Classify + Trace" );
            struct TracePush
            {
                glm::mat4 ViewProj;
                glm::vec4 CameraPos; // xyz = camera, w = per-frame seed (jitter + 2x2 rotation)
                glm::vec4 Params;    // x = maxSteps, y = maxDistance, z = intensity, w = thickness
            } push{ viewProj, glm::vec4( glm::vec3( cameraPos ), static_cast<float>( m_FrameIndex % 1024u ) ),
                    glm::vec4( static_cast<float>( maxSteps ), maxDistance, intensity, thickness ) };

            m_TracePipeline->SetInput( 0, gbuffer->GetColorAttachmentImage( 0 ).get(), RDG::Access::SampledCompute,
                                       RDG::SubresourceRange::All() );
            m_TracePipeline->SetInput( 1, gbuffer->GetColorAttachmentImage( 1 ).get(), RDG::Access::SampledCompute,
                                       RDG::SubresourceRange::All() );
            m_TracePipeline->SetInput( 2, gbuffer->GetColorAttachmentImage( 2 ).get(), RDG::Access::SampledCompute,
                                       RDG::SubresourceRange::All() );

            RDG::PassBindings bindings( context );
            // The sampler the pipeline-setter route sampled the copy with (the image's own: linear, REPEAT).
            bindings
                 .Sampled( "u_SceneColor", sceneCopy, RDG::Access::SampledCompute, RDG::SubresourceRange::All(),
                           RDG::SamplerDesc::LinearRepeat() )
                 .Storage( "u_Trace", trace, RDG::Access::StorageWrite )
                 .Storage( "u_TileMask", tiles, RDG::Access::StorageWrite )
                 .PushConstants( &push, sizeof( push ) );
            return Renderer::GetInstance().DispatchCompute( bindings, *m_TracePipeline, TileGrid( Width() ),
                                                            TileGrid( Height() ), 1 );
        }

        // Pass 2, inside the render pass the graph opens on GetAccumImage() (cleared to 0): spatial + temporal
        // resolve of @p trace (read bilinearly - the upscale) over GetHistoryImage(), drawn over the tiles
        // @p tiles marks.
        [[nodiscard]] Common::BoolResultStr RecordResolve( const RDG::PassContext& context, RDG::TextureRef trace,
                                                           RDG::TextureRef                     tiles,
                                                           const std::shared_ptr<Framebuffer>& gbuffer )
        {
            DESERT_PROFILE_PASS( "SSR: Resolve" );
            m_ResolveMaterial->BindInputs( GetHistoryImage(), gbuffer->GetColorAttachmentImage( 2 ),
                                           m_PrevViewProj, Texel(), m_HistoryValid ? 0.88f : 0.0f );
            RDG::PassBindings bindings( context );
            bindings
                 .Sampled( "u_Trace", trace, RDG::Access::SampledGraphics, RDG::SubresourceRange::Mip( 0 ),
                           RDG::SamplerDesc::LinearClamp() )
                 .Sampled( "u_SSRTileMask", tiles, RDG::Access::SampledGraphics, RDG::SubresourceRange::Mip( 0 ),
                           RDG::SamplerDesc::PointClamp() );
            return Renderer::GetInstance().DrawProcedural(
                 bindings, *m_ResolvePipeline, m_ResolveMaterial->GetMaterialExecutor(), TileVertices(), 1u );
        }

        // Pass 3, inside the render pass the graph opens on the scene target with LOAD: roughness-scaled blur
        // of the RESOLVED buffer, blended over the scene, over the tiles @p tiles marks. Advances the ping-pong
        // only when the draw was recorded.
        [[nodiscard]] Common::BoolResultStr RecordComposite( const RDG::PassContext&             context,
                                                             RDG::TextureRef                     tiles,
                                                             const std::shared_ptr<Framebuffer>& gbuffer,
                                                             const glm::mat4&                    viewProj )
        {
            DESERT_PROFILE_PASS( "SSR: Composite" );
            m_CompositeMaterial->BindInputs( GetAccumImage(), gbuffer->GetColorAttachmentImage( 1 ), Texel() );
            RDG::PassBindings bindings( context );
            bindings.Sampled( "u_SSRTileMask", tiles, RDG::Access::SampledGraphics,
                              RDG::SubresourceRange::Mip( 0 ), RDG::SamplerDesc::PointClamp() );
            const Common::BoolResultStr drawn = Renderer::GetInstance().DrawProcedural(
                 bindings, *m_CompositePipeline, m_CompositeMaterial->GetMaterialExecutor(), TileVertices(), 1u );
            if ( !drawn.IsSuccess() )
                return drawn;

            m_PrevViewProj = viewProj;
            m_HistoryValid = true;
            m_AccumIndex   = 1u - m_AccumIndex;
            ++m_FrameIndex;
            return drawn;
        }

        // Before RecordComposite: the target the resolve writes this frame and the one it reprojects.
        std::shared_ptr<Image2D> GetAccumImage() const
        {
            return m_AccumFB[m_AccumIndex] ? m_AccumFB[m_AccumIndex]->GetColorAttachmentImage( 0 ) : nullptr;
        }
        std::shared_ptr<Image2D> GetHistoryImage() const
        {
            const uint32_t prv = 1u - m_AccumIndex;
            return m_AccumFB[prv] ? m_AccumFB[prv]->GetColorAttachmentImage( 0 ) : nullptr;
        }

        // The current resolved (denoised) result / the raw trace — for the editor's debug dumps.
        std::shared_ptr<Image2D> GetResolvedImage() const
        {
            const uint32_t last = 1u - m_AccumIndex; // RecordComposite flipped the index after writing
            return m_AccumFB[last] ? m_AccumFB[last]->GetColorAttachmentImage( 0 ) : nullptr;
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
        uint32_t Width() const
        {
            return m_TargetFramebuffer.lock()->GetFramebufferWidth();
        }
        uint32_t Height() const
        {
            return m_TargetFramebuffer.lock()->GetFramebufferHeight();
        }
        glm::vec2 Texel() const
        {
            return glm::vec2( 1.0f / static_cast<float>( Width() ), 1.0f / static_cast<float>( Height() ) );
        }
        // Six vertices per tile; SSRTiles.glslh collapses the unmarked ones.
        uint32_t TileVertices() const
        {
            return TileGrid( Width() ) * TileGrid( Height() ) * 6u;
        }
        static uint32_t HalfRes( uint32_t pixels )
        {
            return ( pixels + 1u ) / 2u;
        }

        std::shared_ptr<Shader>               m_TraceShader;
        std::shared_ptr<Shader>               m_ResolveShader;
        std::shared_ptr<Shader>               m_CompositeShader;
        std::shared_ptr<ComputePipeline>      m_TracePipeline;
        std::shared_ptr<GraphicsPipeline>     m_ResolvePipeline;
        std::shared_ptr<GraphicsPipeline>     m_CompositePipeline;
        std::unique_ptr<MaterialSSRResolve>   m_ResolveMaterial;
        std::unique_ptr<MaterialSSRComposite> m_CompositeMaterial;
        std::shared_ptr<Framebuffer>          m_AccumFB[2];

        glm::mat4 m_PrevViewProj{ 1.0f };
        bool      m_HistoryValid = false;
        uint32_t  m_AccumIndex   = 0;
        uint32_t  m_FrameIndex   = 0;
    };
} // namespace Desert::Graphic::System
