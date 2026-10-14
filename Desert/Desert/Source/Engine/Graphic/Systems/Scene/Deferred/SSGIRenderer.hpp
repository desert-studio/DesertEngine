#pragma once

#include <Common/Core/Profiler.hpp>
#include <Engine/Graphic/ViewTargetFormats.hpp>
#include <Engine/Graphic/Systems/RenderSystem.hpp>

#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/ShaderBindingLayoutCache.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/Systems/Scene/Deferred/ScreenSpaceDenoiser.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <glm/glm.hpp>

#include <cstdint>
#include <span>

namespace Desert::Graphic::System
{
    // SCREEN-SPACE GLOBAL ILLUMINATION (UE ScreenSpaceRayTracing.cpp RenderScreenSpaceDiffuseIndirect): the
    // one-bounce diffuse light of EVERYTHING lit on screen.
    //  1) TRACE (SSGI.shader, compute, full resolution): per pixel kRaysPerPixel cosine-distributed rays walked
    //  over
    //     the view's closest-depth HZB (HZBRenderer); a hit returns the previous frame's lit scene where the hit
    //     point was (the view's scene colour history, SceneColorHistoryRenderer, read through the hit pixel's
    //     velocity). Every light that lit that surface - sun, point and spot lights with their shadows, emissive,
    //     last frame's own bounce - comes back with it; no light is evaluated here.
    //  2) DENOISE (ScreenSpaceDenoiser::DenoiseDiffuseIndirect): bilateral reconstruction, temporal accumulation
    //     through the surface with variance rejection, history convolution.
    // The deferred lighting reads the result as its `indirect` (DeferredLighting.shader u_GI). Runs after the
    // G-buffer and the HZB, before the lighting; the target framebuffer is the scene target (its size only).
    class SSGIRenderer final : public RenderSystem
    {
    public:
        using RenderSystem::RenderSystem;

        // Rays per pixel per frame. UE's default SSGI quality (r.SSGI.Quality 3) casts 16 rays per texel of a
        // half-resolution trace: 4 per full-resolution pixel, which is what this full-resolution trace casts.
        static constexpr int kRaysPerPixel = 4;

        Common::BoolResultStr Initialize() override
        {
            m_Shader = Runtime::ResourceRegistry::GetShaderService()->GetByName( "SSGI" );
            if ( !m_Shader )
                return Common::MakeError( "SSGI shader not found (SSGI)" );
            const auto pipeline = ComputePipeline::Create( { .Shader = m_Shader, .DebugName = "SSGITrace" } );
            if ( !pipeline )
                return Common::MakeError( pipeline.GetError() );
            m_Pipeline = pipeline.GetValue();
            return m_Denoiser.Initialize();
        }

        // The denoiser's history is a reprojection of the previous frame — IRenderSystem::OnSceneReplaced, kind 1.
        void OnSceneReplaced() override
        {
            m_Denoiser.InvalidateHistory();
        }
        // Camera cut — IRenderSystem::OnTemporalHistoryReset: the ray sequence restarts with the history.
        void OnTemporalHistoryReset() override
        {
            m_FrameIndex = 0;
            m_Denoiser.InvalidateHistory();
        }

        // The graph textures the trace samples, by shader name.
        struct TraceInputs
        {
            RDG::TextureRef GBufferNormal;
            RDG::TextureRef GBufferDepth;
            RDG::TextureRef SceneColor; // the view's scene colour history (last frame's lit scene), imported
            RDG::TextureRef HZB;        // this frame's closest-depth hierarchy
            RDG::TextureRef Velocity;   // FrameTransients::Velocity
        };

        // The trace's push-constant block (SSGI.shader PushConstants).
        struct TracePush
        {
            glm::vec4 CameraPos; // xyz = camera, w = per-frame seed
            glm::vec4 Params;    // x = rays per pixel, y = HZB iterations per ray, z = GI intensity, w = thickness
            glm::vec4 Params2;   // x = 1 when the scene colour history is last frame's
        };
        // The trace's uniform block (SSGI.shader SSGITraceUB, std140: two mat4).
        struct TraceUniforms
        {
            glm::mat4 InvJitteredViewProjection;
            glm::mat4 JitteredViewProjection;
        };

        // GRAPH BUILD: the trace ("Deferred: SSGI") into a new transient @p signal (kGIResolve, the view's size),
        // then the denoiser. Returns the denoised indirect light (a transient of @p graph); invalid: not
        // initialised. iterations = Reflections.MaxSteps (the HZB walk's budget, shared with SSR); intensity = the
        // view's GI intensity; thickness a WORLD distance (centimetres).
        [[nodiscard]] RDG::TextureRef AddPasses( RDG::Builder& graph, const TraceInputs& in,
                                                 const ViewFrame& frame, const RDG::TextureRef black,
                                                 const bool sceneColorReadable, const int iterations,
                                                 const float intensity, const float thickness,
                                                 RDG::TextureRef& rawSignal )
        {
            const auto& target = m_TargetFramebuffer.lock();
            if ( !target || !m_Pipeline )
                return {};
            const uint32_t width  = target->GetFramebufferWidth();
            const uint32_t height = target->GetFramebufferHeight();
            if ( width == 0u || height == 0u )
                return {};
            const RDG::TextureDesc desc{ .Size   = { .Width = width, .Height = height },
                                         .Format = ViewTargetFormats::kGIResolve };
            const RDG::TextureRef  signal = graph.CreateTexture( desc, "SSGI.Trace" );
            // A lost trace bounces nothing (RDG-FAULT1).
            graph.SetFaultDefault( signal, RDG::FaultDefault::Black );
            rawSignal = signal;

            const TraceUniforms  uniforms{ frame.InvJitteredViewProjection, frame.JitteredViewProjection };
            const RDG::BufferRef uniformBuffer =
                 graph.CreateBuffer( RDG::BufferDesc{ sizeof( TraceUniforms ) }, "SSGI.TraceUB" );
            graph.QueueBufferUpload( uniformBuffer,
                                     std::as_bytes( std::span<const TraceUniforms>( &uniforms, 1 ) ) );
            const TracePush push{ glm::vec4( frame.CameraPosition, static_cast<float>( m_FrameIndex++ % 1024u ) ),
                                  glm::vec4( static_cast<float>( kRaysPerPixel ), static_cast<float>( iterations ),
                                             intensity, thickness ),
                                  glm::vec4( sceneColorReadable ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f ) };
            graph.AddPass(
                 "Deferred: SSGI", RDG::PassFlags::Compute,
                 [&]( RDG::PassBuilder& pass )
                 {
                     pass.Bindings( m_Layout.Get( m_Shader ), Renderer::GetPipelineRouteFill( *m_Pipeline ) )
                          .Sampled( "u_GBufferNormal", in.GBufferNormal, RDG::Access::SampledCompute,
                                    RDG::SubresourceRange::All(), RDG::SamplerDesc::PointClamp() )
                          .Sampled( "u_GBufferDepth", in.GBufferDepth, RDG::Access::SampledCompute,
                                    RDG::SubresourceRange::All(), RDG::SamplerDesc::PointClamp() )
                          .Sampled( "u_SceneColor", in.SceneColor, RDG::Access::SampledCompute,
                                    RDG::SubresourceRange::All(), RDG::SamplerDesc::LinearClamp() )
                          .Sampled( "u_HZB", in.HZB, RDG::Access::SampledCompute, RDG::SubresourceRange::All(),
                                    RDG::SamplerDesc::PointClamp() )
                          .Sampled( "u_Velocity", in.Velocity, RDG::Access::SampledCompute,
                                    RDG::SubresourceRange::All(), RDG::SamplerDesc::PointClamp() )
                          .Storage( "u_Signal", signal, RDG::Access::StorageWrite )
                          .Uniform( "SSGITraceUB", uniformBuffer )
                          .PushConstantBytes( static_cast<uint32_t>( sizeof( TracePush ) ) );
                 },
                 [this, push, width, height]( RDG::PassContext& context ) -> Common::BoolResultStr
                 {
                     DESERT_PROFILE_PASS( "SSGI: Trace" );
                     RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                     bindings.PushConstants( &push, static_cast<uint32_t>( sizeof( push ) ) );
                     return Renderer::GetInstance().DispatchCompute( bindings, *m_Pipeline, ( width + 7u ) / 8u,
                                                                     ( height + 7u ) / 8u, 1 );
                 } );
            return m_Denoiser.DenoiseDiffuseIndirect( graph, DiffuseIndirectDenoiserInputs{ .Signal = signal },
                                                      DenoiserSceneTextures{ .GBufferNormal = in.GBufferNormal,
                                                                             .GBufferDepth  = in.GBufferDepth,
                                                                             .Velocity      = in.Velocity,
                                                                             .Black         = black },
                                                      frame, width, height );
        }

    private:
        std::shared_ptr<Shader>          m_Shader;
        std::shared_ptr<ComputePipeline> m_Pipeline;
        // The trace's block layout, derived from the shader's reflection once per compile (not per frame).
        mutable ShaderBindingLayoutCache m_Layout;
        ScreenSpaceDenoiser              m_Denoiser{ DenoiserSignal::DiffuseIndirect };
        uint32_t                         m_FrameIndex = 0;
    };
} // namespace Desert::Graphic::System
