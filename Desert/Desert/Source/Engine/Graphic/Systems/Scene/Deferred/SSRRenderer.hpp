#pragma once

#include <Common/Core/Profiler.hpp>
#include <Engine/Graphic/ViewTargetLayouts.hpp>
#include <Engine/Graphic/ViewTargetFormats.hpp>
#include <Engine/Graphic/Systems/RenderSystem.hpp>

#include <Engine/Graphic/Image.hpp>
#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/ShaderBindingLayoutCache.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/Materials/Deferred/MaterialSSR.hpp>
#include <Engine/Graphic/View/PassHistory.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <glm/glm.hpp>

#include <array>
#include <cstddef>
#include <optional>
#include <span>

namespace Desert::Graphic::System
{
    // Screen-space reflections (SSR2): one compute dispatch that classifies and traces, then a resolve drawn over
    // the marked tiles only. The deferred lighting composes the result (DeferredLighting.shader u_SSR).
    //  1) CLASSIFY + TRACE (SSR.shader, compute, one workgroup per kTileSize x kTileSize screen tile): the tile
    //     is marked in the tile mask when any pixel passes the trace's G-buffer gate (sky / too rough -
    //     Common/SSRGate.glslh), and the same workgroup then traces at HALF resolution (as UE's SSR at its lower
    //     qualities), from a block pixel that rotates every frame so the temporal resolve sees all four:
    //     Reflections.RaysPerPixel GGX-VNDF rays per texel, each walked over the closest-depth HZB
    //     (HZBRenderer, Common/HZBTrace.glslh), combined with the ratio estimator (rgb = radiance of the hits,
    //     a = the share of the lobe they answered) and written with the texel's representative ray (direction +
    //     pdf) for the resolve. A hit's radiance is LAST frame's lit scene where the hit pixel's VELOCITY says it
    //     was (a moving object is read where it was - no trail): the scene colour history this renderer owns,
    //     which this frame's scene copy refills (UE: the previous frame's scene colour). The passes below draw a
    //     grid of tile quads whose vertex stage drops unmarked tiles (Common/SSRTiles.glslh), so their cost
    //     follows the reflecting share of the screen - UE's roughness-classified SSR tiles.
    //  2) RESOLVE, at full resolution - the upscale: the RATIO ESTIMATOR over the 3x3 neighbouring trace texels
    //     (each texel's ray reweighted by this pixel's BRDF over the pdf that drew it, Stachowiak 2015) +
    //     TEMPORAL accumulation - the previous resolved result where this pixel was (its velocity) is blended
    //     in, AABB-clamped against the current neighbourhood so it can't ghost.
    //     Ping-pongs between two accumulation targets; the one written this frame is what the lighting reads.
    // Runs in the manual chain after the G-buffer and the HZB, BEFORE the deferred lighting.
    class SSRRenderer final : public RenderSystem
    {
    public:
        using RenderSystem::RenderSystem;

        virtual Common::BoolResultStr Initialize() override
        {
            m_TraceShader     = Runtime::ResourceRegistry::GetShaderService()->GetByName( "SSR" );
            m_ResolveShader   = Runtime::ResourceRegistry::GetShaderService()->GetByName( "SSRResolveTiled" );
            if ( !m_TraceShader || !m_ResolveShader )
                return Common::MakeError( "SSR shaders not found (SSR, SSRResolveTiled)" );

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
            // The scene colour history: last frame's lit opaque scene, the radiance a hit reflects.
            FramebufferSpecification historySpec;
            historySpec.DebugName = "SSRSceneColorHistory";
            historySpec.Attachments.Attachments.emplace_back( ViewTargetFormats::kSceneColorCopy );
            m_SceneColorHistoryFB = Framebuffer::Create( historySpec );
            m_SceneColorHistoryFB->Resize( fullW, fullH );

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

            m_ResolveMaterial = std::make_unique<MaterialSSRReflectionResolve>();
            return BOOLSUCCESS;
        }

        // The accumulated reflection is a reprojection of the previous frame, and the previous frame is now
        // a different world — see IRenderSystem::OnSceneReplaced, kind 1. Same argument as GIResolveRenderer
        // next door, at a blend weight of 0.88.
        void OnSceneReplaced() override
        {
            m_History.Invalidate();
            m_SceneColorHistory.Invalidate();
        }

        // Camera cut — see IRenderSystem::OnTemporalHistoryReset. The frame index is the jitter/noise seed
        // and the history ping-pong parity, so it restarts with the history rather than carrying the count
        // of whatever frames came before the cut.
        void OnTemporalHistoryReset() override
        {
            m_FrameIndex   = 0;
            m_History.Invalidate();
            m_SceneColorHistory.Invalidate();
        }

        // The two images the trace writes and the tiled passes read, both transients of this frame's graph
        // (Builder::CreateTexture): the half-resolution trace and the tile mask (one texel per kTileSize tile).
        struct TraceTargets
        {
            RDG::TextureDesc Trace;
            RDG::TextureDesc TraceRay; // the trace texels' hit points + ray pdfs (SSR.shader u_TraceRay)
            RDG::TextureDesc TileMask;
        };

        // Called while the frame graph is built, before any pass runs. The accumulation pair is history (read next
        // frame) and stays this renderer's: it follows the scene target here and a resize invalidates it. The
        // trace targets are sized from this frame's view and kept by no one. Nullopt: nothing to draw.
        std::optional<TraceTargets> Prepare()
        {
            const auto& target = m_TargetFramebuffer.lock();
            if ( !target || !m_TracePipeline || !m_ResolvePipeline || !m_ResolveMaterial || !m_AccumFB[0] ||
                 !m_AccumFB[1] || !m_SceneColorHistoryFB )
                return std::nullopt;
            const uint32_t w = target->GetFramebufferWidth();
            const uint32_t h = target->GetFramebufferHeight();
            if ( w == 0u || h == 0u )
                return std::nullopt;
            if ( m_AccumFB[0]->GetFramebufferWidth() != w || m_AccumFB[0]->GetFramebufferHeight() != h )
            {
                m_AccumFB[0]->Resize( w, h );
                m_AccumFB[1]->Resize( w, h );
                m_SceneColorHistoryFB->Resize( w, h );
                m_History.Invalidate();
                m_SceneColorHistory.Invalidate();
            }
            return TraceTargets{
                 .Trace    = RDG::TextureDesc{ .Size   = { .Width = HalfRes( w ), .Height = HalfRes( h ) },
                                               .Format = ViewTargetFormats::kSSRTrace },
                 .TraceRay = RDG::TextureDesc{ .Size   = { .Width = HalfRes( w ), .Height = HalfRes( h ) },
                                               .Format = ViewTargetFormats::kSSRTraceRay },
                 .TileMask = RDG::TextureDesc{ .Size   = { .Width = TileGrid( w ), .Height = TileGrid( h ) },
                                               .Format = ViewTargetFormats::kSSRTileMask },
            };
        }

        // Pass 1, the "Deferred: SSR" compute node: classify + half-resolution trace, one dispatch (one workgroup
        // per tile), writing @p trace and @p tiles. gbuffer = the camera G-buffer's graph textures (albedo/normal/
        // depth at 0/1/2); sceneHistory = GetSceneColorHistoryImage(), imported (last frame's lit opaque scene);
        // hzb = this frame's closest-depth hierarchy (HZBRenderer); velocity = the view's velocity target
        // (FrameTransients::Velocity, written by the G-buffer pass): where a hit was on last frame's screen.
        // The G-buffer textures the three passes sample, as graph textures: albedo, normal, depth (world positions
        // are reconstructed from it, Common/ReconstructPosition.glslh).
        using GBufferInputs = std::array<RDG::TextureRef, 3>;

        // The trace dispatch's push-constant block.
        struct TracePush
        {
            glm::vec4 CameraPos; // xyz = camera, w = per-frame seed (ray sequence + 2x2 rotation)
            glm::vec4 Params;    // x = rays per texel, y = HZB iterations per ray, z = intensity, w = thickness
            glm::vec4 Params2;   // x = max ray distance, y = 1 when the scene colour history is last frame's
        };

        // The trace's uniform block (SSR.shader SSRTraceUB, std140: two mat4), from the ViewFrame once per view
        // frame on the CPU - never inverted per workgroup in the shader. InvJitteredViewProjection keeps the name
        // every reconstructing reader uses (SSRReflectionResolveUB, DeferredUB) so the camera block can take it over.
        // Last frame's screen position of a hit is the velocity target's, not a matrix's (SSR.shader).
        struct TraceUniforms
        {
            glm::mat4 InvJitteredViewProjection;
            glm::mat4 JitteredViewProjection;
        };

        // GRAPH BUILD (before "Deferred: SSR"): this frame's TraceUniforms as a graph buffer, uploaded by the
        // graph's upload pass (Builder::QueueBufferUpload) so the trace is ordered after it.
        // The two are ViewFrame's InvJitteredViewProjection and JitteredViewProjection.
        static RDG::BufferRef UploadTraceUniforms( RDG::Builder& graph, const glm::mat4& invJitteredViewProjection,
                                                   const glm::mat4& jitteredViewProjection )
        {
            const TraceUniforms  uniforms{ invJitteredViewProjection, jitteredViewProjection };
            const RDG::BufferRef buffer =
                 graph.CreateBuffer( RDG::BufferDesc{ sizeof( TraceUniforms ) }, "SSR.TraceUB" );
            graph.QueueBufferUpload( buffer, std::as_bytes( std::span<const TraceUniforms>( &uniforms, 1 ) ) );
            return buffer;
        }

        // SETUP of the "Deferred: SSR" compute node: declares its one block (block 0) - the G-buffer normal/depth,
        // @p sceneHistory and @p hzb (all mips, point) sampled with the sampler the pipeline-setter route sampled
        // them with (the images' own: linear, REPEAT; the deferred composite reads the same G-buffer with it), @p
        // trace / @p tiles as storage writes, @p uniforms (UploadTraceUniforms) as SSRTraceUB, the TracePush
        // bytes; the pipeline's own setters are the other route. Not prepared: nothing.
        void DeclareTraceBindings( RDG::PassBuilder& pass, RDG::TextureRef trace, RDG::TextureRef traceRay,
                                   RDG::TextureRef tiles, const GBufferInputs& gbuffer, RDG::TextureRef velocity,
                                   RDG::TextureRef sceneHistory, RDG::TextureRef hzb,
                                   RDG::BufferRef uniforms ) const
        {
            if ( !m_TracePipeline )
                return;
            pass.Bindings( m_TraceLayout.Get( m_TraceShader ), Renderer::GetPipelineRouteFill( *m_TracePipeline ) )
                 .Sampled( "u_GBufferNormal", gbuffer[1], RDG::Access::SampledCompute,
                           RDG::SubresourceRange::All(), RDG::SamplerDesc::LinearRepeat() )
                 .Sampled( "u_GBufferDepth", gbuffer[2], RDG::Access::SampledCompute, RDG::SubresourceRange::All(),
                           RDG::SamplerDesc::PointClamp() )
                 .Sampled( "u_SceneColor", sceneHistory, RDG::Access::SampledCompute, RDG::SubresourceRange::All(),
                           RDG::SamplerDesc::LinearClamp() )
                 .Sampled( "u_HZB", hzb, RDG::Access::SampledCompute, RDG::SubresourceRange::All(),
                           RDG::SamplerDesc::PointClamp() )
                 .Sampled( "u_Velocity", velocity, RDG::Access::SampledCompute, RDG::SubresourceRange::All(),
                           RDG::SamplerDesc::PointClamp() )
                 .Storage( "u_Trace", trace, RDG::Access::StorageWrite )
                 .Storage( "u_TraceRay", traceRay, RDG::Access::StorageWrite )
                 .Storage( "u_TileMask", tiles, RDG::Access::StorageWrite )
                 .Uniform( "SSRTraceUB", uniforms )
                 .PushConstantBytes( static_cast<uint32_t>( sizeof( TracePush ) ) );
        }

        // EXEC of the same node: the block DeclareTraceBindings declared + this frame's push constants.
        // maxDistance and thickness are WORLD distances (a world unit is a centimetre - convert through
        // Common::Units). raysPerPixel / iterations: Reflections.RaysPerPixel / Reflections.MaxSteps.
        // sceneHistoryReadable: SceneColorHistoryReadableIn of this frame (false: the trace answers nothing).
        struct TraceSettings
        {
            int   RaysPerPixel = 1;
            int   Iterations   = 32;
            float MaxDistance  = 0.0f;
            float Intensity    = 1.0f;
            float Thickness    = 0.0f;
        };
        [[nodiscard]] Common::BoolResultStr RecordTrace( const RDG::PassContext& context,
                                                         const glm::vec4& cameraPos, const TraceSettings& settings,
                                                         const bool sceneHistoryReadable )
        {
            // Each pass is timed on its own because EnableSSR's default is a budget decision
            // (SceneSettings.hpp), and the whole-pass line cannot say which part to cut.
            DESERT_PROFILE_PASS( "SSR: Classify + Trace" );
            const TracePush push{
                 glm::vec4( glm::vec3( cameraPos ), static_cast<float>( m_FrameIndex % 1024u ) ),
                 glm::vec4( static_cast<float>( settings.RaysPerPixel ), static_cast<float>( settings.Iterations ),
                            settings.Intensity, settings.Thickness ),
                 glm::vec4( settings.MaxDistance, sceneHistoryReadable ? 1.0f : 0.0f, 0.0f, 0.0f ) };

            RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
            bindings.PushConstants( &push, static_cast<uint32_t>( sizeof( push ) ) );
            return Renderer::GetInstance().DispatchCompute( bindings, *m_TracePipeline, TileGrid( Width() ),
                                                            TileGrid( Height() ), 1 );
        }

        // Pass 2, inside the render pass the graph opens on GetAccumImage() (cleared to 0): spatial (ratio
        // estimator over @p trace and the neighbours' hit points @p traceRay) + temporal resolve over @p history
        // (GetHistoryImage(), imported, read where the reflected point was; @p velocity gives the reflecting
        // surface's own motion), drawn over the tiles @p tiles marks.
        //
        // SETUP of "Deferred: SSRResolve": its one block (block 0) - u_History linear REPEAT; point CLAMP for the
        // G-buffer depth and normal, the velocity, the trace and its rays (read texel by texel: the ratio
        // estimator reuses each neighbour texel's hit point, SSRResolveTiled.shader) and u_SSRTileMask, mip 0 of the
        // transients; the resolve material is the other route.
        void DeclareResolveBindings( RDG::PassBuilder& pass, RDG::TextureRef trace, RDG::TextureRef traceRay,
                                     RDG::TextureRef tiles, RDG::TextureRef history, const GBufferInputs& gbuffer,
                                     RDG::TextureRef velocity ) const
        {
            if ( !m_ResolvePipeline || !m_ResolveMaterial )
                return;
            pass.Bindings( m_ResolveLayout.Get( m_ResolveShader ),
                           m_ResolveMaterial->GetMaterialExecutor()->GetRouteFill() )
                 .Sampled( "u_History", history, RDG::Access::SampledGraphics, RDG::SubresourceRange::All(),
                           RDG::SamplerDesc::LinearRepeat() )
                 .Sampled( "u_GBufferDepth", gbuffer[2], RDG::Access::SampledGraphics,
                           RDG::SubresourceRange::All(), RDG::SamplerDesc::PointClamp() )
                 .Sampled( "u_GBufferNormal", gbuffer[1], RDG::Access::SampledGraphics,
                           RDG::SubresourceRange::All(), RDG::SamplerDesc::PointClamp() )
                 .Sampled( "u_Velocity", velocity, RDG::Access::SampledGraphics, RDG::SubresourceRange::All(),
                           RDG::SamplerDesc::PointClamp() )
                 .Sampled( "u_Trace", trace, RDG::Access::SampledGraphics, RDG::SubresourceRange::Mip( 0 ),
                           RDG::SamplerDesc::PointClamp() )
                 .Sampled( "u_TraceRay", traceRay, RDG::Access::SampledGraphics, RDG::SubresourceRange::Mip( 0 ),
                           RDG::SamplerDesc::PointClamp() )
                 .Sampled( "u_SSRTileMask", tiles, RDG::Access::SampledGraphics, RDG::SubresourceRange::Mip( 0 ),
                           RDG::SamplerDesc::PointClamp() );
        }

        // Whether the accumulation target this frame reprojects is the view's previous frame (PassHistory.hpp).
        // Asked while the graph is built, before this frame's composite stamps it.
        [[nodiscard]] bool HistoryReadableIn( const ViewFrame& frame ) const
        {
            return m_History.ReadableIn( frame );
        }

        // EXEC: draws the block DeclareResolveBindings declared. @p viewProjection / @p prevViewProjection are the
        // view's (ViewFrame::ViewProjection / PrevViewProjection, unjittered); @p invJitteredViewProjection is ViewFrame::
        // InvJitteredViewProjection (rebuilds the world positions the reprojection needs from the G-buffer depth);
        // @p historyReadable is HistoryReadableIn of this frame. Advances the ping-pong only when the draw was
        // recorded, and stamps the history with @p viewFrameIndex (the ViewFrame this graph was built for).
        [[nodiscard]] Common::BoolResultStr RecordResolve( const RDG::PassContext& context,
                                                           const glm::mat4&        viewProjection,
                                                           const glm::mat4&        prevViewProjection,
                                                           const glm::mat4&        invJitteredViewProjection,
                                                           const bool              historyReadable,
                                                           const uint64_t          viewFrameIndex )
        {
            DESERT_PROFILE_PASS( "SSR: Resolve" );
            m_ResolveMaterial->BindValues( viewProjection, prevViewProjection, invJitteredViewProjection, Texel(),
                                           historyReadable ? 0.88f : 0.0f );
            const RDG::PassBindings     bindings( context, context.GetBindingBlock( 0 ) );
            const Common::BoolResultStr drawn = Renderer::GetInstance().DrawProcedural(
                 bindings, *m_ResolvePipeline, m_ResolveMaterial->GetMaterialExecutor(), TileVertices(), 1u );
            if ( !drawn.IsSuccess() )
                return drawn;

            m_History.Stamp( viewFrameIndex );
            m_AccumIndex = 1u - m_AccumIndex;
            ++m_FrameIndex;
            return drawn;
        }

        // The scene colour history: the image this frame's scene copy writes (SceneRenderer::AddFrameSceneCopy)
        // and next frame's trace reads. Whether it holds the view's previous frame (PassHistory.hpp), asked while
        // the graph is built; stamped by the copy's exec (StampSceneColorHistory) once it was recorded.
        [[nodiscard]] std::shared_ptr<Image2D> GetSceneColorHistoryImage() const
        {
            return m_SceneColorHistoryFB ? m_SceneColorHistoryFB->GetColorAttachmentImage( 0 ) : nullptr;
        }
        [[nodiscard]] bool SceneColorHistoryReadableIn( const ViewFrame& frame ) const
        {
            return m_SceneColorHistory.ReadableIn( frame );
        }
        void StampSceneColorHistory( const uint64_t viewFrameIndex )
        {
            m_SceneColorHistory.Stamp( viewFrameIndex );
        }

        // Before RecordResolve: the target the resolve writes this frame (the lighting reads it) and the one it
        // reprojects.
        [[nodiscard]] std::shared_ptr<Image2D> GetAccumImage() const
        {
            return m_AccumFB[m_AccumIndex] ? m_AccumFB[m_AccumIndex]->GetColorAttachmentImage( 0 ) : nullptr;
        }
        [[nodiscard]] std::shared_ptr<Image2D> GetHistoryImage() const
        {
            const uint32_t prv = 1u - m_AccumIndex;
            return m_AccumFB[prv] ? m_AccumFB[prv]->GetColorAttachmentImage( 0 ) : nullptr;
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
        [[nodiscard]] uint32_t Width() const
        {
            return m_TargetFramebuffer.lock()->GetFramebufferWidth();
        }
        [[nodiscard]] uint32_t Height() const
        {
            return m_TargetFramebuffer.lock()->GetFramebufferHeight();
        }
        [[nodiscard]] glm::vec2 Texel() const
        {
            return { 1.0f / static_cast<float>( Width() ), 1.0f / static_cast<float>( Height() ) };
        }
        // Six vertices per tile; SSRTiles.glslh collapses the unmarked ones.
        [[nodiscard]] uint32_t TileVertices() const
        {
            return TileGrid( Width() ) * TileGrid( Height() ) * 6u;
        }
        static uint32_t HalfRes( uint32_t pixels )
        {
            return ( pixels + 1u ) / 2u;
        }

        std::shared_ptr<Shader>               m_TraceShader;
        std::shared_ptr<Shader>               m_ResolveShader;
        // The two block layouts, derived from each shader's reflection once per compile (not per frame).
        mutable ShaderBindingLayoutCache      m_TraceLayout;
        mutable ShaderBindingLayoutCache      m_ResolveLayout;
        std::shared_ptr<ComputePipeline>      m_TracePipeline;
        std::shared_ptr<GraphicsPipeline>     m_ResolvePipeline;
        std::unique_ptr<MaterialSSRReflectionResolve> m_ResolveMaterial;
        std::shared_ptr<Framebuffer>          m_AccumFB[2];

        std::shared_ptr<Framebuffer> m_SceneColorHistoryFB;

        PassHistoryStamp m_History;           // which view frame wrote GetHistoryImage()
        PassHistoryStamp m_SceneColorHistory; // which view frame wrote GetSceneColorHistoryImage()
        uint32_t         m_AccumIndex = 0;
        uint32_t         m_FrameIndex = 0;
    };
} // namespace Desert::Graphic::System
