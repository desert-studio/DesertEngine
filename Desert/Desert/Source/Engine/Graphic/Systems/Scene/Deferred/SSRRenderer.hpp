#pragma once

#include <Common/Core/Profiler.hpp>
#include <Engine/Graphic/ViewTargetLayouts.hpp>
#include <Engine/Graphic/ViewTargetFormats.hpp>
#include <Engine/Graphic/Systems/RenderSystem.hpp>

#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/ShaderBindingLayoutCache.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/Systems/Scene/Deferred/ScreenSpaceDenoiser.hpp>
#include <Engine/Graphic/DistanceField/GlobalDistanceField.hpp>
#include <Engine/Graphic/GlobalIllumination/GITracing.hpp>
#include <Engine/Graphic/GlobalIllumination/WorldRadianceCache.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <glm/glm.hpp>

#include <array>
#include <cstddef>
#include <format>
#include <optional>
#include <span>

namespace Desert::Graphic::System
{
    // Screen-space reflections (SSR2): one compute dispatch that classifies and traces; the screen-space
    // denoiser (ScreenSpaceDenoiser, the reflections signal) turns the trace into the reflection the deferred
    // lighting composes (DeferredLighting.shader u_SSR).
    //  1) CLASSIFY + TRACE (SSR.shader, compute, one workgroup per kTileSize x kTileSize screen tile): the tile
    //     is marked in the tile mask when any pixel passes the trace's G-buffer gate (sky / too rough -
    //     Common/SSRGate.glslh), and the same workgroup then traces at HALF resolution (as UE's SSR at its lower
    //     qualities), from a block pixel that rotates every frame so the temporal accumulation sees all four:
    //     Reflections.RaysPerPixel GGX-VNDF rays per texel, each walked over the closest-depth HZB
    //     (HZBRenderer, Common/HZBTrace.glslh), combined with the ratio estimator (rgb = radiance of the hits,
    //     a = the share of the lobe they answered) and written with the texel's representative hit point + pdf
    //     for the reconstruction. A hit's radiance is LAST frame's lit scene where the hit pixel's VELOCITY says
    //     it was (a moving object is read where it was - no trail): the view's scene colour history
    //     (SceneColorHistoryRenderer, shared with SSGI), which this frame's scene copy refills (UE: the previous
    //     frame's scene colour).
    //  2) DENOISE (ScreenSpaceDenoiser::DenoiseReflections): reconstruction at full resolution (the ratio
    //     estimator over the neighbouring trace texels, bilateral by depth plane and specular lobe), temporal
    //     accumulation through the reflected point with variance rejection, and the history convolution. The
    //     tile mask lets the reconstruction skip the tiles that reflect nothing.
    // Runs in the manual chain after the G-buffer and the HZB, BEFORE the deferred lighting.
    //
    // ONE PASS, TWO METHODS. ReflectionMethod::ScreenSpace runs the program's default compile (the screen alone);
    // ReflectionMethod::SoftwareTraced the permutation DESERT_REFLECTIONS_TRACED under the GI tracer's defines
    // (GlobalIllumination::TracerVariant): the rays the screen does not answer go on through the mesh SDFs and the
    // global field, a hit lit from the surface cache (UE Lumen's reflections, SSR.shader's header). The tracer,
    // the cards and the mesh SDFs are the GI's own frame records (TracedReads) - there is no second tracer - and
    // the denoiser is the same, so the two methods differ only in which rays come back answered.
    class SSRRenderer final : public RenderSystem
    {
    public:
        using RenderSystem::RenderSystem;

        virtual Common::BoolResultStr Initialize() override
        {
            m_TraceShader = Runtime::ResourceRegistry::GetShaderService()->GetByName( "SSR" );
            if ( !m_TraceShader )
                return Common::MakeError( "SSR shader not found (SSR)" );

            const auto tracePipeline =
                 ComputePipeline::Create( { .Shader = m_TraceShader, .DebugName = "SSRTrace" } );
            if ( !tracePipeline )
                return Common::MakeError( tracePipeline.GetError() );
            m_TracePipeline = tracePipeline.GetValue();

            return m_Denoiser.Initialize();
        }

        // What the traced permutation reads beside the screen's inputs (SSR.shader, bindings 10..41), all of this
        // frame's graph: GIHitRadiance's inputs (the GI frame record, the lights and the world light grid, the
        // radiance cache's read - a neutral one covering nothing when the GI does not run it -, the sky's cube,
        // the G-buffer's albedo and emissive), the tracer (GlobalIllumination::TracerFrame: for the software
        // tracer the view's mesh SDF near field too) and the surface cache's hits.
        struct TracedReads
        {
            GlobalIllumination::TracerFrame                                 Tracer;
            GlobalIllumination::SurfaceCacheFrame                           SurfaceCache;
            GlobalIllumination::RadianceCacheFrame                          RadianceCache;
            RDG::BufferRef                                                  GIFrame;
            RDG::BufferRef                                                  PointLights;
            RDG::BufferRef                                                  SpotLights;
            RDG::BufferRef                                                  WorldGridParams;
            RDG::BufferRef                                                  WorldGridCells;
            RDG::TextureRef                                                 EnvSpecular;
            RDG::TextureRef                                                 GBufferA;
            RDG::TextureRef                                                 GBufferEmissive;
        };

        // GRAPH BUILD, before DeclareTraceBindings of a traced frame: the traced permutation for @p pipeline's
        // tracer, compiled on first use and again when the tracer changes. An error: the program did not compile.
        [[nodiscard]] Common::BoolResultStr EnsureTracedProgram( const GlobalIllumination::Pipeline& pipeline )
        {
            if ( m_TracedPipeline && pipeline == m_TracedFor )
                return BOOLSUCCESS;
            Core::ShaderVariant variant = GlobalIllumination::TracerVariant( pipeline );
            variant.Defines.emplace_back( "DESERT_REFLECTIONS_TRACED" );
            auto shader = Runtime::ResourceRegistry::GetShaderService()->AcquireVariant( "SSR", variant );
            if ( !shader )
                return Common::MakeFormattedError<bool>(
                     "SSR: the traced reflections' program ({}, {}) did not compile",
                     GlobalIllumination::TracerDefine( pipeline.Tracer ),
                     GlobalIllumination::HitLightingDefine( pipeline.Hits ) );
            const auto compute = ComputePipeline::Create(
                 { .Shader = shader,
                   .DebugName =
                        std::format( "SSRTrace ({})", GlobalIllumination::TracerDefine( pipeline.Tracer ) ) } );
            if ( !compute )
                return Common::MakeError( compute.GetError() );
            m_TracedShader   = std::move( shader );
            m_TracedPipeline = compute.GetValue();
            m_TracedFor      = pipeline;
            return BOOLSUCCESS;
        }

        // The denoiser's history is a reprojection of the previous frame, and the previous frame is now a
        // different world — see IRenderSystem::OnSceneReplaced, kind 1.
        void OnSceneReplaced() override
        {
            m_Denoiser.InvalidateHistory();
        }

        // Camera cut — see IRenderSystem::OnTemporalHistoryReset. The frame index is the jitter/noise seed
        // and the history ping-pong parity, so it restarts with the history rather than carrying the count
        // of whatever frames came before the cut.
        void OnTemporalHistoryReset() override
        {
            m_FrameIndex = 0;
            m_Denoiser.InvalidateHistory();
        }

        // The two images the trace writes and the tiled passes read, both transients of this frame's graph
        // (Builder::CreateTexture): the half-resolution trace and the tile mask (one texel per kTileSize tile).
        struct TraceTargets
        {
            RDG::TextureDesc Trace;
            RDG::TextureDesc TraceRay; // the trace texels' hit points + ray pdfs (SSR.shader u_TraceRay)
            RDG::TextureDesc TileMask;
        };

        // Called while the frame graph is built, before any pass runs. The trace targets are sized from this
        // frame's view and kept by no one. Nullopt: nothing to draw.
        std::optional<TraceTargets> Prepare()
        {
            const auto& target = m_TargetFramebuffer.lock();
            if ( !target || !m_TracePipeline )
                return std::nullopt;
            const uint32_t w = target->GetFramebufferWidth();
            const uint32_t h = target->GetFramebufferHeight();
            if ( w == 0u || h == 0u )
                return std::nullopt;
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
        // depth at 0/1/2); sceneHistory = the view's scene colour history, imported (last frame's lit opaque
        // scene); hzb = this frame's closest-depth hierarchy (HZBRenderer); velocity = the view's velocity target
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
        // every reconstructing reader uses (SSRReflectionResolveUB, DeferredUB) so the camera block can take it
        // over. Last frame's screen position of a hit is the velocity target's, not a matrix's (SSR.shader).
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
        // @p traced: the traced permutation's reads (EnsureTracedProgram first), or null for the screen alone.
        void DeclareTraceBindings( RDG::PassBuilder& pass, RDG::TextureRef trace, RDG::TextureRef traceRay,
                                   RDG::TextureRef tiles, const GBufferInputs& gbuffer, RDG::TextureRef velocity,
                                   RDG::TextureRef sceneHistory, RDG::TextureRef hzb, RDG::BufferRef uniforms,
                                   const TracedReads* traced = nullptr ) const
        {
            const bool tracedProgram = traced != nullptr && m_TracedPipeline;
            if ( !m_TracePipeline || ( traced != nullptr && !tracedProgram ) )
                return;
            const std::shared_ptr<Shader>& shader   = tracedProgram ? m_TracedShader : m_TraceShader;
            ComputePipeline&               pipeline = tracedProgram ? *m_TracedPipeline : *m_TracePipeline;
            ShaderBindingLayoutCache&      layout   = tracedProgram ? m_TracedLayout : m_TraceLayout;
            auto block = pass.Bindings( layout.Get( shader ), Renderer::GetPipelineRouteFill( pipeline ) );
            block.Sampled( "u_GBufferNormal", gbuffer[1], RDG::Access::SampledCompute,
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
            if ( tracedProgram )
                DeclareTracedReads( block, *traced );
        }

        // EXEC of the same node: the block DeclareTraceBindings declared + this frame's push constants.
        // maxDistance and thickness are WORLD distances (a world unit is a centimetre - convert through
        // Common::Units). raysPerPixel / iterations: Reflections.RaysPerPixel / Reflections.MaxSteps.
        // sceneHistoryReadable: whether the scene colour history holds the previous frame (false: the trace
        // answers nothing).
        // seed: NextTraceSeed() of this frame, taken at graph build (the denoiser's reconstruction reads it too).
        struct TraceSettings
        {
            int   RaysPerPixel = 1;
            int   Iterations   = 32;
            float MaxDistance  = 0.0f;
            float Intensity    = 1.0f;
            float Thickness    = 0.0f;
            // SoftwareTraced: the traced permutation (EnsureTracedProgram).
            bool Traced = false;
        };
        [[nodiscard]] Common::BoolResultStr RecordTrace( const RDG::PassContext& context,
                                                         const glm::vec4& cameraPos, const TraceSettings& settings,
                                                         const bool     sceneHistoryReadable,
                                                         const uint32_t seed ) const
        {
            // Each pass is timed on its own because EnableSSR's default is a budget decision
            // (SceneSettings.hpp), and the whole-pass line cannot say which part to cut.
            DESERT_PROFILE_PASS( "SSR: Classify + Trace" );
            const TracePush push{
                 glm::vec4( glm::vec3( cameraPos ), static_cast<float>( seed ) ),
                 glm::vec4( static_cast<float>( settings.RaysPerPixel ), static_cast<float>( settings.Iterations ),
                            settings.Intensity, settings.Thickness ),
                 glm::vec4( settings.MaxDistance, sceneHistoryReadable ? 1.0f : 0.0f, 0.0f, 0.0f ) };
            ComputePipeline* const pipeline = settings.Traced ? m_TracedPipeline.get() : m_TracePipeline.get();
            if ( pipeline == nullptr )
                return Common::MakeError( "SSR: the traced reflections' program was not made this frame" );

            RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
            bindings.PushConstants( &push, static_cast<uint32_t>( sizeof( push ) ) );
            return Renderer::GetInstance().DispatchCompute( bindings, *pipeline, TileGrid( Width() ),
                                                            TileGrid( Height() ), 1 );
        }

        // GRAPH BUILD: this frame's trace seed (the ray sequence and the 2x2 block rotation), advancing the frame
        // count. Taken once per frame, before the trace and the denoiser's reconstruction are added.
        [[nodiscard]] uint32_t NextTraceSeed()
        {
            return m_FrameIndex++ % 1024u;
        }

        // Pass 2 (GRAPH BUILD): the screen-space denoiser over this frame's trace - see ScreenSpaceDenoiser.
        // Returns the denoised reflection the lighting composes (a transient of @p graph); invalid: not prepared.
        [[nodiscard]] RDG::TextureRef Denoise( RDG::Builder& graph, const ReflectionsDenoiserInputs& trace,
                                               const DenoiserSceneTextures& scene, const ViewFrame& frame )
        {
            return m_Denoiser.DenoiseReflections( graph, trace, scene, frame, Width(), Height() );
        }

    private:
        // The traced permutation's reads (SSR.shader DESERT_REFLECTIONS_TRACED), by the names its includes
        // declare.
        static void DeclareTracedReads( RDG::BindingBlockBuilder& block, const TracedReads& traced )
        {
            constexpr RDG::Access kR  = RDG::Access::StorageRead;
            constexpr RDG::Access kS  = RDG::Access::SampledCompute;
            const auto            all = RDG::SubresourceRange::All();
            block.Storage( "GIFrameUB", traced.GIFrame, kR )
                 .Storage( "PointLightsUB", traced.PointLights, kR )
                 .Storage( "SpotLightsUB", traced.SpotLights, kR )
                 .Storage( "LightGridParamsUB", traced.WorldGridParams, kR )
                 .Storage( "LightGridWorldCellsUB", traced.WorldGridCells, kR )
                 .Storage( "RadianceCacheParamsUB", traced.RadianceCache.Params, kR )
                 .Storage( "RadianceCacheIndirectionUB", traced.RadianceCache.Indirection, kR )
                 .Storage( "RadianceCacheProbesUB", traced.RadianceCache.Probes, kR )
                 .Sampled( "u_RadianceCacheIrradiance", traced.RadianceCache.Irradiance, kS, all,
                           RDG::SamplerDesc::LinearClamp() )
                 .Sampled( "u_RadianceCacheDepth", traced.RadianceCache.Depth, kS, all,
                           RDG::SamplerDesc::LinearClamp() )
                 .Sampled( "u_EnvSpecularTex", traced.EnvSpecular, kS, all, RDG::SamplerDesc::LinearClamp() )
                 .Sampled( "u_GBufferA", traced.GBufferA, kS, all, RDG::SamplerDesc::PointClamp() )
                 .Sampled( "u_GBufferEmissive", traced.GBufferEmissive, kS, all, RDG::SamplerDesc::PointClamp() );
            GlobalIllumination::DeclareTracerReads( block, traced.Tracer );
            if ( traced.Tracer.Pipeline.Hits == GlobalIllumination::HitLighting::SurfaceCache )
                GlobalIllumination::DeclareSurfaceCacheReads( block, traced.SurfaceCache );
        }

        // Screen pixels per tile edge. The grid is ceil(size / kTileSize) and SSR.shader stretches the
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
        static uint32_t HalfRes( uint32_t pixels )
        {
            return ( pixels + 1u ) / 2u;
        }

        std::shared_ptr<Shader>               m_TraceShader;
        // The trace's block layout, derived from the shader's reflection once per compile (not per frame).
        mutable ShaderBindingLayoutCache      m_TraceLayout;
        std::shared_ptr<ComputePipeline>      m_TracePipeline;
        // The traced permutation (EnsureTracedProgram) and the tracer it was compiled for.
        std::shared_ptr<Shader>               m_TracedShader;
        mutable ShaderBindingLayoutCache      m_TracedLayout;
        std::shared_ptr<ComputePipeline>      m_TracedPipeline;
        GlobalIllumination::Pipeline          m_TracedFor{};
        ScreenSpaceDenoiser                   m_Denoiser{ DenoiserSignal::Reflections };

        uint32_t m_FrameIndex = 0;
    };
} // namespace Desert::Graphic::System
