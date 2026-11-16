#pragma once

#include <Common/Core/Profiler.hpp>
#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/ShaderBindingLayoutCache.hpp>
#include <Engine/Graphic/ViewTargetFormats.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/Systems/Scene/Deferred/ScreenSpaceDenoiser.hpp>
#include <Engine/Graphic/View/ViewFrame.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <span>

namespace Desert::Graphic::System
{
    // The slot names the trace binds, spelled once (RDGShaderParameters.hpp).
    struct SSGIShaderParameters
    {
        DESERT_SHADER_PARAMETER( Sampler, PointClampSampler );
        DESERT_SHADER_PARAMETER( Sampler, LinearClampSampler );
        DESERT_SHADER_PARAMETER( Texture, GBufferNormalTexture );
        DESERT_SHADER_PARAMETER( Texture, GBufferDepthTexture );
        DESERT_SHADER_PARAMETER( Texture, SceneColorHistoryTexture );
        DESERT_SHADER_PARAMETER( Texture, HzbTexture );
        DESERT_SHADER_PARAMETER( Texture, VelocityTexture );
        DESERT_SHADER_PARAMETER( StorageTexture, SsgiSignalTexture );
        DESERT_SHADER_PARAMETER( UniformBuffer, SSGIUB );
    };

    // GIMethod::ScreenSpace (UE ScreenSpaceRayTracing.cpp RenderScreenSpaceDiffuseIndirect): the screen-space
    // diffuse indirect of a deferred view. Two stages per frame:
    //  1) "SSGI: Trace" (SSGI.shader) - per pixel, cosine-distributed rays walked over this frame's closest-depth
    //     HZB, each hit returning LAST frame's lit scene (the view's scene colour history) where the hit was on
    //     that frame's screen; rgb = the cosine-lobe mean radiance, a = coverage.
    //  2) the screen-space denoiser's diffuse-indirect signal (ScreenSpaceDenoiser::DenoiseDiffuseIndirect, UE
    //     IScreenSpaceDenoiser::DenoiseDiffuseIndirect): reconstruction, temporal accumulation, history
    //     convolution - its history is this object's, per view.
    // The output is the frame's FrameTransients::GIDiffuse, which the composite reads as GI mode 1 (the ambient's
    // `indirect`, DeferredLighting.shader). UE composites its SSGI before TAA the same way.
    class SSGIRenderer
    {
    public:
        // The trace's quality (Scalability GlobalIllumination.ScreenSpace*, UE r.SSGI.Quality's ray counts).
        struct Settings
        {
            uint32_t RaysPerPixel   = 8;     // GlobalIllumination.ScreenSpaceRays
            uint32_t Iterations     = 32;    // GlobalIllumination.ScreenSpaceSteps: HZB walk iterations per ray
            uint32_t DenoiserRadius = 2;     // GlobalIllumination.ScreenSpaceDenoiserRadius: reconstruction taps
            float    Thickness      = 50.0f; // world cm a hit may lie behind the depth buffer's first layer
        };

        // The frame's inputs, graph textures of the view's frame.
        struct Inputs
        {
            RDG::TextureRef GBufferNormal; // GBufferB: rgb = world normal, a = roughness
            RDG::TextureRef GBufferDepth;  // the G-buffer's device depth
            RDG::TextureRef Velocity;      // FrameTransients::Velocity
            RDG::TextureRef HZB;           // FrameScreenTrace::HZB
            RDG::TextureRef SceneHistory;  // FrameScreenTrace::SceneHistory
            RDG::TextureRef Black;         // the system black texture (the denoiser's missing history)
            bool            SceneHistoryReadable = false; // FrameScreenTrace::SceneHistoryReadable
        };

        // SSGI.shader SSGIUB (std140: two mat4, two vec4).
        struct Uniforms
        {
            glm::mat4 InvJitteredViewProjection;
            glm::mat4 JitteredViewProjection;
            glm::vec4 Trace;   // x = rays, y = iterations, z = thickness (cm), w = seed
            glm::vec4 History; // x = 1 when the scene colour history is last frame's, y = 1 / PrevPreExposure
        };

        // The trace's program and the denoiser's. An error names the program that did not build; the view then
        // refuses GIMethod::ScreenSpace (SceneRenderer::EnsureSSGIResources).
        [[nodiscard]] Common::BoolResultStr Initialize()
        {
            m_Shader = Runtime::ResourceRegistry::GetShaderService()->GetByName( "SSGI" );
            if ( !m_Shader )
                return Common::MakeError( "SSGI shader not found" );
            const auto pipeline = ComputePipeline::Create( { .Shader = m_Shader, .DebugName = "SSGI" } );
            if ( !pipeline )
                return Common::MakeError( pipeline.GetError() );
            m_Pipeline = pipeline.GetValue();
            if ( const Common::BoolResultStr denoiser = m_Denoiser.Initialize(); !denoiser )
                return Common::MakeError( denoiser.GetError() );
            return BOOLSUCCESS;
        }

        // GRAPH BUILD: "SSGI: Trace" over the @p width x @p height G-buffer, then the denoiser. Returns the
        // denoised diffuse indirect (rgb linear radiance, a coverage), a transient of @p graph.
        [[nodiscard]] Common::ResultStr<RDG::TextureRef> AddPasses( RDG::Builder& graph, const Inputs& in,
                                                                    const Settings&  settings,
                                                                    const ViewFrame& frame, const uint32_t width,
                                                                    const uint32_t height )
        {
            if ( !m_Pipeline )
                return Common::MakeError<RDG::TextureRef>( "SSGI: the trace pipeline is not initialised" );
            auto desc = graph.GetTextureDesc( in.GBufferNormal );
            if ( !desc )
                return Common::MakeError<RDG::TextureRef>( desc.GetError() );
            RDG::TextureDesc signalDesc  = desc.GetValue();
            signalDesc.Format            = ViewTargetFormats::kSSGISignal;
            signalDesc.Mips              = 1;
            signalDesc.Layers            = 1;
            signalDesc.Samples           = 1;
            const RDG::TextureRef signal = graph.CreateTexture( signalDesc, "SSGI.Signal" );
            // A lost trace is "no bounce": the composite goes on with the sky's ambient alone (RDG-FAULT1).
            graph.SetFaultDefault( signal, RDG::FaultDefault::Black );

            const float          readable = in.SceneHistoryReadable ? 1.0f : 0.0f;
            const float          invPrev  = frame.PrevPreExposure > 0.0f ? 1.0f / frame.PrevPreExposure : 1.0f;
            const float          seed     = static_cast<float>( frame.FrameIndex % 1024u );
            const Uniforms       data{ .InvJitteredViewProjection = frame.InvJitteredViewProjection,
                                       .JitteredViewProjection    = frame.JitteredViewProjection,
                                       .Trace   = glm::vec4( static_cast<float>( settings.RaysPerPixel ),
                                                             static_cast<float>( settings.Iterations ),
                                                             settings.Thickness,
                                                             seed ),
                                       .History = glm::vec4( readable, invPrev, 0.0f, 0.0f ) };
            const RDG::BufferRef uniforms = graph.CreateBuffer( RDG::BufferDesc{ sizeof( Uniforms ) }, "SSGI.UB" );
            graph.QueueBufferUpload( uniforms, std::as_bytes( std::span<const Uniforms>( &data, 1 ) ) );

            graph.AddPass(
                 "SSGI: Trace", RDG::PassFlags::Compute,
                 [&]( RDG::PassBuilder& pass )
                 {
                     pass.Bindings( m_Layout.Get( m_Shader ), Renderer::GetPipelineRouteFill( *m_Pipeline ) )
                          .Sampler( SSGIShaderParameters::PointClampSampler, RDG::SamplerDesc::PointClamp() )
                          .Sampler( SSGIShaderParameters::LinearClampSampler, RDG::SamplerDesc::LinearClamp() )
                          .Texture( SSGIShaderParameters::GBufferNormalTexture, in.GBufferNormal,
                                    RDG::Access::SampledCompute, RDG::SubresourceRange::All() )
                          .Texture( SSGIShaderParameters::GBufferDepthTexture, in.GBufferDepth,
                                    RDG::Access::SampledCompute, RDG::SubresourceRange::All() )
                          .Texture( SSGIShaderParameters::SceneColorHistoryTexture, in.SceneHistory,
                                    RDG::Access::SampledCompute, RDG::SubresourceRange::All() )
                          .Texture( SSGIShaderParameters::HzbTexture, in.HZB, RDG::Access::SampledCompute,
                                    RDG::SubresourceRange::All() )
                          .Texture( SSGIShaderParameters::VelocityTexture, in.Velocity,
                                    RDG::Access::SampledCompute, RDG::SubresourceRange::All() )
                          .Storage( SSGIShaderParameters::SsgiSignalTexture, signal, RDG::Access::StorageWrite )
                          .Uniform( SSGIShaderParameters::SSGIUB, uniforms );
                 },
                 [this, width, height]( RDG::PassContext& context ) -> Common::BoolResultStr
                 {
                     DESERT_PROFILE_PASS( "SSGI: Trace" );
                     const RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                     return Renderer::GetInstance().DispatchCompute( bindings, *m_Pipeline, GroupCount( width ),
                                                                     GroupCount( height ), 1 );
                 } );

            const DenoiserSceneTextures scene{ .GBufferNormal = in.GBufferNormal,
                                               .GBufferDepth  = in.GBufferDepth,
                                               .Velocity      = in.Velocity,
                                               .Black         = in.Black };
            const RDG::TextureRef       denoised = m_Denoiser.DenoiseDiffuseIndirect(
                 graph,
                 DiffuseIndirectDenoiserInputs{ .Signal               = signal,
                                                      .ReconstructionRadius = settings.DenoiserRadius },
                 scene, frame, width, height );
            if ( !denoised.IsValid() )
                return Common::MakeError<RDG::TextureRef>(
                     "SSGI: the diffuse-indirect denoiser is not initialised" );
            return Common::MakeSuccess( denoised );
        }

    private:
        static uint32_t GroupCount( const uint32_t texels )
        {
            return ( texels + 7u ) / 8u; // SSGI.shader LocalSize(8, 8, 1)
        }

        std::shared_ptr<Shader> m_Shader;
        // The block layout, derived from m_Shader's reflection once per compile (not per frame).
        ShaderBindingLayoutCache         m_Layout;
        std::shared_ptr<ComputePipeline> m_Pipeline;
        ScreenSpaceDenoiser              m_Denoiser{ DenoiserSignal::DiffuseIndirect };
    };
} // namespace Desert::Graphic::System
