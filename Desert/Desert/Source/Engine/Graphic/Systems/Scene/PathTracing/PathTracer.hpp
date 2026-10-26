#pragma once

#include <Common/Core/Profiler.hpp>
#include <Engine/Assets/FrameRetireQueue.hpp>
#include <Engine/Core/FrameManager.hpp>
#include <Engine/Graphic/Systems/RenderSystem.hpp>

#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/RayTracingPipeline.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/ShaderBindingLayoutCache.hpp>
#include <Engine/Graphic/Systems/RayTracing/EmissiveLightsRenderer.hpp>
#include <Engine/Graphic/Systems/RayTracing/RayTracingHitGroupTable.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/View/PassHistory.hpp>
#include <Engine/Graphic/View/ViewFrame.hpp>
#include <Engine/Graphic/ViewTargetFormats.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <glm/glm.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

namespace Desert::Graphic::System
{
    // The slot names this pass binds, spelled once (RDGShaderParameters.hpp).
    struct PathTracingSkyPrepareShaderParameters
    {
        DESERT_SHADER_PARAMETER( Texture, EnvironmentSpecularTexture );
        DESERT_SHADER_PARAMETER( Sampler, LinearClampSampler );
        DESERT_SHADER_PARAMETER( StorageTexture, SkyRadianceTexture );
        DESERT_SHADER_PARAMETER( StorageBuffer, PathTracingSkyPdfUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, GIFrameUB );
    };

    // The slot names this pass binds, spelled once (RDGShaderParameters.hpp).
    struct PathTracingSkyReduceShaderParameters
    {
        DESERT_SHADER_PARAMETER( StorageBuffer, PathTracingSkyPdfUB );
    };

    // The slot names this pass binds, spelled once (RDGShaderParameters.hpp).
    struct PathTracingTraceShaderParameters
    {
        DESERT_SHADER_PARAMETER( UniformBuffer, CameraUB );
        DESERT_SHADER_PARAMETER( AccelerationStructure, SceneTlas );
        DESERT_SHADER_PARAMETER( StorageTexture, AccumulationTexture );
        DESERT_SHADER_PARAMETER( StorageBuffer, GIFrameUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, PointLightsUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, SpotLightsUB );
        DESERT_SHADER_PARAMETER( StorageTexture, SkyRadianceTexture );
        DESERT_SHADER_PARAMETER( StorageBuffer, PathTracingSkyPdfUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, EmissiveSegmentsUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, EmissiveLookupUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, EmissiveTrianglesUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, EmissivePowerUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, EmissiveDynamicSegmentsUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, EmissiveDynamicTrianglesUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, EmissiveDynamicPowerUB );
        DESERT_SHADER_PARAMETER( Texture, EnvironmentSpecularTexture );
        DESERT_SHADER_PARAMETER( Sampler, LinearClampSampler );
        DESERT_SHADER_PARAMETER( Texture, CloudShadowMapTexture );
        DESERT_SHADER_PARAMETER( Sampler, LinearRepeatSampler );
        DESERT_SHADER_PARAMETER( UniformBuffer, CloudShadowUB );
    };

    // The slot names this pass binds, spelled once (RDGShaderParameters.hpp).
    struct PathTracingResolveShaderParameters
    {
        DESERT_SHADER_PARAMETER( Texture, AccumulationTexture );
        DESERT_SHADER_PARAMETER( Sampler, PointClampSampler );
    };

    // THE PATH TRACER (GIMethod / ReflectionMethod PathTraced). UE 5.8
    // Renderer/Private/PathTracing/PathTracing.cpp (FPathTracingState, RenderPathTracing) is the model: the view
    // keeps ONE running sum of paths per pixel and how many were summed; each frame adds
    // PathTracing.PathsPerFrame paths of up to PathTracing.MaxBounces bounces (PathTracer.shader "PathTracing",
    // the ray tracing pipeline over the view's TLAS with each surface template's own hit group), and the resolve
    // (PathTracerResolve.shader) writes sum / count over the scene colour before post. Once
    // PathTracing.SamplesPerPixel paths are summed (UE r.PathTracing.SamplesPerPixel: the total per pixel), the
    // view stops tracing and keeps resolving the converged image.
    //
    // THE RESTART (UE: the accumulation is invalidated when anything it integrated changed): the sum starts again
    // at 0 when the history is not this view's previous frame (PassHistoryStamp: a cut, a resize, a method change,
    // a skipped frame), the camera moved (ViewProjection != PrevViewProjection), MaxBounces changed, geometry was
    // rebuilt under unchanged rows (RayTracingSceneFrame::DynamicGeometry), or the scene's hash moved
    // (FrameInputs::SceneHash: the TLAS rows, the lights, the sun and sky, the environment, the hit groups'
    // material rows - SceneRenderer::PathTracingSceneHash), MaxPathIntensity changed, or the exposure the clamp
    // was set at moved by more than kClampExposureStops (below).
    //
    // THE PATH INTENSITY CLAMP (UE PathTracingCore.ush AccumulateRadiance, r.PathTracing.MaxPathIntensity): every
    // contribution a path adds - each light sample, emission or sky its material rays meet - is scaled down, hue
    // kept, to at most MaxPathIntensity in exposed units; what the camera ray itself sees (through delta glass
    // interfaces too) is never clamped. That bounds a pixel's variance (the fireflies a small bright emitter met by a
    // diffuse bounce makes) at the price of a bias toward darker highlights. The clamp is stated in exposed units,
    // so the trace receives MaxPathIntensity / PreExposure - the PreExposure of the frame the sum (re)started at
    // (m_ClampPreExposure). A sum keeps one clamp: clamping is not linear, so paths clamped at another value cannot
    // be summed with it. In UE the clamp follows the current exposure and a change of it is a change of the
    // config; here eye adaptation moves PreExposure a little every frame (the path-traced image feeds the
    // histogram), and restarting on each move would never converge, so the sum restarts only when the exposure
    // left the one it was clamped at by more than kClampExposureStops.
    //
    // THE SKY LIGHT (UE PathTracingSkylightPrepare.usf): each traced frame the environment cube is resampled into
    // an equal-area square of side S (the cube face rounded up to a power of two, UE: the capture size) and the
    // pyramid of its weights is reduced level by level (PathTracerSky.shader "Prepare" / "Reduce"); the paths
    // importance-sample it (Common/PathTracingSky.glslh).
    //
    // THE EMISSIVE SURFACES (RayTracing::EmissiveLightsFrame, the scene's one emissive triangle list): each vertex
    // also samples one emissive triangle by power and a point on it, a strategy of its own beside the analytic
    // lights, and emission met by a material ray is weighed against it (PathTracer.shader). The list's hash is in
    // the scene hash, so a changed emitter restarts the sum.
    //
    // GLASS (UE PathTracingGlass.ush, smooth): a BlendMode Translucent surface is a dielectric interface - Fresnel
    // reflection or refraction by its IOR, thin at IOR 1, Beer absorption by its tint inside a solid - and light
    // rays are attenuated by it instead of blocked (PathTracer.shader; its primitives carry
    // RayTracing::kMaskTranslucent, plus kMaskTranslucentShadowCaster with CastShadows, so only these rays meet
    // them). Light rays trace the shadow-caster bits only: CastShadows off never occludes a path-traced light.
    //
    // Not here (each is UE's and a task of its own): a denoiser (UE's is a plugin over albedo/normal guides),
    // rough glass (the raster's glass pass reads no roughness), participating media, a Sobol sequence, pixel
    // filters other than a box.
    class PathTracer final : public RenderSystem
    {
    public:
        using RenderSystem::RenderSystem;

        // How far (in stops) the view's PreExposure may move from the one the sum's clamp was stated at before the
        // sum restarts (THE PATH INTENSITY CLAMP above): the clamp holds to within a factor of 2 of the image's
        // white, and the noise of a converging sum through eye adaptation never reaches a stop.
        static constexpr float kClampExposureStops = 1.0f;

        // Scalability PathTracing.* (BeginScene).
        struct Settings
        {
            uint32_t SamplesPerPixel = 1024; // the total per pixel; tracing stops once summed (UE's meaning)
            uint32_t PathsPerFrame   = 1;    // paths added per pixel each frame (UE traces one)
            uint32_t MaxBounces      = 8;
            // UE r.PathTracing.MaxPathIntensity: the brightest one path's contribution may be, in EXPOSED units (1 ~
            // the image's white); a brighter one is scaled down to it with its hue kept (PathTracer.shader
            // PathTracingAccumulate). 0 = no clamp.
            float MaxPathIntensity = 24.0f;
        };

        // What one frame's paths read, all of this frame's graph (SceneRenderer::AddFramePathTracing).
        struct FrameInputs
        {
            RDG::BufferRef  Tlas;        // RayTracingSceneFrame::Tlas (Built)
            RDG::BufferRef  Camera;      // ShaderProtocols::Camera of the frame (CameraUB)
            RDG::BufferRef  GIFrame;     // the GI record (GIFrameUB): sun, sky look
            RDG::BufferRef  PointLights; // FrameTransients Lights.Point / Spot
            RDG::BufferRef  SpotLights;
            uint32_t        PointLightCount = 0;
            uint32_t        SpotLightCount  = 0;
            RDG::TextureRef EnvSpecular; // the environment cube (System.BlackCube without one)
            RDG::TextureRef CloudShadowMap; // the cloud layer's shadow map (CloudShadowMapOrWhite)
            RDG::BufferRef  CloudShadow;    // its CloudShadowUB (CloudShadowPackUniforms; Params.y = 0: no map)
            // This frame's hit group table; its Templates built the pipeline (EnsurePipeline).
            const RayTracing::RayTracingHitGroupTable* HitGroups = nullptr;
            RayTracing::EmissiveLightsFrame            Emissive; // this frame's emissive triangle list (Valid)
            float    SunSinHalfAngle = 0.0f; // sin of half the sun's SourceAngle (0: a delta direction)
            float    Light1SinHalfAngle = 0.0f; // the same of atmosphere light 1 (the moon)
            float    PixelSpread     = 0.0f; // the angle one pixel covers (the ray cone)
            uint64_t SceneHash       = 0;
            bool     DynamicGeometry = false;
        };

        // FNV-1a over what the accumulation integrated (FrameInputs::SceneHash). By bytes, so only for types with
        // no padding (the GPU payloads: rows of 16-byte vectors); a padded struct is hashed field by field.
        class SceneHasher
        {
        public:
            void Bytes( const std::span<const std::byte> bytes )
            {
                for ( const std::byte b : bytes )
                    m_Value = ( m_Value ^ static_cast<uint64_t>( b ) ) * 1099511628211ull;
            }
            template <typename T>
            void Value( const T& value )
            {
                static_assert( std::is_trivially_copyable_v<T>, "hashed by its bytes" );
                Bytes( std::as_bytes( std::span<const T>( &value, 1 ) ) );
            }
            template <typename T>
            void Values( const std::span<const T> values )
            {
                static_assert( std::is_trivially_copyable_v<T>, "hashed by its bytes" );
                Bytes( std::as_bytes( values ) );
            }
            [[nodiscard]] uint64_t Get() const
            {
                return m_Value;
            }

        private:
            uint64_t m_Value = 14695981039346656037ull;
        };

        Common::BoolResultStr Initialize() override
        {
            auto* shaders = Runtime::ResourceRegistry::GetShaderService();
            m_TraceShader = shaders->GetByName( "PathTracer/PathTracing" );
            if ( !m_TraceShader || !m_TraceShader->IsCompiled() )
                return Common::MakeError(
                     "PathTracer: the ray generation 'PathTracer/PathTracing' did not compile or "
                     "is not registered on this device" );
            m_SkyPrepareShader = shaders->GetByName( "PathTracerSky/Prepare" );
            m_SkyReduceShader  = shaders->GetByName( "PathTracerSky/Reduce" );
            if ( !m_SkyPrepareShader || !m_SkyReduceShader )
                return Common::MakeError( "PathTracer: the sky programs 'PathTracerSky/Prepare' / "
                                          "'PathTracerSky/Reduce' are not registered" );
            const auto prepare =
                 ComputePipeline::Create( { .Shader = m_SkyPrepareShader, .DebugName = "PathTracerSkyPrepare" } );
            if ( !prepare )
                return Common::MakeError( prepare.GetError() );
            const auto reduce =
                 ComputePipeline::Create( { .Shader = m_SkyReduceShader, .DebugName = "PathTracerSkyReduce" } );
            if ( !reduce )
                return Common::MakeError( reduce.GetError() );
            m_SkyPreparePipeline = prepare.GetValue();
            m_SkyReducePipeline  = reduce.GetValue();

            m_ResolveShader = shaders->GetByName( "PathTracerResolve" );
            if ( !m_ResolveShader )
                return Common::MakeError( "PathTracer: the resolve 'PathTracerResolve' is not registered" );
            GraphicsPipelineSpecification spec;
            spec.DebugName    = "PathTracerResolve";
            spec.TargetLayout = RenderTargetLayout{
                 .ColorFormats = { ViewTargetFormats::kSceneColor }, .DepthFormat = std::nullopt, .Samples = 1 };
            spec.Shader            = m_ResolveShader;
            spec.DepthTestEnabled  = false;
            spec.DepthWriteEnabled = false;
            const auto resolve     = GraphicsPipeline::Create( spec );
            if ( !resolve )
                return Common::MakeError( resolve.GetError() );
            m_ResolvePipeline = resolve.GetValue();
            return BOOLSUCCESS;
        }

        // GRAPH BUILD, before AddPasses: the ray tracing pipeline - the "PathTracer/PathTracing" program and one
        // hit group per surface template of this frame's hit group table (@p templates =
        // RayTracingHitGroupTable::Templates, each "<Template>/RayTracing.Hit"). Rebuilt when the list changes.
        // The table holds only templates with a compiled cell, so a missing one is a broken invariant, named.
        [[nodiscard]] Common::BoolResultStr EnsurePipeline( const std::vector<std::string>& templates )
        {
            if ( m_TracePipeline && templates == m_PipelineTemplates )
                return BOOLSUCCESS;
            auto*                           shaders = Runtime::ResourceRegistry::GetShaderService();
            RayTracingPipelineSpecification spec{
                 .Program = m_TraceShader, .HitGroups = {}, .DebugName = "PathTracer" };
            for ( const std::string& name : templates )
            {
                const std::string cell = std::format( "{}/RayTracing.Hit", name );
                auto              hit  = shaders->GetByName( cell );
                if ( !hit || !hit->IsCompiled() )
                    return Common::MakeFormattedError(
                         "PathTracer: the hit group table names surface template '{}', "
                         "whose hit group '{}' is not compiled",
                         name, cell );
                spec.HitGroups.push_back( std::move( hit ) );
            }
            auto pipeline = RayTracingPipeline::Create( spec );
            if ( !pipeline )
                return Common::MakeError( pipeline.GetError() );
            m_TracePipeline     = pipeline.GetValue();
            m_PipelineTemplates = templates;
            return BOOLSUCCESS;
        }

        // A new world: nothing summed belongs to it (IRenderSystem::OnSceneReplaced).
        void OnSceneReplaced() override
        {
            Restart();
        }

        // A camera cut (IRenderSystem::OnTemporalHistoryReset).
        void OnTemporalHistoryReset() override
        {
            Restart();
        }

        // GRAPH BUILD: this frame's path tracing into @p sceneColor (the scene target's colour 0, single-sampled
        // at native scale): the restart decision, the sky and the trace when paths are still owed, and the
        // resolve, which runs every frame. An error names what was refused; nothing is declared then.
        [[nodiscard]] Common::BoolResultStr AddPasses( RDG::Builder& graph, const RDG::TextureRef sceneColor,
                                                       const ViewFrame& frame, const FrameInputs& inputs,
                                                       const Settings& settings )
        {
            if ( !m_TracePipeline || !m_ResolvePipeline )
                return Common::MakeError( "PathTracer: the pipelines were not made (Initialize, EnsurePipeline)" );
            if ( inputs.HitGroups == nullptr )
                return Common::MakeError( "PathTracer: the frame has no hit group table" );
            if ( !inputs.Emissive.Valid )
                return Common::MakeError( "PathTracer: the frame has no emissive triangle list" );
            const auto sceneDesc = graph.GetTextureDesc( sceneColor );
            if ( !sceneDesc )
                return Common::MakeError( sceneDesc.GetError() );
            if ( sceneDesc.GetValue().Samples != 1 )
                return Common::MakeFormattedError( "PathTracer: the scene colour has {} samples; the path tracer "
                                                   "writes one value per pixel and refuses a multisampled target",
                                                   sceneDesc.GetValue().Samples );
            const uint32_t width  = sceneDesc.GetValue().Size.Width;
            const uint32_t height = sceneDesc.GetValue().Size.Height;
            if ( width == 0u || height == 0u )
                return Common::MakeError( "PathTracer: the scene colour has no pixels" );
            const auto envDesc = graph.GetTextureDesc( inputs.EnvSpecular );
            if ( !envDesc )
                return Common::MakeError( envDesc.GetError() );

            m_TraceFailed = false;
            const bool resized =
                 !m_Accumulation.Built || m_Accumulation.Width != width || m_Accumulation.Height != height;
            const bool exposureLeft =
                 settings.MaxPathIntensity > 0.0f &&
                 !( std::abs( std::log2( frame.PreExposure / m_ClampPreExposure ) ) <= kClampExposureStops );
            const bool restart =
                 resized || !m_Stamp.ReadableIn( frame ) || frame.ViewProjection != frame.PrevViewProjection ||
                 settings.MaxBounces != m_MaxBounces || inputs.SceneHash != m_SceneHash || inputs.DynamicGeometry ||
                 settings.MaxPathIntensity != m_MaxPathIntensity || exposureLeft;
            if ( restart )
            {
                m_Summed = 0;
                ++m_Restarts;
                m_ClampPreExposure = frame.PreExposure;
            }
            m_MaxBounces                       = settings.MaxBounces;
            m_MaxPathIntensity                 = settings.MaxPathIntensity;
            m_SceneHash                        = inputs.SceneHash;
            const RDG::TextureRef accumulation = AcquireAccumulation( graph, width, height );

            const uint32_t owed   = settings.SamplesPerPixel > m_Summed ? settings.SamplesPerPixel - m_Summed : 0u;
            const uint32_t traced = std::min( settings.PathsPerFrame, owed );
            if ( traced > 0u )
            {
                const SkyTargets sky = AddSkyPasses( graph, inputs, envDesc.GetValue().Size.Width );
                AddTracePass( graph, accumulation, sky, inputs, width, height, traced );
                m_Summed += traced;
            }
            AddResolvePass( graph, accumulation, sceneColor, frame.FrameIndex, frame.PreExposure );
            return BOOLSUCCESS;
        }

        // Paths per pixel summed in the view's accumulation (the overlay's progress).
        [[nodiscard]] uint32_t GetSummedSamples() const
        {
            return m_Summed;
        }

    private:
        // PathTracerResolve.shader "ResolvePush".
        struct ResolvePush
        {
            float PreExposure;
        };

        struct TracePush
        {
            glm::uvec4 Counts; // point lights, spot lights, paths already summed (0: overwrite), max bounces
            glm::uvec4 Sky;    // sky side S, its levels, frame seed, paths this frame
            glm::vec4  Sun;    // sin of half the sun's angle, pixel spread angle, sin of half atmosphere light 1's angle,
                               // the path intensity clamp in radiance (MaxPathIntensity / m_ClampPreExposure; 0: none)
            glm::uvec4 Emissive;        // EmissiveLightsFrame::StaticParams
            glm::uvec4 EmissiveDynamic; // EmissiveLightsFrame::DynamicParams
        };
        struct SkyPush
        {
            glm::uvec4 Sky; // side S; Reduce: the level written
        };
        struct SkyTargets
        {
            RDG::TextureRef Radiance;
            RDG::BufferRef  Pdf;
            uint32_t        Side   = 0;
            uint32_t        Levels = 0;
        };
        struct AccumulationTarget
        {
            RDG::ExternalTexture Texture;
            bool                 Built  = false;
            uint32_t             Width  = 0;
            uint32_t             Height = 0;
        };

        static uint32_t Groups( const uint32_t texels )
        {
            return ( texels + 7u ) / 8u; // LocalSize(8, 8, 1) in both sky passes
        }

        void Restart()
        {
            m_Summed = 0;
            m_Stamp.Invalidate();
        }

        // The view's accumulation in this graph: registered when it exists at this size, created and extracted
        // (kept for the next frames' traces and resolves) when it does not. A new one is always written whole
        // before it is read: its frame restarts, so the trace overwrites (Counts.z = 0) every pixel.
        RDG::TextureRef AcquireAccumulation( RDG::Builder& graph, const uint32_t width, const uint32_t height )
        {
            const auto& frames = ::Desert::Engine::FrameManager::GetInstance();
            m_RetiringAccumulation.Collect( frames.GetAbsoluteFrameCount(), frames.GetMaxFramesInFlight() );
            if ( m_Accumulation.Built && m_Accumulation.Width == width && m_Accumulation.Height == height )
                return graph.RegisterExternal( m_Accumulation.Texture, "PathTracing.Accumulation" );
            // The previous size's sum is parked, not dropped: the frames recorded before this one may still be
            // tracing into it or resolving it on the GPU.
            if ( m_Accumulation.Texture.Physical )
                m_RetiringAccumulation.Park( std::move( m_Accumulation.Texture ), frames.GetAbsoluteFrameCount() );
            m_Accumulation.Texture = {};
            m_Stamp.Invalidate();
            const RDG::TextureRef created =
                 graph.CreateTexture( RDG::TextureDesc{ .Size   = { .Width = width, .Height = height },
                                                        .Format = ViewTargetFormats::kPathTracingAccumulation },
                                      "PathTracing.Accumulation" );
            graph.Extract( created, m_Accumulation.Texture, RDG::Access::SampledGraphics,
                           { RDG::Access::StorageWrite, RDG::Access::SampledGraphics } );
            m_Accumulation.Built  = true;
            m_Accumulation.Width  = width;
            m_Accumulation.Height = height;
            return created;
        }

        // "PathTracing: Sky Prepare" + one "PathTracing: Sky Reduce" per pyramid level above 0.
        SkyTargets AddSkyPasses( RDG::Builder& graph, const FrameInputs& inputs, const uint32_t cubeFace )
        {
            SkyTargets sky;
            sky.Side           = std::bit_ceil( std::max( cubeFace, 1u ) );
            sky.Levels         = static_cast<uint32_t>( std::bit_width( sky.Side ) ); // S, S/2, ..., 1
            uint64_t pdfFloats = 0;
            for ( uint32_t level = 0; level < sky.Levels; ++level )
                pdfFloats += static_cast<uint64_t>( sky.Side >> level ) * ( sky.Side >> level );
            sky.Radiance =
                 graph.CreateTexture( RDG::TextureDesc{ .Size   = { .Width = sky.Side, .Height = sky.Side },
                                                        .Format = ViewTargetFormats::kPathTracingSkyRadiance },
                                      "PathTracing.SkyRadiance" );
            sky.Pdf = graph.CreateBuffer( RDG::BufferDesc{ pdfFloats * sizeof( float ) }, "PathTracing.SkyPdf" );

            const SkyPush prepare{ glm::uvec4( sky.Side, 0u, 0u, 0u ) };
            graph.AddPass(
                 "PathTracing: Sky Prepare", RDG::PassFlags::Compute,
                 [&]( RDG::PassBuilder& pass )
                 {
                     pass.Bindings( m_SkyPrepareLayout.Get( m_SkyPrepareShader ),
                                    Renderer::GetPipelineRouteFill( *m_SkyPreparePipeline ) )
                          .Texture( PathTracingSkyPrepareShaderParameters::EnvironmentSpecularTexture,
                                    inputs.EnvSpecular, RDG::Access::SampledCompute, RDG::SubresourceRange::All() )
                          .Sampler( PathTracingSkyPrepareShaderParameters::LinearClampSampler,
                                    RDG::SamplerDesc::LinearClamp() )
                          .Storage( PathTracingSkyPrepareShaderParameters::SkyRadianceTexture, sky.Radiance,
                                    RDG::Access::StorageWrite )
                          .Storage( PathTracingSkyPrepareShaderParameters::PathTracingSkyPdfUB, sky.Pdf,
                                    RDG::Access::StorageWrite )
                          .Storage( PathTracingSkyPrepareShaderParameters::GIFrameUB, inputs.GIFrame,
                                    RDG::Access::StorageRead )
                          .PushConstantBytes( static_cast<uint32_t>( sizeof( SkyPush ) ) );
                 },
                 [this, prepare]( RDG::PassContext& context ) -> Common::BoolResultStr
                 {
                     DESERT_PROFILE_PASS( "PathTracing: Sky Prepare" );
                     RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                     bindings.PushConstants( &prepare, static_cast<uint32_t>( sizeof( prepare ) ) );
                     return Renderer::GetInstance().DispatchCompute(
                          bindings, *m_SkyPreparePipeline, Groups( prepare.Sky.x ), Groups( prepare.Sky.x ), 1 );
                 } );
            // Each level reads the one below it, so each is its own pass (the graph orders the buffer's writes).
            for ( uint32_t level = 1; level < sky.Levels; ++level )
            {
                const SkyPush reduce{ glm::uvec4( sky.Side, level, 0u, 0u ) };
                graph.AddPass(
                     "PathTracing: Sky Reduce", RDG::PassFlags::Compute,
                     [&]( RDG::PassBuilder& pass )
                     {
                         pass.Bindings( m_SkyReduceLayout.Get( m_SkyReduceShader ),
                                        Renderer::GetPipelineRouteFill( *m_SkyReducePipeline ) )
                              .Storage( PathTracingSkyReduceShaderParameters::PathTracingSkyPdfUB, sky.Pdf,
                                        RDG::Access::StorageWrite )
                              .PushConstantBytes( static_cast<uint32_t>( sizeof( SkyPush ) ) );
                     },
                     [this, reduce]( RDG::PassContext& context ) -> Common::BoolResultStr
                     {
                         DESERT_PROFILE_PASS( "PathTracing: Sky Reduce" );
                         RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                         bindings.PushConstants( &reduce, static_cast<uint32_t>( sizeof( reduce ) ) );
                         const uint32_t side = reduce.Sky.x >> reduce.Sky.y;
                         return Renderer::GetInstance().DispatchCompute( bindings, *m_SkyReducePipeline,
                                                                         Groups( side ), Groups( side ), 1 );
                     } );
            }
            return sky;
        }

        // "PathTracing: Trace": @p traced paths per pixel added to the accumulation (one launch per pixel).
        void AddTracePass( RDG::Builder& graph, const RDG::TextureRef accumulation, const SkyTargets& sky,
                           const FrameInputs& inputs, const uint32_t width, const uint32_t height,
                           const uint32_t traced )
        {
            const TracePush push{
                 glm::uvec4( inputs.PointLightCount, inputs.SpotLightCount, m_Summed, m_MaxBounces ),
                 glm::uvec4( sky.Side, sky.Levels, m_Restarts, traced ),
                 glm::vec4( inputs.SunSinHalfAngle, inputs.PixelSpread, inputs.Light1SinHalfAngle,
                            m_MaxPathIntensity > 0.0f ? m_MaxPathIntensity / m_ClampPreExposure : 0.0f ),
                 inputs.Emissive.StaticParams(), inputs.Emissive.DynamicParams() };
            graph.AddPass(
                 "PathTracing: Trace", RDG::PassFlags::Compute,
                 [&]( RDG::PassBuilder& pass )
                 {
                     constexpr RDG::Access kR = RDG::Access::StorageRead;
                     pass.Bindings( m_TraceLayout.Get( m_TraceShader ), RDG::OtherRouteFill{} )
                          .Uniform( PathTracingTraceShaderParameters::CameraUB, inputs.Camera )
                          .AccelerationStructure( PathTracingTraceShaderParameters::SceneTlas, inputs.Tlas )
                          .Storage( PathTracingTraceShaderParameters::AccumulationTexture, accumulation,
                                    RDG::Access::StorageWrite )
                          .Storage( PathTracingTraceShaderParameters::GIFrameUB, inputs.GIFrame, kR )
                          .Storage( PathTracingTraceShaderParameters::PointLightsUB, inputs.PointLights, kR )
                          .Storage( PathTracingTraceShaderParameters::SpotLightsUB, inputs.SpotLights, kR )
                          .Storage( PathTracingTraceShaderParameters::SkyRadianceTexture, sky.Radiance, kR )
                          .Storage( PathTracingTraceShaderParameters::PathTracingSkyPdfUB, sky.Pdf, kR )
                          .Storage( PathTracingTraceShaderParameters::EmissiveSegmentsUB,
                                    inputs.Emissive.Static.Segments, kR )
                          .Storage( PathTracingTraceShaderParameters::EmissiveLookupUB, inputs.Emissive.Lookup,
                                    kR )
                          .Storage( PathTracingTraceShaderParameters::EmissiveTrianglesUB,
                                    inputs.Emissive.Static.Triangles, kR )
                          .Storage( PathTracingTraceShaderParameters::EmissivePowerUB,
                                    inputs.Emissive.Static.Power, kR )
                          .Storage( PathTracingTraceShaderParameters::EmissiveDynamicSegmentsUB,
                                    inputs.Emissive.Dynamic.Segments, kR )
                          .Storage( PathTracingTraceShaderParameters::EmissiveDynamicTrianglesUB,
                                    inputs.Emissive.Dynamic.Triangles, kR )
                          .Storage( PathTracingTraceShaderParameters::EmissiveDynamicPowerUB,
                                    inputs.Emissive.Dynamic.Power, kR )
                          .Texture( PathTracingTraceShaderParameters::EnvironmentSpecularTexture,
                                    inputs.EnvSpecular, RDG::Access::SampledCompute, RDG::SubresourceRange::All() )
                          .Sampler( PathTracingTraceShaderParameters::LinearClampSampler,
                                    RDG::SamplerDesc::LinearClamp() )
                          .Texture( PathTracingTraceShaderParameters::CloudShadowMapTexture,
                                    inputs.CloudShadowMap, RDG::Access::SampledCompute,
                                    RDG::SubresourceRange::All() )
                          .Sampler( PathTracingTraceShaderParameters::LinearRepeatSampler,
                                    RDG::SamplerDesc::LinearRepeat() )
                          .Uniform( PathTracingTraceShaderParameters::CloudShadowUB, inputs.CloudShadow )
                          .PushConstantBytes( static_cast<uint32_t>( sizeof( TracePush ) ) );
                 },
                 [this, push, hits = inputs.HitGroups, width,
                  height]( RDG::PassContext& context ) -> Common::BoolResultStr
                 {
                     DESERT_PROFILE_PASS( "PathTracing: Trace" );
                     RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                     bindings.PushConstants( &push, static_cast<uint32_t>( sizeof( push ) ) );
                     Common::BoolResultStr traced =
                          Renderer::TraceRays( bindings, *m_TracePipeline, *hits, width, height );
                     // The sum no longer holds what m_Summed counts: the resolve does not stamp it, so the next
                     // frame restarts (PassHistoryStamp not readable).
                     if ( !traced )
                         m_TraceFailed = true;
                     return traced;
                 } );
        }

        // "PathTracing: Resolve": sum / count over the scene colour, every frame (also once converged), stored
        // pre-exposed like every other writer of the scene colour (ViewFrame::PreExposure; UE
        // PathTracingCompositingPixelShader.usf multiplies by View.PreExposure) - the tonemap divides it back out.
        void AddResolvePass( RDG::Builder& graph, const RDG::TextureRef accumulation,
                             const RDG::TextureRef sceneColor, const uint64_t frameIndex, const float preExposure )
        {
            const ResolvePush push { preExposure };
            graph.AddPass(
                 "PathTracing: Resolve", RDG::PassFlags::Raster,
                 [&]( RDG::PassBuilder& pass )
                 {
                     pass.Bindings( m_ResolveLayout.Get( m_ResolveShader ), RDG::OtherRouteFill{} )
                          .Texture( PathTracingResolveShaderParameters::AccumulationTexture, accumulation,
                                    RDG::Access::SampledGraphics, RDG::SubresourceRange::All() )
                          .Sampler( PathTracingResolveShaderParameters::PointClampSampler,
                                    RDG::SamplerDesc::PointClamp() )
                          .PushConstantBytes( static_cast<uint32_t>( sizeof( ResolvePush ) ) );
                     // Every pixel is written: what the scene colour held is not loaded.
                     pass.ColorTarget( 0, sceneColor, RDG::LoadOp::DontCare() );
                 },
                 [this, frameIndex, push]( RDG::PassContext& context ) -> Common::BoolResultStr
                 {
                     DESERT_PROFILE_PASS( "PathTracing: Resolve" );
                     RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                     bindings.PushConstants( &push, static_cast<uint32_t>( sizeof( push ) ) );
                     const Common::BoolResultStr drawn =
                          Renderer::GetInstance().DrawFullscreen( bindings, *m_ResolvePipeline, nullptr );
                     if ( drawn.IsSuccess() && !m_TraceFailed )
                         m_Stamp.Stamp( frameIndex );
                     return drawn;
                 } );
        }

        std::shared_ptr<Shader>             m_TraceShader;
        std::shared_ptr<Shader>             m_SkyPrepareShader;
        std::shared_ptr<Shader>             m_SkyReduceShader;
        std::shared_ptr<Shader>             m_ResolveShader;
        std::shared_ptr<RayTracingPipeline> m_TracePipeline;
        std::vector<std::string>            m_PipelineTemplates;
        std::shared_ptr<ComputePipeline>    m_SkyPreparePipeline;
        std::shared_ptr<ComputePipeline>    m_SkyReducePipeline;
        std::shared_ptr<GraphicsPipeline>   m_ResolvePipeline;
        // The block layouts, derived from each shader's reflection once per compile (not per frame).
        mutable ShaderBindingLayoutCache m_TraceLayout;
        mutable ShaderBindingLayoutCache m_SkyPrepareLayout;
        mutable ShaderBindingLayoutCache m_SkyReduceLayout;
        mutable ShaderBindingLayoutCache m_ResolveLayout;

        AccumulationTarget m_Accumulation;
        // Accumulations a resize replaced, kept until no frame in flight can still read them.
        Assets::FrameRetireQueue<RDG::ExternalTexture> m_RetiringAccumulation;
        PassHistoryStamp   m_Stamp;               // the view frame whose resolve last stood behind the sum
        uint32_t           m_Summed      = 0;     // paths per pixel in the sum
        uint32_t           m_MaxBounces  = 0;     // the bounces the sum was traced with
        float              m_MaxPathIntensity = 0.0f; // Settings::MaxPathIntensity the sum was clamped with
        float              m_ClampPreExposure = 1.0f; // the PreExposure that clamp was stated at (its restart's)
        uint64_t           m_SceneHash   = 0;     // FrameInputs::SceneHash the sum was traced over
        uint32_t           m_Restarts    = 0;     // the random sequence's seed: each restart draws new paths
        bool               m_TraceFailed = false; // this frame's trace was refused (set at execution)
    };
} // namespace Desert::Graphic::System
