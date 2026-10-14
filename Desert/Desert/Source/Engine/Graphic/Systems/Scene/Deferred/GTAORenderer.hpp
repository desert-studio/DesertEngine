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
    // GROUND-TRUTH AMBIENT OCCLUSION with a bent normal (Jimenez 2016; UE PostProcessAmbientOcclusion GTAO):
    //  1) HORIZON SEARCH + INTEGRATION (GTAO.shader, compute, full resolution): kSlices slices per pixel, Steps
    //     depth taps per side of each, the visible arc integrated against the projected normal and its bisector
    //     accumulated into the bent normal. Slices and step offsets rotate every frame.
    //  2) DENOISE (ScreenSpaceDenoiser::DenoiseAmbientOcclusion): the diffuse-shaped reconstruction, temporal
    //     accumulation through the surface, history convolution - where UE runs GTAO's own spatial and temporal
    //     filters, this engine has one denoiser for every screen signal.
    // The deferred lighting multiplies its ambient by the visibility and reads the sky's irradiance along the bent
    // normal (DeferredLighting.shader u_SSAO). Deferred only; the target framebuffer is the scene target (its
    // size).
    class GTAORenderer final : public RenderSystem
    {
    public:
        using RenderSystem::RenderSystem;

        // Slices per pixel per frame: UE's r.GTAO.NumAngles default.
        static constexpr int kSlices = 2;
        // UE's r.GTAO.ThicknessBlend default: how much of the best horizon a nearer, lower sample keeps (thin
        // occluders - a pole in front of a wall - do not darken the whole slice).
        static constexpr float kThicknessBlend = 0.5f;

        Common::BoolResultStr Initialize() override
        {
            m_Shader = Runtime::ResourceRegistry::GetShaderService()->GetByName( "GTAO" );
            if ( !m_Shader )
                return Common::MakeError( "GTAO shader not found (GTAO)" );
            const auto pipeline = ComputePipeline::Create( { .Shader = m_Shader, .DebugName = "GTAO" } );
            if ( !pipeline )
                return Common::MakeError( pipeline.GetError() );
            m_Pipeline = pipeline.GetValue();
            return m_Denoiser.Initialize();
        }

        void OnSceneReplaced() override
        {
            m_Denoiser.InvalidateHistory();
        }
        void OnTemporalHistoryReset() override
        {
            m_FrameIndex = 0;
            m_Denoiser.InvalidateHistory();
        }

        // The push-constant block (GTAO.shader PushConstants) and the uniform block (GTAOUB, std140: two mat4).
        struct Push
        {
            glm::vec4 CameraPos; // xyz = camera, w = per-frame seed
            glm::vec4 Params;    // x = slices, y = steps per side, z = world radius, w = thickness blend
        };
        struct Uniforms
        {
            glm::mat4 InvJitteredViewProjection;
            glm::mat4 JitteredViewProjection;
        };

        // GRAPH BUILD: "Deferred: GTAO" into a transient (kGTAO, the view's size), then the denoiser. Returns the
        // denoised bent normal + visibility (a transient of @p graph); invalid: not initialised. steps =
        // PostProcess.AmbientOcclusionSamples (taps per side of a slice); radius a WORLD distance (centimetres).
        [[nodiscard]] RDG::TextureRef AddPasses( RDG::Builder& graph, const DenoiserSceneTextures& scene,
                                                 const ViewFrame& frame, const int steps, const float radius )
        {
            const auto& target = m_TargetFramebuffer.lock();
            if ( !target || !m_Pipeline )
                return {};
            const uint32_t width  = target->GetFramebufferWidth();
            const uint32_t height = target->GetFramebufferHeight();
            if ( width == 0u || height == 0u )
                return {};
            const RDG::TextureDesc desc{ .Size   = { .Width = width, .Height = height },
                                         .Format = ViewTargetFormats::kGTAO };
            const RDG::TextureRef  raw = graph.CreateTexture( desc, "GTAO" );
            // A lost estimate is "no occlusion" (RDG-FAULT1).
            graph.SetFaultDefault( raw, RDG::FaultDefault::White );

            const Uniforms       uniforms{ frame.InvJitteredViewProjection, frame.JitteredViewProjection };
            const RDG::BufferRef uniformBuffer =
                 graph.CreateBuffer( RDG::BufferDesc{ sizeof( Uniforms ) }, "GTAO.UB" );
            graph.QueueBufferUpload( uniformBuffer, std::as_bytes( std::span<const Uniforms>( &uniforms, 1 ) ) );
            const Push push{ glm::vec4( frame.CameraPosition, static_cast<float>( m_FrameIndex++ % 1024u ) ),
                             glm::vec4( static_cast<float>( kSlices ), static_cast<float>( steps ), radius,
                                        kThicknessBlend ) };
            graph.AddPass(
                 "Deferred: GTAO", RDG::PassFlags::Compute,
                 [&]( RDG::PassBuilder& pass )
                 {
                     pass.Bindings( m_Layout.Get( m_Shader ), Renderer::GetPipelineRouteFill( *m_Pipeline ) )
                          .Sampled( "u_GBufferNormal", scene.GBufferNormal, RDG::Access::SampledCompute,
                                    RDG::SubresourceRange::All(), RDG::SamplerDesc::PointClamp() )
                          .Sampled( "u_GBufferDepth", scene.GBufferDepth, RDG::Access::SampledCompute,
                                    RDG::SubresourceRange::All(), RDG::SamplerDesc::PointClamp() )
                          .Storage( "u_AO", raw, RDG::Access::StorageWrite )
                          .Uniform( "GTAOUB", uniformBuffer )
                          .PushConstantBytes( static_cast<uint32_t>( sizeof( Push ) ) );
                 },
                 [this, push, width, height]( RDG::PassContext& context ) -> Common::BoolResultStr
                 {
                     DESERT_PROFILE_PASS( "GTAO: Horizon Search" );
                     RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                     bindings.PushConstants( &push, static_cast<uint32_t>( sizeof( push ) ) );
                     return Renderer::GetInstance().DispatchCompute( bindings, *m_Pipeline, ( width + 7u ) / 8u,
                                                                     ( height + 7u ) / 8u, 1 );
                 } );
            return m_Denoiser.DenoiseAmbientOcclusion( graph, AmbientOcclusionDenoiserInputs{ .Signal = raw },
                                                       scene, frame, width, height );
        }

    private:
        std::shared_ptr<Shader>          m_Shader;
        std::shared_ptr<ComputePipeline> m_Pipeline;
        mutable ShaderBindingLayoutCache m_Layout;
        ScreenSpaceDenoiser              m_Denoiser{ DenoiserSignal::AmbientOcclusion };
        uint32_t                         m_FrameIndex = 0;
    };
} // namespace Desert::Graphic::System
