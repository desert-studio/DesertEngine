#pragma once

#include <Common/Core/Profiler.hpp>
#include <Engine/Graphic/ViewTargetLayouts.hpp>
#include <Engine/Graphic/ViewTargetFormats.hpp>
#include <Engine/Graphic/Systems/RenderSystem.hpp>

#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/RayTracingPipeline.hpp>
#include <Engine/Graphic/Systems/RayTracing/RayTracingHitGroupTable.hpp>
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
    //     Common/SSRGate.glslh), and the same workgroup then traces at the resolution Reflections.DownsampleFactor
    //     names (UE r.Lumen.Reflections.DownsampleFactor): 1 = every pixel; 2 = HALF resolution, from the pixel of
    //     each 2x2 block that rotates every frame so the temporal accumulation sees all four:
    //     Reflections.RaysPerPixel GGX-VNDF rays per texel, each walked over the closest-depth HZB
    //     (HZBRenderer, Common/HZBTrace.glslh), combined with the ratio estimator (rgb = radiance of the hits,
    //     a = the share of the lobe they answered) and written with the texel's representative hit point + pdf
    //     for the reconstruction. A hit's radiance is LAST frame's lit scene where the hit pixel's VELOCITY says
    //     it was (a moving object is read where it was - no trail): the view's scene colour history
    //     (SceneColorHistoryRenderer, shared with SSGI), which this frame's scene copy refills (UE: the previous
    //     frame's scene colour).
    //  2) DENOISE (ScreenSpaceDenoiser::DenoiseReflections): reconstruction at full resolution from either trace
    //     (the ratio
    //     estimator over the neighbouring trace texels, bilateral by depth plane and specular lobe), temporal
    //     accumulation through the reflected point with variance rejection, and the history convolution. The
    //     tile mask lets the reconstruction skip the tiles that reflect nothing.
    // Runs in the manual chain after the G-buffer and the HZB, BEFORE the deferred lighting.
    //
    // ONE PASS, THREE METHODS. ReflectionMethod::ScreenSpace runs the program's default compile (the screen
    // alone); the traced methods the permutation DESERT_REFLECTIONS_TRACED under a GI tracer's defines
    // (GlobalIllumination::TracerVariant of SceneRenderer::ReflectionsTracer): the rays the screen does not answer
    // go on into the world - SoftwareTraced through the mesh SDFs and the global field, HardwareTraced by ray
    // query over the view's TLAS (alpha-tested, UE Lumen HWRT reflections) - a hit lit from the surface cache (UE
    // Lumen's reflections, SSR.shader's header). The tracer, the cards and the mesh SDFs are the GI's own frame
    // records (TracedReads) - there is no second tracer - and the denoiser is the same, so the methods differ only
    // in which rays come back answered.
    class SSRRenderer final : public RenderSystem
    {
    public:
        using RenderSystem::RenderSystem;

        // WHICH SURFACE THE RAYS LEAVE FROM (UE ELumenReflectionPass: Opaque / FrontLayerTranslucency). Opaque: the
        // G-buffer's. TranslucentFrontLayer: the nearest translucent surface of each pixel ("Translucency: Front
        // Layer G-buffer", Reflections.TranslucentFrontLayer) - every program of this renderer is SSR.shader's
        // DESERT_REFLECTIONS_FRONT_LAYER permutation, the trace's blocks carry FrontLayerInputs, and the denoiser's
        // history is this renderer's own, as UE keeps the front layer's reflection history apart.
        enum class ReflectingSurface : uint8_t
        {
            Opaque,
            TranslucentFrontLayer,
        };
        static constexpr std::string_view kFrontLayerDefine = "DESERT_REFLECTIONS_FRONT_LAYER";

        // Before Initialize: the surface this renderer reflects (Opaque until said otherwise).
        void TraceFrom( const ReflectingSurface surface )
        {
            m_Surface = surface;
        }
        [[nodiscard]] ReflectingSurface Surface() const
        {
            return m_Surface;
        }

        // The front layer's G-buffer (Mesh/Surface/Pass_FrontLayer.glslh) the TranslucentFrontLayer programs start
        // their rays from: normal + roughness and depth, as u_FrontLayerNormal / u_FrontLayerDepth.
        struct FrontLayerInputs
        {
            RDG::TextureRef Normal;
            RDG::TextureRef Depth;
        };

        virtual Common::BoolResultStr Initialize() override
        {
            m_TraceShader = m_Surface == ReflectingSurface::TranslucentFrontLayer
                                 ? Runtime::ResourceRegistry::GetShaderService()->AcquireVariant(
                                        "SSR", Core::ShaderVariant{ .Defines = { std::string( kFrontLayerDefine ) } } )
                                 : Runtime::ResourceRegistry::GetShaderService()->GetByName( "SSR" );
            if ( !m_TraceShader )
                return Common::MakeError( m_Surface == ReflectingSurface::TranslucentFrontLayer
                                               ? "SSR: the front layer's program (SSR, DESERT_REFLECTIONS_FRONT_LAYER) "
                                                 "did not compile"
                                               : "SSR shader not found (SSR)" );

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
        // the G-buffer's albedo and emissive), the tracer (GlobalIllumination::TracerFrame), the surface cache's
        // hits and, for the software tracer, the mesh SDF near field with its distances (UE
        // r.Lumen.TraceMeshSDFs.TraceDistance, CardTraceEndDistanceFromCamera - the screen probes' own).
        struct TracedReads
        {
            GlobalIllumination::TracerFrame Tracer;
            // Hit lighting (Reflections.HitLighting): this frame's hit group table, whose templates built the
            // hit-lit pipeline (EnsureHitLightingPipeline); null = the traced compute walks the world itself.
            // Owned by the SceneRenderer for the frame (SceneRenderer::m_ReflectionHitGroups).
            const RayTracing::RayTracingHitGroupTable*                      HitGroups = nullptr;
            GlobalIllumination::SurfaceCacheFrame                           SurfaceCache;
            std::optional<DistanceField::GlobalDistanceField::MeshSDFScene> MeshSDFs;
            GlobalIllumination::RadianceCacheFrame                          RadianceCache;
            RDG::BufferRef                                                  GIFrame;
            RDG::BufferRef                                                  PointLights;
            RDG::BufferRef                                                  SpotLights;
            RDG::BufferRef                                                  WorldGridParams;
            RDG::BufferRef                                                  WorldGridCells;
            RDG::TextureRef                                                 EnvSpecular;
            RDG::TextureRef                                                 GBufferA;
            RDG::TextureRef                                                 GBufferEmissive;
            float                                                           MeshSDFTraceDistance         = 0.0f;
            float                                                           MeshSDFEndDistanceFromCamera = 0.0f;
        };

        // GRAPH BUILD, before DeclareTraceBindings of a traced frame: the traced permutation for @p pipeline's
        // tracer, compiled on first use and again when the tracer changes. An error: the program did not compile.
        // @p hitLighting: Reflections.HitLighting on the hardware tracer - the compute classifies only and the
        // "SSR/HitLighting" ray generation traces (EnsureHitLightingPipeline); the permutation adds
        // DESERT_REFLECTIONS_HIT_LIGHTING to both entry points.
        [[nodiscard]] Common::BoolResultStr EnsureTracedProgram( const GlobalIllumination::Pipeline& pipeline,
                                                                 const bool                          hitLighting )
        {
            if ( m_TracedPipeline && pipeline == m_TracedFor && hitLighting == m_TracedHitLit )
                return BOOLSUCCESS;
            Core::ShaderVariant variant = GlobalIllumination::TracerVariant( pipeline );
            variant.Defines.emplace_back( "DESERT_REFLECTIONS_TRACED" );
            if ( hitLighting )
                variant.Defines.emplace_back( "DESERT_REFLECTIONS_HIT_LIGHTING" );
            if ( m_Surface == ReflectingSurface::TranslucentFrontLayer )
                variant.Defines.emplace_back( kFrontLayerDefine );
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
            m_TracedHitLit   = hitLighting;
            m_TracedVariant  = std::move( variant );
            return BOOLSUCCESS;
        }

        // GRAPH BUILD, after EnsureTracedProgram( ..., true ): the ray tracing pipeline of the hit-lit trace - the
        // "SSR/HitLighting" pass of the same permutation and one hit group per surface template of this frame's
        // hit group table (@p templates = RayTracingHitGroupTable::Templates: each template's `RayTracing.Hit`
        // cell, "<Template>/RayTracing.Hit"). Rebuilt when the permutation or the template list changes. The table
        // holds only templates with a compiled cell (BuildRayTracingHitGroupTable falls back to the default
        // surface), so a missing cell here is a broken invariant; an error names it or what the pipeline refused.
        [[nodiscard]] Common::BoolResultStr EnsureHitLightingPipeline( const std::vector<std::string>& templates )
        {
            if ( !m_TracedHitLit )
                return Common::MakeError(
                     "SSR: the hit-lit pipeline was asked for a permutation without hit lighting" );
            if ( m_HitLightingPipeline && templates == m_HitLightingTemplates &&
                 m_HitLightingFor == m_TracedVariant.Hash() )
                return BOOLSUCCESS;
            auto* shaders = Runtime::ResourceRegistry::GetShaderService();
            auto  program = shaders->AcquireVariant( "SSR/HitLighting", m_TracedVariant );
            if ( !program )
                return Common::MakeError( "SSR: the hit-lit ray generation ('SSR/HitLighting') did not compile or "
                                          "is not registered on this device" );
            RayTracingPipelineSpecification spec{
                 .Program = program, .HitGroups = {}, .DebugName = "SSRHitLighting" };
            for ( const std::string& name : templates )
            {
                const std::string cell = std::format( "{}/RayTracing.Hit", name );
                auto              hit  = shaders->GetByName( cell );
                if ( !hit || !hit->IsCompiled() )
                    return Common::MakeFormattedError(
                         "SSR: the hit group table names surface template '{}', whose "
                         "hit group '{}' is not compiled",
                         name, cell );
                spec.HitGroups.push_back( std::move( hit ) );
            }
            auto pipeline = RayTracingPipeline::Create( spec );
            if ( !pipeline )
                return Common::MakeError( pipeline.GetError() );
            m_HitLightingShader    = std::move( program );
            m_HitLightingPipeline  = pipeline.GetValue();
            m_HitLightingTemplates = templates;
            m_HitLightingFor       = m_TracedVariant.Hash();
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
        // (Builder::CreateTexture): the trace (one texel per DownsampleFactor x DownsampleFactor pixel block) and the
        // tile mask (one texel per kTileSize tile).
        struct TraceTargets
        {
            RDG::TextureDesc Trace;
            RDG::TextureDesc TraceRay; // the trace texels' hit points + ray pdfs (SSR.shader u_TraceRay)
            RDG::TextureDesc TileMask;
            // The factor the targets were sized for: the trace's push (TraceSettings) and the denoiser's
            // reconstruction (ReflectionsDenoiserInputs) take it from here, so all three agree within a frame.
            uint32_t DownsampleFactor = 2;
        };

        // The trace resolutions the shaders know (SSR.shader's source pixel, SSDReconstructReflections' block
        // rotation): every pixel, or one pixel of each 2x2 block. Scalability Reflections.DownsampleFactor's range.
        [[nodiscard]] static constexpr bool IsSupportedDownsampleFactor( const uint32_t factor )
        {
            return factor == 1u || factor == 2u;
        }

        // Called while the frame graph is built, before any pass runs. The trace targets are sized from this
        // frame's view and @p downsampleFactor (IsSupportedDownsampleFactor; the caller reports any other) and kept
        // by no one. Nullopt: nothing to draw.
        std::optional<TraceTargets> Prepare( const uint32_t downsampleFactor )
        {
            const auto& target = m_TargetFramebuffer.lock();
            if ( !target || !m_TracePipeline )
                return std::nullopt;
            const uint32_t w = target->GetFramebufferWidth();
            const uint32_t h = target->GetFramebufferHeight();
            if ( w == 0u || h == 0u || !IsSupportedDownsampleFactor( downsampleFactor ) )
                return std::nullopt;
            const uint32_t traceW = TraceExtent( w, downsampleFactor );
            const uint32_t traceH = TraceExtent( h, downsampleFactor );
            return TraceTargets{
                 .Trace    = RDG::TextureDesc{ .Size   = { .Width = traceW, .Height = traceH },
                                               .Format = ViewTargetFormats::kSSRTrace },
                 .TraceRay = RDG::TextureDesc{ .Size   = { .Width = traceW, .Height = traceH },
                                               .Format = ViewTargetFormats::kSSRTraceRay },
                 .TileMask = RDG::TextureDesc{ .Size   = { .Width = TileGrid( w ), .Height = TileGrid( h ) },
                                               .Format = ViewTargetFormats::kSSRTileMask },
                 .DownsampleFactor = downsampleFactor,
            };
        }

        // Pass 1, the "Deferred: SSR" compute node: classify + trace, one dispatch (one workgroup
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
            glm::vec4 Params2;   // x = max ray distance, y = 1 when the scene colour history is last frame's,
                                 // z = mesh SDF trace distance, w = mesh SDF end distance from the camera
            glm::vec4 Params3;   // x = the trace's downsample factor (TraceTargets::DownsampleFactor), yzw unused
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
        // @p sceneHistory, @p hzb (all mips) and the velocity as separate textures read through the pass's shared
        // sampler states (the normal linear REPEAT, as the deferred composite reads it; the scene history linear
        // clamped; depth, HZB and velocity point-clamped: SSR.shader names each one's state), @p
        // trace / @p tiles as storage writes, @p uniforms (UploadTraceUniforms) as SSRTraceUB, the TracePush
        // bytes; the pipeline's own setters are the other route. Not prepared: nothing.
        // @p traced: the traced permutation's reads (EnsureTracedProgram first), or null for the screen alone.
        // @p frontLayer: the TranslucentFrontLayer surface's G-buffer (null for an Opaque renderer).
        void DeclareTraceBindings( RDG::PassBuilder& pass, RDG::TextureRef trace, RDG::TextureRef traceRay,
                                   RDG::TextureRef tiles, const GBufferInputs& gbuffer, RDG::TextureRef velocity,
                                   RDG::TextureRef sceneHistory, RDG::TextureRef hzb, RDG::BufferRef uniforms,
                                   const TracedReads* traced = nullptr, const FrontLayerInputs* frontLayer = nullptr ) const
        {
            const bool tracedProgram = traced != nullptr && m_TracedPipeline;
            if ( !m_TracePipeline || ( traced != nullptr && !tracedProgram ) )
                return;
            const std::shared_ptr<Shader>& shader   = tracedProgram ? m_TracedShader : m_TraceShader;
            ComputePipeline&               pipeline = tracedProgram ? *m_TracedPipeline : *m_TracePipeline;
            ShaderBindingLayoutCache&      layout   = tracedProgram ? m_TracedLayout : m_TraceLayout;
            auto block = pass.Bindings( layout.Get( shader ), Renderer::GetPipelineRouteFill( pipeline ) );
            block.Texture( "u_GBufferNormal", gbuffer[1], RDG::Access::SampledCompute,
                           RDG::SubresourceRange::All() )
                 .Sampler( "u_LinearRepeatSampler", RDG::SamplerDesc::LinearRepeat() )
                 .Texture( "u_GBufferDepth", gbuffer[2], RDG::Access::SampledCompute,
                           RDG::SubresourceRange::All() )
                 .Texture( "u_SceneColor", sceneHistory, RDG::Access::SampledCompute,
                           RDG::SubresourceRange::All() )
                 .Sampler( "u_LinearClampSampler", RDG::SamplerDesc::LinearClamp() )
                 .Texture( "u_HZB", hzb, RDG::Access::SampledCompute, RDG::SubresourceRange::All() )
                 .Texture( "u_Velocity", velocity, RDG::Access::SampledCompute, RDG::SubresourceRange::All() )
                 .Sampler( "u_PointClampSampler", RDG::SamplerDesc::PointClamp() )
                 .Storage( "u_Trace", trace, RDG::Access::StorageWrite )
                 .Storage( "u_TraceRay", traceRay, RDG::Access::StorageWrite )
                 .Storage( "u_TileMask", tiles, RDG::Access::StorageWrite )
                 .Uniform( "SSRTraceUB", uniforms )
                 .PushConstantBytes( static_cast<uint32_t>( sizeof( TracePush ) ) );
            if ( tracedProgram )
                DeclareTracedReads( block, *traced );
            DeclareFrontLayerReads( block, frontLayer );
        }

        // SETUP of the "Deferred: Reflections (HitLighting)" node (after the classify): the hit-lit trace's block
        // - DeclareTraceBindings' reads and writes against the ray generation's program, the tile mask READ (the
        // classify wrote it), the view block @p camera (ShaderProtocols::Camera, for the hit cells' SurfaceInput
        // and the ray cone) and the traced reads. EnsureHitLightingPipeline first.
        void DeclareHitLightingBindings( RDG::PassBuilder& pass, RDG::TextureRef trace, RDG::TextureRef traceRay,
                                         RDG::TextureRef tiles, const GBufferInputs& gbuffer,
                                         RDG::TextureRef velocity, RDG::TextureRef sceneHistory,
                                         RDG::TextureRef hzb, RDG::BufferRef uniforms, RDG::BufferRef camera,
                                         const TracedReads& traced,
                                         const FrontLayerInputs* frontLayer = nullptr ) const
        {
            if ( !m_HitLightingPipeline )
                return;
            auto block = pass.Bindings( m_HitLightingLayout.Get( m_HitLightingShader ), RDG::OtherRouteFill{} );
            block.Texture( "u_GBufferNormal", gbuffer[1], RDG::Access::SampledCompute,
                           RDG::SubresourceRange::All() )
                 .Sampler( "u_LinearRepeatSampler", RDG::SamplerDesc::LinearRepeat() )
                 .Texture( "u_GBufferDepth", gbuffer[2], RDG::Access::SampledCompute,
                           RDG::SubresourceRange::All() )
                 .Texture( "u_SceneColor", sceneHistory, RDG::Access::SampledCompute,
                           RDG::SubresourceRange::All() )
                 .Sampler( "u_LinearClampSampler", RDG::SamplerDesc::LinearClamp() )
                 .Texture( "u_HZB", hzb, RDG::Access::SampledCompute, RDG::SubresourceRange::All() )
                 .Texture( "u_Velocity", velocity, RDG::Access::SampledCompute, RDG::SubresourceRange::All() )
                 .Sampler( "u_PointClampSampler", RDG::SamplerDesc::PointClamp() )
                 .Storage( "u_Trace", trace, RDG::Access::StorageWrite )
                 .Storage( "u_TraceRay", traceRay, RDG::Access::StorageWrite )
                 .Storage( "u_TileMask", tiles, RDG::Access::StorageRead )
                 .Uniform( "SSRTraceUB", uniforms )
                 .Uniform( "CameraUB", camera )
                 .PushConstantBytes( static_cast<uint32_t>( sizeof( TracePush ) ) );
            DeclareTracedReads( block, traced );
            DeclareFrontLayerReads( block, frontLayer );
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
            // A traced method: the traced permutation (EnsureTracedProgram), with the near field's distances (cm;
            // software tracer only, 0 otherwise).
            bool  Traced                       = false;
            float MeshSDFTraceDistance         = 0.0f;
            float MeshSDFEndDistanceFromCamera = 0.0f;
            // TraceTargets::DownsampleFactor of this frame's targets.
            uint32_t DownsampleFactor = 2;
        };
        [[nodiscard]] Common::BoolResultStr RecordTrace( const RDG::PassContext& context,
                                                         const glm::vec4& cameraPos, const TraceSettings& settings,
                                                         const bool     sceneHistoryReadable,
                                                         const uint32_t seed ) const
        {
            // Each pass is timed on its own because EnableSSR's default is a budget decision
            // (SceneSettings.hpp), and the whole-pass line cannot say which part to cut.
            DESERT_PROFILE_PASS( "SSR: Classify + Trace" );
            const TracePush push = MakeTracePush( cameraPos, settings, sceneHistoryReadable, seed );
            ComputePipeline* const pipeline = settings.Traced ? m_TracedPipeline.get() : m_TracePipeline.get();
            if ( pipeline == nullptr )
                return Common::MakeError( "SSR: the traced reflections' program was not made this frame" );

            RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
            bindings.PushConstants( &push, static_cast<uint32_t>( sizeof( push ) ) );
            return Renderer::GetInstance().DispatchCompute( bindings, *pipeline, TileGrid( Width() ),
                                                            TileGrid( Height() ), 1 );
        }

        // EXEC of the hit-lit node: the same push constants as the classify's, one ray generation per trace texel,
        // the shader binding table from @p hits (this frame's RayTracingHitGroupTable, whose Templates built the
        // pipeline).
        [[nodiscard]] Common::BoolResultStr
        RecordHitLighting( const RDG::PassContext& context, const glm::vec4& cameraPos,
                           const TraceSettings& settings, const bool sceneHistoryReadable, const uint32_t seed,
                           const RayTracing::RayTracingHitGroupTable& hits ) const
        {
            DESERT_PROFILE_PASS( "SSR: Hit-lit trace" );
            if ( !m_HitLightingPipeline )
                return Common::MakeError( "SSR: the hit-lit pipeline was not made this frame" );
            const TracePush push = MakeTracePush( cameraPos, settings, sceneHistoryReadable, seed );
            RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
            bindings.PushConstants( &push, static_cast<uint32_t>( sizeof( push ) ) );
            return Renderer::TraceRays( bindings, *m_HitLightingPipeline, hits,
                                        TraceExtent( Width(), settings.DownsampleFactor ),
                                        TraceExtent( Height(), settings.DownsampleFactor ) );
        }

        // GRAPH BUILD: this frame's trace seed (the ray sequence and, at factor 2, the 2x2 block rotation), advancing the frame
        // count. Taken once per frame, before the trace and the denoiser's reconstruction are added.
        [[nodiscard]] uint32_t NextTraceSeed()
        {
            return m_FrameIndex++ % 1024u;
        }

        // The view's temporal upscaler takes this signal raw this frame (ITemporalUpscaler::
        // TakesRawLightingSignals: DLSS Ray Reconstruction denoises it): the denoiser does not run, so its history
        // stops being the previous frame's and is dropped - the frame it resumes on starts it afresh.
        void SkipDenoise()
        {
            m_Denoiser.InvalidateHistory();
        }

        // Pass 2 (GRAPH BUILD): the screen-space denoiser over this frame's trace - see ScreenSpaceDenoiser.
        // Returns the denoised reflection the lighting composes (a transient of @p graph); invalid: not prepared.
        [[nodiscard]] RDG::TextureRef Denoise( RDG::Builder& graph, const ReflectionsDenoiserInputs& trace,
                                               const DenoiserSceneTextures& scene, const ViewFrame& frame )
        {
            return m_Denoiser.DenoiseReflections( graph, trace, scene, frame, Width(), Height() );
        }

    private:
        // The push constants of both trace entry points (SSR.shader's compute and its "HitLighting" ray generation).
        static TracePush MakeTracePush( const glm::vec4& cameraPos, const TraceSettings& settings,
                                        const bool sceneHistoryReadable, const uint32_t seed )
        {
            return TracePush{ glm::vec4( glm::vec3( cameraPos ), static_cast<float>( seed ) ),
                              glm::vec4( static_cast<float>( settings.RaysPerPixel ),
                                         static_cast<float>( settings.Iterations ), settings.Intensity,
                                         settings.Thickness ),
                              glm::vec4( settings.MaxDistance, sceneHistoryReadable ? 1.0f : 0.0f,
                                         settings.MeshSDFTraceDistance, settings.MeshSDFEndDistanceFromCamera ),
                              glm::vec4( static_cast<float>( settings.DownsampleFactor ), 0.0f, 0.0f, 0.0f ) };
        }

        // The front layer permutation's reads (Common/SSRTrace.glslh DESERT_REFLECTIONS_FRONT_LAYER): a
        // TranslucentFrontLayer renderer's programs declare them, an Opaque one's do not - an absent @p frontLayer
        // on the former leaves the slots unbound and the node refused by name, never a trace from the wrong surface.
        static void DeclareFrontLayerReads( RDG::BindingBlockBuilder& block, const FrontLayerInputs* frontLayer )
        {
            if ( frontLayer == nullptr )
                return;
            block
                 .Texture( "u_FrontLayerNormal", frontLayer->Normal, RDG::Access::SampledCompute,
                           RDG::SubresourceRange::All() )
                 .Texture( "u_FrontLayerDepth", frontLayer->Depth, RDG::Access::SampledCompute,
                           RDG::SubresourceRange::All() )
                 .Sampler( "u_PointClampSampler", RDG::SamplerDesc::PointClamp() );
        }

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
                 .Texture( "u_RadianceCacheIrradiance", traced.RadianceCache.Irradiance, kS, all )
                 .Texture( "u_RadianceCacheDepth", traced.RadianceCache.Depth, kS, all )
                 .Texture( "u_EnvSpecularTex", traced.EnvSpecular, kS, all )
                 .Sampler( "u_LinearClampSampler", RDG::SamplerDesc::LinearClamp() )
                 .Texture( "u_GBufferA", traced.GBufferA, kS, all )
                 .Texture( "u_GBufferEmissive", traced.GBufferEmissive, kS, all )
                 .Sampler( "u_PointClampSampler", RDG::SamplerDesc::PointClamp() );
            GlobalIllumination::DeclareTracerReads( block, traced.Tracer );
            if ( traced.Tracer.Pipeline.Hits == GlobalIllumination::HitLighting::SurfaceCache )
                GlobalIllumination::DeclareSurfaceCacheReads( block, traced.SurfaceCache );
            if ( traced.MeshSDFs )
            {
                const DistanceField::GlobalDistanceField::MeshSDFScene& mesh = *traced.MeshSDFs;
                block.Texture( "u_DFBrickAtlas", mesh.BrickAtlas, kS, all )
                     .Sampler( "u_LinearRepeatSampler", RDG::SamplerDesc::LinearRepeat() )
                     .Storage( "DFAssetData", mesh.AssetData, kR )
                     .Storage( "DFIndirection", mesh.Indirection, kR )
                     .Storage( "DFObjects", mesh.Objects, kR )
                     .Storage( "GIMeshSDFObjectRowsUB", mesh.ObjectRows, kR )
                     .Storage( "GIMeshSDFCulledUB", mesh.Culled, kR );
            }
        }

        // Screen pixels per tile edge. The grid is ceil(size / kTileSize) and SSR.shader stretches the
        // tiles over the target, so every pixel belongs to exactly one tile.
        static constexpr uint32_t kTileSize = 8;
        static uint32_t           TileGrid( uint32_t pixels )
        {
            return ( pixels + kTileSize - 1u ) / kTileSize;
        }
        // The view's size in pixels (the trace's is TraceExtent of it).
        [[nodiscard]] uint32_t Width() const
        {
            return m_TargetFramebuffer.lock()->GetFramebufferWidth();
        }
        [[nodiscard]] uint32_t Height() const
        {
            return m_TargetFramebuffer.lock()->GetFramebufferHeight();
        }
        // The trace's size along one axis: one texel per @p factor pixels, rounded up so a partial edge block still
        // has a texel.
        static uint32_t TraceExtent( const uint32_t pixels, const uint32_t factor )
        {
            return ( pixels + factor - 1u ) / factor;
        }

        std::shared_ptr<Shader>               m_TraceShader;
        // The trace's block layout, derived from the shader's reflection once per compile (not per frame).
        mutable ShaderBindingLayoutCache      m_TraceLayout;
        std::shared_ptr<ComputePipeline>      m_TracePipeline;
        // The traced permutation (EnsureTracedProgram) and the tracer it was compiled for.
        std::shared_ptr<Shader>          m_TracedShader;
        mutable ShaderBindingLayoutCache m_TracedLayout;
        std::shared_ptr<ComputePipeline> m_TracedPipeline;
        GlobalIllumination::Pipeline     m_TracedFor{};
        bool                             m_TracedHitLit = false;
        Core::ShaderVariant              m_TracedVariant;
        // The hit-lit trace (Reflections.HitLighting): its ray generation program (the traced permutation's
        // "SSR/HitLighting" pass), its ray tracing pipeline and the templates whose hit groups it was built with.
        std::shared_ptr<Shader>               m_HitLightingShader;
        mutable ShaderBindingLayoutCache      m_HitLightingLayout;
        std::shared_ptr<RayTracingPipeline>   m_HitLightingPipeline;
        std::vector<std::string>              m_HitLightingTemplates;
        uint64_t                              m_HitLightingFor = 0;
        ScreenSpaceDenoiser                   m_Denoiser{ DenoiserSignal::Reflections };
        ReflectingSurface                     m_Surface = ReflectingSurface::Opaque;

        uint32_t m_FrameIndex = 0;
    };
} // namespace Desert::Graphic::System
