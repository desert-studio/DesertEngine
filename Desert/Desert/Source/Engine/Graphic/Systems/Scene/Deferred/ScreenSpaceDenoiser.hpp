#pragma once

#include <Common/Core/Profiler.hpp>
#include <Engine/Graphic/ViewTargetFormats.hpp>

#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/ShaderBindingLayoutCache.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/View/PassHistory.hpp>
#include <Engine/Graphic/View/ViewFrame.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <format>
#include <memory>
#include <span>
#include <string>

namespace Desert::Graphic::System
{
    // Which signal a denoiser instance serves (the shaders' kSSDSignal*, Common/ScreenSpaceDenoise.glslh).
    enum class DenoiserSignal : uint8_t
    {
        Reflections      = 0, // SSR (and, later, ray-traced reflections): specular, lobe-aware
        DiffuseIndirect  = 1, // RSM-GI (and, later, SSGI / ray-traced GI): diffuse, normal-aware
        ShadowVisibility = 2, // a light's penumbra mask (ray-traced sun shadow): penumbra-sized, normal-aware
        DirectSpecular   = 3, // MegaLights' specular direct light: lobe-aware, reprojected through the surface
    };

    // The view's scene textures every pass of the denoiser compares its taps with (graph textures of the frame).
    struct DenoiserSceneTextures
    {
        RDG::TextureRef GBufferNormal; // rgb = world normal, a = roughness
        RDG::TextureRef GBufferDepth;  // device depth: world positions are reconstructed from it
        RDG::TextureRef Velocity;      // the view's velocity (FrameTransients::Velocity)
        RDG::TextureRef Black;         // the system black texture (a history that does not exist yet)
    };

    // What SSR hands the reflections denoiser (UE IScreenSpaceDenoiser::FReflectionsInputs): its half-resolution
    // trace, the trace texels' hit points + pdfs, the tile mask, and the seed of the 2x2 block rotation the trace
    // used this frame (the reconstruction finds each texel's source pixel from it).
    struct ReflectionsDenoiserInputs
    {
        RDG::TextureRef Trace;
        RDG::TextureRef TraceRay;
        RDG::TextureRef TileMask;
        uint32_t        TraceSeed = 0;
        // The reconstruction's kernel, trace texels per side from the centre (Scalability
        // Reflections.DenoiserRadius; UE r.Reflections.Denoiser.ReconstructionSamples). 2 = 5x5.
        uint32_t ReconstructionRadius = 2;
    };

    // What a diffuse-indirect producer hands the denoiser (UE FDiffuseIndirectInputs): its full-resolution
    // estimate, rgb = light, a = the share it answered - RSM-GI's bounce light, MegaLights' demodulated local
    // light.
    struct DiffuseIndirectDenoiserInputs
    {
        RDG::TextureRef Signal;
        // The reconstruction's kernel, taps per side from the centre (UE r.*.Denoiser.ReconstructionSamples;
        // MegaLights: Scalability Shadows.MegaLightsDenoiserRadius). 2 = 5x5.
        uint32_t ReconstructionRadius = 2;
    };

    // What MegaLights hands the denoiser's specular signal (UE MegaLights denoises its specular lighting apart
    // from the diffuse): its full-resolution demodulated specular light, rgb = light, a = 1 on a surface. The
    // diffuse half goes through DiffuseIndirectDenoiserInputs, so a glossy highlight never gets the diffuse
    // kernel.
    struct DirectSpecularDenoiserInputs
    {
        RDG::TextureRef Signal;
        // The reconstruction's kernel, taps per side from the centre (Shadows.MegaLightsDenoiserRadius), shrunk
        // further by each pixel's specular lobe (SSDReconstructDirectSpecular.shader). 2 = 5x5.
        uint32_t ReconstructionRadius = 2;
    };

    // UE FShadowVisibilityInputs: a penumbra mask and the closest occluder distance, whatever produced them (no
    // ray-tracing type here). Mask: r = visibility estimate (0 shadowed .. 1 lit); ClosestOccluder: r = the
    // distance to the closest occluder hit in cm, <= 0 where nothing was hit. The light's angular diameter sizes
    // the penumbra the reconstruction blurs over (occluder distance * tan(angle / 2)).
    struct ShadowVisibilityDenoiserInputs
    {
        RDG::TextureRef Mask;
        RDG::TextureRef ClosestOccluder;
        float           LightSourceAngleRadians = 0.0f;
        // The reconstruction's kernel, taps per side from the centre, spread over the penumbra (Scalability
        // Shadows.RayTracedDenoiserRadius; UE r.Shadow.Denoiser.ReconstructionSamples). 2 = 5x5.
        uint32_t ReconstructionRadius = 2;
    };

    // THE SCREEN-SPACE DENOISER (UE ScreenSpaceDenoiser.cpp / ScreenSpaceDenoise/*.usf): one denoiser for every
    // noisy screen-space signal - SSR's reflections and RSM-GI's diffuse light now, ray-traced reflections and GI
    // through the same two entry points later. Per frame, three compute passes:
    //  1) RECONSTRUCTION - per signal (SSDReconstructReflections / SSDReconstructDiffuse): the spatial
    //     reconstruction of the producer's raw samples at full resolution, every tap weighed by the bilateral
    //     weight of its surface against the pixel's (depth plane, normal, specular lobe).
    //  2) TEMPORAL ACCUMULATION (SSDTemporalAccumulation): the history reprojected (reflections through the
    //     reflected point), rejected against the neighbourhood's variance, accumulated by sample count.
    //  3) HISTORY CONVOLUTION (SSDHistoryConvolution): a bilateral post filter whose radius shrinks with the
    //     pixel's sample count - the output.
    // The history (accumulation + sample count) is this object's, per view (each SceneRenderer's producer owns
    // one denoiser): two slots, one read and one written per frame, each a graph texture created once and
    // extracted (UE: the view state's FScreenSpaceDenoiserHistory), re-created when the view resizes.
    class ScreenSpaceDenoiser
    {
    public:
        explicit ScreenSpaceDenoiser( const DenoiserSignal signal ) : m_Signal( signal )
        {
        }

        [[nodiscard]] Common::BoolResultStr Initialize()
        {
            const auto        shaders         = Runtime::ResourceRegistry::GetShaderService();
            const std::string reconstructName = ReconstructShaderName( m_Signal );
            m_ReconstructShader               = shaders->GetByName( reconstructName );
            if ( !m_ReconstructShader )
                return Common::MakeError( std::format( "Denoiser: shader {} not found", reconstructName ) );
            const ComputePipelineSpecification reconstructSpec{ .Shader    = m_ReconstructShader,
                                                                .DebugName = reconstructName };
            const auto                         reconstruct = ComputePipeline::Create( reconstructSpec );
            if ( !reconstruct )
                return Common::MakeError( reconstruct.GetError() );
            m_ReconstructPipeline = reconstruct.GetValue();

            m_TemporalShader = shaders->GetByName( "SSDTemporalAccumulation" );
            if ( !m_TemporalShader )
                return Common::MakeError( "Denoiser: shader SSDTemporalAccumulation not found" );
            const ComputePipelineSpecification temporalSpec{ .Shader    = m_TemporalShader,
                                                             .DebugName = "SSDTemporalAccumulation" };
            const auto                         temporal = ComputePipeline::Create( temporalSpec );
            if ( !temporal )
                return Common::MakeError( temporal.GetError() );
            m_TemporalPipeline = temporal.GetValue();

            m_ConvolutionShader = shaders->GetByName( "SSDHistoryConvolution" );
            if ( !m_ConvolutionShader )
                return Common::MakeError( "Denoiser: shader SSDHistoryConvolution not found" );
            const ComputePipelineSpecification convolutionSpec{ .Shader    = m_ConvolutionShader,
                                                                .DebugName = "SSDHistoryConvolution" };
            const auto                         convolution = ComputePipeline::Create( convolutionSpec );
            if ( !convolution )
                return Common::MakeError( convolution.GetError() );
            m_ConvolutionPipeline = convolution.GetValue();
            return BOOLSUCCESS;
        }

        // The previous frame is a different world (scene replaced) or a different shot (camera cut): nothing
        // in the history may be reprojected into this frame.
        void InvalidateHistory()
        {
            for ( HistorySlot& slot : m_Slots )
                slot.Stamp.Invalidate();
        }

        // UE IScreenSpaceDenoiser::DenoiseReflections. Adds the three passes to @p graph and returns the denoised
        // reflection (full resolution, straight rgb + coverage), a transient of this graph. Invalid: not
        // initialised.
        [[nodiscard]] RDG::TextureRef DenoiseReflections( RDG::Builder& graph, const ReflectionsDenoiserInputs& in,
                                                          const DenoiserSceneTextures& scene,
                                                          const ViewFrame& frame, const uint32_t width,
                                                          const uint32_t height )
        {
            if ( m_Signal != DenoiserSignal::Reflections || !m_ReconstructPipeline )
                return {};
            const RDG::BufferRef  uniforms = UploadViewUniforms( graph, frame );
            const RDG::TextureRef signal =
                 graph.CreateTexture( Desc( width, height, kSignalFormat ), "SSD.Signal" );
            graph.SetFaultDefault( signal, RDG::FaultDefault::Black );
            graph.AddPass(
                 "Denoise: Reflections Reconstruction", RDG::PassFlags::Compute,
                 [&]( RDG::PassBuilder& pass )
                 {
                     pass.Bindings( m_ReconstructLayout.Get( m_ReconstructShader ),
                                    Renderer::GetPipelineRouteFill( *m_ReconstructPipeline ) )
                          .Texture( "u_GBufferNormal", scene.GBufferNormal, RDG::Access::SampledCompute,
                                    RDG::SubresourceRange::All() )
                          .Sampler( "u_PointClampSampler", RDG::SamplerDesc::PointClamp() )
                          .Texture( "u_GBufferDepth", scene.GBufferDepth, RDG::Access::SampledCompute,
                                    RDG::SubresourceRange::All() )
                          .Texture( "u_Trace", in.Trace, RDG::Access::SampledCompute,
                                    RDG::SubresourceRange::Mip( 0 ) )
                          .Texture( "u_TraceRay", in.TraceRay, RDG::Access::SampledCompute,
                                    RDG::SubresourceRange::Mip( 0 ) )
                          .Texture( "u_TileMask", in.TileMask, RDG::Access::SampledCompute,
                                    RDG::SubresourceRange::Mip( 0 ) )
                          .Storage( "u_Signal", signal, RDG::Access::StorageWrite )
                          .Uniform( "DenoiserViewUB", uniforms )
                          .PushConstantBytes( static_cast<uint32_t>( sizeof( glm::vec4 ) ) );
                 },
                 [this, seed = in.TraceSeed, radius = in.ReconstructionRadius, width,
                  height]( RDG::PassContext& context ) -> Common::BoolResultStr
                 {
                     DESERT_PROFILE_PASS( "Denoise: Reflections Reconstruction" );
                     const glm::vec4 push( static_cast<float>( seed ), static_cast<float>( radius ), 0.0f, 0.0f );
                     RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                     bindings.PushConstants( &push, static_cast<uint32_t>( sizeof( push ) ) );
                     return Renderer::GetInstance().DispatchCompute( bindings, *m_ReconstructPipeline,
                                                                     Groups( width ), Groups( height ), 1 );
                 } );
            return AddTemporalAndConvolution( graph, signal, in.Trace, in.TraceRay, scene, frame, uniforms, width,
                                              height );
        }

        // UE IScreenSpaceDenoiser::DenoiseDiffuseIndirect. As DenoiseReflections, for diffuse light (the history
        // is reprojected through the surface itself: no hit distance).
        [[nodiscard]] RDG::TextureRef DenoiseDiffuseIndirect( RDG::Builder&                        graph,
                                                              const DiffuseIndirectDenoiserInputs& in,
                                                              const DenoiserSceneTextures&         scene,
                                                              const ViewFrame& frame, const uint32_t width,
                                                              const uint32_t height )
        {
            if ( m_Signal != DenoiserSignal::DiffuseIndirect || !m_ReconstructPipeline )
                return {};
            return DenoiseSurfaceSignal( graph, in.Signal, in.ReconstructionRadius, scene, frame, width, height,
                                         "Denoise: Diffuse Reconstruction" );
        }

        // As DenoiseDiffuseIndirect, for MegaLights' specular direct light: the reconstruction and the history
        // convolution weigh each tap by the agreement of the specular lobes (UE BILATERAL_TOKOYASHI), the history
        // is reprojected through the surface (a light's highlight moves with it, not with a reflected point).
        [[nodiscard]] RDG::TextureRef DenoiseDirectSpecular( RDG::Builder&                       graph,
                                                             const DirectSpecularDenoiserInputs& in,
                                                             const DenoiserSceneTextures&        scene,
                                                             const ViewFrame& frame, const uint32_t width,
                                                             const uint32_t height )
        {
            if ( m_Signal != DenoiserSignal::DirectSpecular || !m_ReconstructPipeline )
                return {};
            return DenoiseSurfaceSignal( graph, in.Signal, in.ReconstructionRadius, scene, frame, width, height,
                                         "Denoise: Direct Specular Reconstruction" );
        }

    private:
        // The full-resolution surface signals (diffuse indirect, direct specular): one reconstruction pass of this
        // signal's program, then the shared temporal accumulation and history convolution.
        RDG::TextureRef DenoiseSurfaceSignal( RDG::Builder& graph, const RDG::TextureRef input,
                                              const uint32_t               reconstructionRadius,
                                              const DenoiserSceneTextures& scene, const ViewFrame& frame,
                                              const uint32_t width, const uint32_t height,
                                              const std::string_view passName )
        {
            const RDG::BufferRef  uniforms = UploadViewUniforms( graph, frame );
            const RDG::TextureRef signal =
                 graph.CreateTexture( Desc( width, height, kSignalFormat ), "SSD.Signal" );
            graph.SetFaultDefault( signal, RDG::FaultDefault::Black );
            // x = the kernel's taps per side from the centre (SSDReconstructDiffuse /
            // SSDReconstructDirectSpecular).
            const glm::vec4 push( static_cast<float>( std::max( reconstructionRadius, 1u ) ), 0.0f, 0.0f, 0.0f );
            graph.AddPass(
                 passName, RDG::PassFlags::Compute,
                 [&]( RDG::PassBuilder& pass )
                 {
                     pass.Bindings( m_ReconstructLayout.Get( m_ReconstructShader ),
                                    Renderer::GetPipelineRouteFill( *m_ReconstructPipeline ) )
                          .Texture( "u_GBufferNormal", scene.GBufferNormal, RDG::Access::SampledCompute,
                                    RDG::SubresourceRange::All() )
                          .Sampler( "u_PointClampSampler", RDG::SamplerDesc::PointClamp() )
                          .Texture( "u_GBufferDepth", scene.GBufferDepth, RDG::Access::SampledCompute,
                                    RDG::SubresourceRange::All() )
                          .Texture( "u_Input", input, RDG::Access::SampledCompute, RDG::SubresourceRange::All() )
                          .Storage( "u_Signal", signal, RDG::Access::StorageWrite )
                          .Uniform( "DenoiserViewUB", uniforms )
                          .PushConstantBytes( static_cast<uint32_t>( sizeof( glm::vec4 ) ) );
                 },
                 [this, push, width, height]( RDG::PassContext& context ) -> Common::BoolResultStr
                 {
                     DESERT_PROFILE_PASS( "Denoise: Surface Reconstruction" );
                     RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                     bindings.PushConstants( &push, static_cast<uint32_t>( sizeof( push ) ) );
                     return Renderer::GetInstance().DispatchCompute( bindings, *m_ReconstructPipeline,
                                                                     Groups( width ), Groups( height ), 1 );
                 } );
            return AddTemporalAndConvolution( graph, signal, scene.Black, scene.Black, scene, frame, uniforms,
                                              width, height );
        }

    public:
        // UE IScreenSpaceDenoiser::DenoiseShadowVisibilityMasks. As DenoiseDiffuseIndirect, for a light's penumbra
        // mask: the reconstruction's kernel is the penumbra's width (closest occluder * tan(angle / 2)), the
        // history is reprojected through the surface. Returns the denoised visibility (r; rgb equal, a = 1). A
        // light of angle 0 casts a hard shadow from one ray per pixel - nothing stochastic to denoise: the mask
        // itself is returned and no pass is added. Invalid: not initialised for this signal.
        [[nodiscard]] RDG::TextureRef DenoiseShadowVisibility( RDG::Builder&                         graph,
                                                               const ShadowVisibilityDenoiserInputs& in,
                                                               const DenoiserSceneTextures&          scene,
                                                               const ViewFrame& frame, const uint32_t width,
                                                               const uint32_t height )
        {
            if ( m_Signal != DenoiserSignal::ShadowVisibility || !m_ReconstructPipeline )
                return {};
            if ( in.LightSourceAngleRadians <= 0.0f )
                return in.Mask;
            const RDG::BufferRef  uniforms = UploadViewUniforms( graph, frame );
            const RDG::TextureRef signal =
                 graph.CreateTexture( Desc( width, height, kSignalFormat ), "SSD.Signal" );
            graph.SetFaultDefault( signal, RDG::FaultDefault::Black );
            const glm::vec4 push( std::tan( in.LightSourceAngleRadians * 0.5f ),
                                  static_cast<float>( in.ReconstructionRadius ), 0.0f, 0.0f );
            graph.AddPass(
                 "Denoise: Shadow Visibility Reconstruction", RDG::PassFlags::Compute,
                 [&]( RDG::PassBuilder& pass )
                 {
                     pass.Bindings( m_ReconstructLayout.Get( m_ReconstructShader ),
                                    Renderer::GetPipelineRouteFill( *m_ReconstructPipeline ) )
                          .Texture( "u_GBufferNormal", scene.GBufferNormal, RDG::Access::SampledCompute,
                                    RDG::SubresourceRange::All() )
                          .Sampler( "u_PointClampSampler", RDG::SamplerDesc::PointClamp() )
                          .Texture( "u_GBufferDepth", scene.GBufferDepth, RDG::Access::SampledCompute,
                                    RDG::SubresourceRange::All() )
                          .Texture( "u_Mask", in.Mask, RDG::Access::SampledCompute, RDG::SubresourceRange::All() )
                          .Texture( "u_ClosestOccluder", in.ClosestOccluder, RDG::Access::SampledCompute,
                                    RDG::SubresourceRange::All() )
                          .Storage( "u_Signal", signal, RDG::Access::StorageWrite )
                          .Uniform( "DenoiserViewUB", uniforms )
                          .PushConstantBytes( static_cast<uint32_t>( sizeof( glm::vec4 ) ) );
                 },
                 [this, push, width, height]( RDG::PassContext& context ) -> Common::BoolResultStr
                 {
                     DESERT_PROFILE_PASS( "Denoise: Shadow Visibility Reconstruction" );
                     RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                     bindings.PushConstants( &push, static_cast<uint32_t>( sizeof( push ) ) );
                     return Renderer::GetInstance().DispatchCompute( bindings, *m_ReconstructPipeline,
                                                                     Groups( width ), Groups( height ), 1 );
                 } );
            return AddTemporalAndConvolution( graph, signal, scene.Black, scene.Black, scene, frame, uniforms,
                                              width, height );
        }

        // The view-independent constants of a signal (UE's per-signal TARGETED_SAMPLE_COUNT): the most frames a
        // pixel's history may hold, and the fewest a reflection's lobe targets - the trace is half resolution,
        // one pixel of each 2x2 block per frame, so a pixel has its own sample once every four frames.
        static constexpr float kMaxSampleCountReflections     = 16.0f;
        static constexpr float kMaxSampleCountDiffuseIndirect = 32.0f;
        // UE's shadow-visibility history (TARGETED_SAMPLE_COUNT of the shadow mask): a penumbra follows its
        // occluder, so it holds fewer frames than bounce light.
        static constexpr float kMaxSampleCountShadowVisibility = 8.0f;
        static constexpr float kMinSampleCountReflections     = 4.0f;

        // The formats of the denoiser's images (ViewMemory counts them).
        static constexpr Core::Formats::ImageFormat kSignalFormat      = ViewTargetFormats::kDenoiserSignal;
        static constexpr Core::Formats::ImageFormat kSampleCountFormat = ViewTargetFormats::kDenoiserSampleCount;

        // The DenoiserViewUB block every pass declares (std140: three mat4), from the ViewFrame once per frame.
        struct ViewUniforms
        {
            glm::mat4 ViewProjection;
            glm::mat4 PrevViewProjection;
            glm::mat4 InvJitteredViewProjection;
        };

    private:
        // The reconstruction program of a signal (one per signal; the temporal and convolution passes are shared).
        static std::string ReconstructShaderName( const DenoiserSignal signal )
        {
            switch ( signal )
            {
                case DenoiserSignal::Reflections:
                    return "SSDReconstructReflections";
                case DenoiserSignal::DiffuseIndirect:
                    return "SSDReconstructDiffuse";
                case DenoiserSignal::ShadowVisibility:
                    return "SSDReconstructShadowVisibility";
                case DenoiserSignal::DirectSpecular:
                    return "SSDReconstructDirectSpecular";
            }
            return "SSDReconstructDiffuse"; // unreachable: the switch names every signal (-Wswitch)
        }

        // The graph name of a signal's output (its history slots append ".History<slot>").
        static std::string SignalName( const DenoiserSignal signal )
        {
            switch ( signal )
            {
                case DenoiserSignal::Reflections:
                    return "SSD.Reflections";
                case DenoiserSignal::DiffuseIndirect:
                    return "SSD.Diffuse";
                case DenoiserSignal::ShadowVisibility:
                    return "SSD.ShadowVisibility";
                case DenoiserSignal::DirectSpecular:
                    return "SSD.DirectSpecular";
            }
            return "SSD.Diffuse"; // unreachable: the switch names every signal (-Wswitch)
        }

        static float MaxSampleCount( const DenoiserSignal signal )
        {
            switch ( signal )
            {
                case DenoiserSignal::Reflections:
                    return kMaxSampleCountReflections;
                case DenoiserSignal::DiffuseIndirect:
                    return kMaxSampleCountDiffuseIndirect;
                case DenoiserSignal::ShadowVisibility:
                    return kMaxSampleCountShadowVisibility;
                case DenoiserSignal::DirectSpecular:
                    return kMaxSampleCountReflections; // a highlight follows the light as fast as a reflection
            }
            return kMaxSampleCountDiffuseIndirect; // unreachable: the switch names every signal (-Wswitch)
        }

        // One history slot: the premultiplied accumulation and its sample count, each a graph texture created
        // once (CreateTexture + Extract) and registered every frame after (RegisterExternal).
        struct HistorySlot
        {
            RDG::ExternalTexture Accumulation;
            RDG::ExternalTexture SampleCount;
            bool                 Built  = false;
            uint32_t             Width  = 0;
            uint32_t             Height = 0;
            PassHistoryStamp     Stamp; // which view frame wrote it
        };

        static RDG::TextureDesc Desc( const uint32_t width, const uint32_t height,
                                      const Core::Formats::ImageFormat format )
        {
            return RDG::TextureDesc{ .Size = { .Width = width, .Height = height }, .Format = format };
        }
        static uint32_t Groups( const uint32_t pixels )
        {
            return ( pixels + 7u ) / 8u; // LocalSize(8, 8, 1) in every pass
        }

        static RDG::BufferRef UploadViewUniforms( RDG::Builder& graph, const ViewFrame& frame )
        {
            const ViewUniforms   uniforms{ frame.ViewProjection, frame.PrevViewProjection,
                                         frame.InvJitteredViewProjection };
            const RDG::BufferRef buffer =
                 graph.CreateBuffer( RDG::BufferDesc{ sizeof( ViewUniforms ) }, "SSD.ViewUB" );
            graph.QueueBufferUpload( buffer, std::as_bytes( std::span<const ViewUniforms>( &uniforms, 1 ) ) );
            return buffer;
        }

        // The slot's two textures in this graph: registered when they exist at this size, created and extracted
        // (kept past the graph, sampled by next frame's temporal pass) when they do not.
        std::array<RDG::TextureRef, 2> AcquireSlot( RDG::Builder& graph, HistorySlot& slot, const uint32_t width,
                                                    const uint32_t height, const std::string& name )
        {
            if ( slot.Built && slot.Width == width && slot.Height == height )
                return { graph.RegisterExternal( slot.Accumulation, name ),
                         graph.RegisterExternal( slot.SampleCount, name + ".Count" ) };
            slot.Accumulation = {};
            slot.SampleCount  = {};
            slot.Stamp.Invalidate();
            const RDG::TextureRef accumulation = graph.CreateTexture( Desc( width, height, kSignalFormat ), name );
            const RDG::TextureRef count =
                 graph.CreateTexture( Desc( width, height, kSampleCountFormat ), name + ".Count" );
            graph.Extract( accumulation, slot.Accumulation, RDG::Access::SampledCompute );
            graph.Extract( count, slot.SampleCount, RDG::Access::SampledCompute );
            slot.Built  = true;
            slot.Width  = width;
            slot.Height = height;
            return { accumulation, count };
        }

        // @p trace / @p traceRay: the reflections' half-res trace and hit points, from which the temporal pass
        // estimates the distance to the reflected point (diffuse: the black texture - the surface itself).
        RDG::TextureRef AddTemporalAndConvolution( RDG::Builder& graph, const RDG::TextureRef signal,
                                                   const RDG::TextureRef trace, const RDG::TextureRef traceRay,
                                                   const DenoiserSceneTextures& scene, const ViewFrame& frame,
                                                   const RDG::BufferRef uniforms, const uint32_t width,
                                                   const uint32_t height )
        {
            const std::string prefix   = SignalName( m_Signal ) + ".History";
            HistorySlot& previous = m_Slots[m_Parity];
            HistorySlot& next     = m_Slots[1u - m_Parity];
            // Decided at build time: the exec runs after the frame was built and must not consult a stamp a later
            // frame may have written.
            const bool readable = previous.Built && previous.Width == width && previous.Height == height &&
                                  previous.Stamp.ReadableIn( frame );
            std::array<RDG::TextureRef, 2> history = { scene.Black, scene.Black };
            if ( readable )
                history = AcquireSlot( graph, previous, width, height, prefix + std::to_string( m_Parity ) );
            const std::array<RDG::TextureRef, 2> written =
                 AcquireSlot( graph, next, width, height, prefix + std::to_string( 1u - m_Parity ) );
            m_Parity = 1u - m_Parity;

            const bool      reflections = m_Signal == DenoiserSignal::Reflections;
            const glm::vec4 temporalPush( readable ? 1.0f : 0.0f, static_cast<float>( m_Signal ),
                                          MaxSampleCount( m_Signal ),
                                          reflections ? kMinSampleCountReflections : 1.0f );
            HistorySlot*    stamped = &next;
            graph.AddPass(
                 "Denoise: Temporal Accumulation", RDG::PassFlags::Compute,
                 [&]( RDG::PassBuilder& pass )
                 {
                     pass.Bindings( m_TemporalLayout.Get( m_TemporalShader ),
                                    Renderer::GetPipelineRouteFill( *m_TemporalPipeline ) )
                          .Texture( "u_GBufferNormal", scene.GBufferNormal, RDG::Access::SampledCompute,
                                    RDG::SubresourceRange::All() )
                          .Sampler( "u_PointClampSampler", RDG::SamplerDesc::PointClamp() )
                          .Texture( "u_GBufferDepth", scene.GBufferDepth, RDG::Access::SampledCompute,
                                    RDG::SubresourceRange::All() )
                          .Texture( "u_Signal", signal, RDG::Access::SampledCompute, RDG::SubresourceRange::All() )
                          .Texture( "u_Trace", trace, RDG::Access::SampledCompute,
                                    RDG::SubresourceRange::Mip( 0 ) )
                          .Texture( "u_TraceRay", traceRay, RDG::Access::SampledCompute,
                                    RDG::SubresourceRange::Mip( 0 ) )
                          .Texture( "u_History", history[0], RDG::Access::SampledCompute,
                                    RDG::SubresourceRange::All() )
                          .Sampler( "u_LinearClampSampler", RDG::SamplerDesc::LinearClamp() )
                          .Texture( "u_HistoryCount", history[1], RDG::Access::SampledCompute,
                                    RDG::SubresourceRange::All() )
                          .Texture( "u_Velocity", scene.Velocity, RDG::Access::SampledCompute,
                                    RDG::SubresourceRange::All() )
                          .Storage( "u_NewHistory", written[0], RDG::Access::StorageWrite )
                          .Storage( "u_NewHistoryCount", written[1], RDG::Access::StorageWrite )
                          .Uniform( "DenoiserViewUB", uniforms )
                          .PushConstantBytes( static_cast<uint32_t>( sizeof( glm::vec4 ) ) );
                 },
                 [this, temporalPush, stamped, index = frame.FrameIndex, width,
                  height]( RDG::PassContext& context ) -> Common::BoolResultStr
                 {
                     DESERT_PROFILE_PASS( "Denoise: Temporal Accumulation" );
                     RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                     bindings.PushConstants( &temporalPush, static_cast<uint32_t>( sizeof( temporalPush ) ) );
                     const Common::BoolResultStr dispatched = Renderer::GetInstance().DispatchCompute(
                          bindings, *m_TemporalPipeline, Groups( width ), Groups( height ), 1 );
                     if ( dispatched.IsSuccess() )
                         stamped->Stamp.Stamp( index );
                     return dispatched;
                 } );

            const RDG::TextureRef output =
                 graph.CreateTexture( Desc( width, height, kSignalFormat ), SignalName( m_Signal ) );
            graph.SetFaultDefault( output, RDG::FaultDefault::Black );
            const glm::vec4 convolutionPush( 0.0f, static_cast<float>( m_Signal ), 0.0f, 0.0f );
            graph.AddPass(
                 "Denoise: History Convolution", RDG::PassFlags::Compute,
                 [&]( RDG::PassBuilder& pass )
                 {
                     pass.Bindings( m_ConvolutionLayout.Get( m_ConvolutionShader ),
                                    Renderer::GetPipelineRouteFill( *m_ConvolutionPipeline ) )
                          .Texture( "u_GBufferNormal", scene.GBufferNormal, RDG::Access::SampledCompute,
                                    RDG::SubresourceRange::All() )
                          .Sampler( "u_PointClampSampler", RDG::SamplerDesc::PointClamp() )
                          .Texture( "u_GBufferDepth", scene.GBufferDepth, RDG::Access::SampledCompute,
                                    RDG::SubresourceRange::All() )
                          .Texture( "u_History", written[0], RDG::Access::SampledCompute,
                                    RDG::SubresourceRange::All() )
                          .Texture( "u_HistoryCount", written[1], RDG::Access::SampledCompute,
                                    RDG::SubresourceRange::All() )
                          .Storage( "u_Output", output, RDG::Access::StorageWrite )
                          .Uniform( "DenoiserViewUB", uniforms )
                          .PushConstantBytes( static_cast<uint32_t>( sizeof( glm::vec4 ) ) );
                 },
                 [this, convolutionPush, width, height]( RDG::PassContext& context ) -> Common::BoolResultStr
                 {
                     DESERT_PROFILE_PASS( "Denoise: History Convolution" );
                     RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                     bindings.PushConstants( &convolutionPush,
                                             static_cast<uint32_t>( sizeof( convolutionPush ) ) );
                     return Renderer::GetInstance().DispatchCompute( bindings, *m_ConvolutionPipeline,
                                                                     Groups( width ), Groups( height ), 1 );
                 } );
            return output;
        }

        DenoiserSignal m_Signal;

        std::shared_ptr<Shader>          m_ReconstructShader;
        std::shared_ptr<Shader>          m_TemporalShader;
        std::shared_ptr<Shader>          m_ConvolutionShader;
        std::shared_ptr<ComputePipeline> m_ReconstructPipeline;
        std::shared_ptr<ComputePipeline> m_TemporalPipeline;
        std::shared_ptr<ComputePipeline> m_ConvolutionPipeline;
        // The block layouts, derived from each shader's reflection once per compile (not per frame).
        mutable ShaderBindingLayoutCache m_ReconstructLayout;
        mutable ShaderBindingLayoutCache m_TemporalLayout;
        mutable ShaderBindingLayoutCache m_ConvolutionLayout;

        std::array<HistorySlot, 2> m_Slots;
        uint32_t                   m_Parity = 0; // the slot read this frame; the other is written
    };
} // namespace Desert::Graphic::System
