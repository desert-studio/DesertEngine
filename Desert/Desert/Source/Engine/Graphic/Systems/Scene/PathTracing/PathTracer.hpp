#pragma once

#include <Common/Core/Logger.hpp>
#include <Common/Core/Profiler.hpp>
#include <Common/Settings/MachineSettings.hpp>
#include <Common/Settings/Scalability.hpp>
#include <Engine/Assets/FrameRetireQueue.hpp>
#include <Engine/Core/EngineContext.hpp>
#include <Engine/Core/FrameManager.hpp>
#include <Engine/Core/ShaderCompiler/ShaderVariant.hpp>
#include <Engine/Graphic/Neural/NeuralRadianceCache.hpp>
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
#include <Engine/Graphic/View/PathTracedGuides.hpp>
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
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
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
        DESERT_SHADER_PARAMETER( StorageTexture, GuideAlbedoTexture );
        DESERT_SHADER_PARAMETER( StorageTexture, GuideNormalTexture );
        DESERT_SHADER_PARAMETER( StorageTexture, DemodulatedDiffuseTexture );
        DESERT_SHADER_PARAMETER( StorageTexture, GuideSpecularTexture );
        DESERT_SHADER_PARAMETER( StorageTexture, GuideHitTexture );
        DESERT_SHADER_PARAMETER( StorageTexture, DemodulatedSpecularTexture );
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
        // PATH_TRACING_NRC=1 only (Engine/Graphic/Neural/NeuralRadianceCache.hpp).
        DESERT_SHADER_PARAMETER( StorageBuffer, NrcQueriesUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, NrcTrainingSamplesUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, NrcSelfQueriesUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, NrcCountersUB );
    };

    // The slot names this pass binds, spelled once (RDGShaderParameters.hpp).
    struct PathTracingDenoiseSnapshotShaderParameters
    {
        DESERT_SHADER_PARAMETER( StorageTexture, GuideAlbedoTexture );
        DESERT_SHADER_PARAMETER( StorageTexture, GuideNormalTexture );
        DESERT_SHADER_PARAMETER( StorageTexture, GuideSpecularTexture );
        DESERT_SHADER_PARAMETER( StorageTexture, GuideHitTexture );
        DESERT_SHADER_PARAMETER( StorageTexture, DemodulatedDiffuseTexture );
        DESERT_SHADER_PARAMETER( StorageTexture, DemodulatedSpecularTexture );
        DESERT_SHADER_PARAMETER( StorageTexture, HistoryDiffuseTexture );
        DESERT_SHADER_PARAMETER( StorageTexture, HistorySpecularTexture );
        DESERT_SHADER_PARAMETER( StorageTexture, HistoryMomentsTexture );
        DESERT_SHADER_PARAMETER( StorageTexture, SnapshotDiffuseTexture );
        DESERT_SHADER_PARAMETER( StorageTexture, SnapshotSpecularTexture );
        DESERT_SHADER_PARAMETER( StorageTexture, SnapshotMomentsTexture );
        DESERT_SHADER_PARAMETER( StorageTexture, SnapshotGuideTexture );
    };

    // The slot names this pass binds, spelled once (RDGShaderParameters.hpp).
    struct PathTracingDenoiseTemporalShaderParameters
    {
        DESERT_SHADER_PARAMETER( UniformBuffer, CameraUB );
        DESERT_SHADER_PARAMETER( StorageTexture, GuideAlbedoTexture );
        DESERT_SHADER_PARAMETER( StorageTexture, GuideNormalTexture );
        DESERT_SHADER_PARAMETER( StorageTexture, GuideSpecularTexture );
        DESERT_SHADER_PARAMETER( StorageTexture, GuideHitTexture );
        DESERT_SHADER_PARAMETER( StorageTexture, SnapshotDiffuseTexture );
        DESERT_SHADER_PARAMETER( StorageTexture, SnapshotSpecularTexture );
        DESERT_SHADER_PARAMETER( StorageTexture, SnapshotMomentsTexture );
        DESERT_SHADER_PARAMETER( StorageTexture, SnapshotGuideTexture );
        DESERT_SHADER_PARAMETER( StorageTexture, HistoryDiffuseTexture );
        DESERT_SHADER_PARAMETER( StorageTexture, HistorySpecularTexture );
        DESERT_SHADER_PARAMETER( StorageTexture, HistoryMomentsTexture );
    };

    // The slot names this pass binds, spelled once (RDGShaderParameters.hpp).
    struct PathTracingDenoiseCombineShaderParameters
    {
        DESERT_SHADER_PARAMETER( StorageTexture, GuideAlbedoTexture );
        DESERT_SHADER_PARAMETER( StorageTexture, GuideNormalTexture );
        DESERT_SHADER_PARAMETER( StorageTexture, DemodulatedDiffuseTexture );
        DESERT_SHADER_PARAMETER( StorageTexture, DemodulatedSpecularTexture );
        DESERT_SHADER_PARAMETER( StorageTexture, HistoryDiffuseTexture );
        DESERT_SHADER_PARAMETER( StorageTexture, HistorySpecularTexture );
        DESERT_SHADER_PARAMETER( StorageTexture, HistoryMomentsTexture );
        DESERT_SHADER_PARAMETER( StorageTexture, IlluminationDiffuseTexture );
        DESERT_SHADER_PARAMETER( StorageTexture, IlluminationSpecularTexture );
        DESERT_SHADER_PARAMETER( StorageTexture, HistoryLengthTexture );
    };

    // The slot names this pass binds, spelled once (RDGShaderParameters.hpp).
    struct PathTracingDenoiseAntiFireflyShaderParameters
    {
        DESERT_SHADER_PARAMETER( StorageTexture, GuideAlbedoTexture );
        DESERT_SHADER_PARAMETER( StorageTexture, GuideNormalTexture );
        DESERT_SHADER_PARAMETER( StorageTexture, IlluminationDiffuseTexture );
        DESERT_SHADER_PARAMETER( StorageTexture, IlluminationSpecularTexture );
        DESERT_SHADER_PARAMETER( StorageTexture, FilteredDiffuseTexture );
        DESERT_SHADER_PARAMETER( StorageTexture, FilteredSpecularTexture );
    };

    // The slot names this pass binds, spelled once (RDGShaderParameters.hpp).
    struct PathTracingDenoiseFilterShaderParameters
    {
        DESERT_SHADER_PARAMETER( StorageTexture, GuideAlbedoTexture );
        DESERT_SHADER_PARAMETER( StorageTexture, GuideNormalTexture );
        DESERT_SHADER_PARAMETER( StorageTexture, GuideSpecularTexture );
        DESERT_SHADER_PARAMETER( StorageTexture, HistoryLengthTexture );
        DESERT_SHADER_PARAMETER( StorageTexture, IlluminationDiffuseTexture );
        DESERT_SHADER_PARAMETER( StorageTexture, IlluminationSpecularTexture );
        DESERT_SHADER_PARAMETER( StorageTexture, FilteredDiffuseTexture );
        DESERT_SHADER_PARAMETER( StorageTexture, FilteredSpecularTexture );
    };

    // The slot names this pass binds, spelled once (RDGShaderParameters.hpp).
    struct PathTracingDenoiseRemodulateShaderParameters
    {
        DESERT_SHADER_PARAMETER( StorageTexture, AccumulationTexture );
        DESERT_SHADER_PARAMETER( StorageTexture, GuideAlbedoTexture );
        DESERT_SHADER_PARAMETER( StorageTexture, GuideSpecularTexture );
        DESERT_SHADER_PARAMETER( StorageTexture, DemodulatedDiffuseTexture );
        DESERT_SHADER_PARAMETER( StorageTexture, DemodulatedSpecularTexture );
        DESERT_SHADER_PARAMETER( StorageTexture, FilteredDiffuseTexture );
        DESERT_SHADER_PARAMETER( StorageTexture, FilteredSpecularTexture );
        DESERT_SHADER_PARAMETER( StorageTexture, DenoisedTexture );
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
    // interfaces too) is never clamped. That bounds a pixel's variance (the fireflies a small bright emitter met
    // by a diffuse bounce makes) at the price of a bias toward darker highlights. The clamp is stated in exposed
    // units, so the trace receives MaxPathIntensity / PreExposure - the PreExposure of the frame the sum
    // (re)started at (m_ClampPreExposure). A sum keeps one clamp: clamping is not linear, so paths clamped at
    // another value cannot be summed with it. In UE the clamp follows the current exposure and a change of it is a
    // change of the config; here eye adaptation moves PreExposure a little every frame (the path-traced image
    // feeds the histogram), and restarting on each move would never converge, so the sum restarts only when the
    // exposure left the one it was clamped at by more than kClampExposureStops.
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
    // THE DENOISER (PathTracing.Denoiser = Off | ReLAX | RR | Auto, PathTracingDenoiserMethod; UE
    // r.PathTracing.Denoiser): the image is clean from the first frame, not after the sum converges, and while the
    // camera moves. Each path that meets a surface also adds its first
    // surface's guides - diffuse and specular reflectance, world normal, camera distance, roughness, the first
    // bounce's length - and the light that surface reflects split by lobe, each lobe divided by its reflectance
    // per path, with their squared luminances (Common/PathTracingDenoise.glslh; UE's denoiser reads the radiance,
    // albedo and normal per pixel; the lobe split is NRD ReLAX's). Two consumers:
    //  - RR, an upscaler that denoises (ITemporalUpscaler::TakesRawLightingSignals: DLSS Ray Reconstruction, the
    //    real-time path tracers' way): the view keeps that upscaler whatever AntiAliasing.Method says, traces
    //    PathsPerFrame paths through each jittered pixel centre every frame and hands that raw estimate to it -
    //    no accumulation, the upscaler's history is the integration - with this frame's guide sums
    //    (PathTracedGuides, AddPasses' result), the factors the colour was made with, in place of the G-buffer's;
    //  - ReLAX, and RR whose runtime refused: the ReLAX-style denoiser (PathTracerDenoise.shader) runs on every
    //  frame before the resolve:
    //    per lobe, a temporal history reprojected by the camera's motion on the frames the camera moved (THE
    //    DENOISER'S HISTORY below), the variance of the integrated signal, NRD ReLAX's anti-firefly
    //    (PathTracing.Denoiser.AntiFirefly: an energy-conserving neighbourhood clamp whose reach is two deviations
    //    of the mean), an energy-conserving variance-guided a-trous, and the remodulation. Its edge-stopping width
    //    is the estimate's standard deviation, so it fades as the paths add up and the converged image is the
    //    tracer's own.
    //
    // THE DENOISER'S HISTORY: the accumulation is the temporal integration of a still camera; a moving camera
    // restarts it every frame, and the history carries the integration across that restart instead - only that
    // restart: any other (a cut, a resize, the scene, the bounces, the clamp, the denoising upscaler) empties the
    // history, as it empties the sum. On a moving frame "Denoise Snapshot" folds the previous frame's sum into the
    // history before the trace overwrites it and "Denoise Temporal" reprojects it after the trace; on a still
    // frame the history stays as the camera left it and the sum grows beside it (PathTracerDenoise.shader). It is
    // not IScreenSpaceDenoiser's work: that one reconstructs one lighting signal of the raster (diffuse,
    // reflections, shadow visibility) with the raster's G-buffer; here the signal is the whole path-traced
    // radiance with guides from the paths themselves (glass looked through, sub-pixel averaged), as UE keeps its
    // path tracer's denoiser apart from the screen-space one.
    //
    // THE NEURAL RADIANCE CACHE (PathTracing.RadianceCache Nrc; Engine/Graphic/Neural/NeuralRadianceCache.hpp is
    // the contract): the trace runs PathTracer.shader "PathTracing" built with PATH_TRACING_NRC=1, whose paths end
    // in the cache once their footprint has spread (the paper's heuristic) and whose training paths feed it; the
    // cache's passes then add the cached radiance into the sums and train it, before the denoiser and the resolve.
    // The cache changes every frame it trains, so under Nrc the sum holds this frame's paths only: it restarts
    // every frame, and that restart keeps THE DENOISER'S HISTORY as a moving camera's does (the previous frame's
    // sum folded into the history, reprojected - by no motion on a still camera), so the temporal integration of
    // the image is the denoiser's history and the cache term (THE INFERENCE WEIGHTS: a moving average) is a stable
    // input to it, not a reset. Turning the cache on or off empties the history (a method change). The cached
    // contribution goes through the same path intensity clamp as the traced ones (NrcCompositeTargets).
    // A cache that cannot run on this device (its NN kernel path refused, a program or pipeline refused) does not
    // refuse the frame: the paths run without it (the cache Off, GetRadianceCacheStatus names why), the refusal is
    // logged once per reason (ShaderService::ClaimRefusalReport for a refused shader variant). The refusal HOLDS:
    // the cache is not built again until an input it was refused under changes - PathTracing.RadianceCache (set
    // away from Nrc and back), MachineSettings::ForcedNeuralKernelPath, or ShaderService::RecompileGeneration (the
    // rule of SceneRenderer::RetryRefusedProgramsAfterRecompile: the same bytes fail the same way) - so a refused
    // cache costs one build attempt, not one per frame, and a hot-reloaded fix or a changed path takes it back on
    // the next frame. A retry refused again for the same reason is not said again. The frame's data-shape
    // refusals (AddTraceTargets, AddCachePasses) are the same held refusal: AddTraceTargets is asked before the
    // sum's restart decision, so a refusal there leaves this frame's paths without the cache; AddCachePasses
    // cannot refuse a batch (Initialize validated every one), and a refusal it still gives leaves the passes it
    // added - the sum it composited into is this frame's only (it restarts every Nrc frame), and the next frame,
    // without the cache, restarts it and empties the history (a method change).
    //
    // Not here (each is UE's and a task of its own): UE's offline denoiser plugin (OIDN), rough glass (the
    // raster's glass pass reads no roughness), participating media, a Sobol sequence, pixel filters other than a
    // box.
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
            // PathTracing.Denoiser as resolved (THE DENOISER above): never Auto, Resolve settles it.
            Common::Scalability::PathTracingDenoiserMethod Denoiser =
                 Common::Scalability::PathTracingDenoiserMethod::ReLAX;
            // PathTracing.Denoiser.AntiFirefly: ReLAX's "Denoise AntiFirefly" runs between the combine and the
            // a-trous (PathTracerDenoise.shader). It reads the integrated signal only, so the sum and the history
            // are the same with it on or off and a change restarts nothing.
            bool AntiFirefly = true;
            // PathTracing.RadianceCache (THE NEURAL RADIANCE CACHE above).
            Common::Scalability::PathTracingRadianceCacheMethod RadianceCache =
                 Common::Scalability::PathTracingRadianceCacheMethod::Off;
        };

        // Whether a path-traced view renders under DLSS Ray Reconstruction to hand the paths to it raw: the
        // resolved PathTracing.Denoiser is RR (Auto settled to it on a device that offers it). The view then runs
        // that temporal method at its render scale whatever AntiAliasing.Method says (SceneRenderer::BeginScene /
        // OnUpdate); otherwise it renders at 100 % with no temporal method (the accumulation is its own
        // anti-aliasing). A runtime that refused Ray Reconstruction after the catalog offered it is not RR here:
        // the device withdraws it (VulkanPhysicalDevice::RefreshRuntimeOffers, CapabilityCatalog::
        // RefusedAtRuntime) and Resolve settles the denoiser to ReLAX with a Fallback, so the view is exactly a
        // device without RR (100 %, no jitter, ReLAX). The frame still asks the upscaler that was made
        // (FrameInputs::RawToUpscaler) for the one frame between a refusal and the next resolution.
        [[nodiscard]] static bool KeepsDenoisingUpscaler( const Common::Scalability::ResolvedQuality& quality )
        {
            using Common::Scalability::Parameter;
            using Common::Scalability::PathTracingDenoiserMethod;
            return quality.As<PathTracingDenoiserMethod>( Parameter::PathTracingDenoiser ) ==
                   PathTracingDenoiserMethod::RR;
        }

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
            // The view's temporal upscaler denoises (ITemporalUpscaler::TakesRawLightingSignals) and
            // Settings::Denoiser is RR: this frame's paths go to it raw (THE DENOISER above).
            bool RawToUpscaler = false;
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

            for ( DenoiseProgram* program : { &m_DenoiseSnapshot, &m_DenoiseTemporal, &m_DenoiseCombine,
                                              &m_DenoiseAntiFirefly, &m_DenoiseFilter, &m_DenoiseRemodulate } )
            {
                const std::string name = std::format( "PathTracerDenoise/{}", program->Name );
                program->PassShader    = shaders->GetByName( name );
                if ( !program->PassShader )
                    return Common::MakeFormattedError( "PathTracer: the denoiser program '{}' is not registered",
                                                       name );
                const auto pipeline = ComputePipeline::Create(
                     { .Shader    = program->PassShader,
                       .DebugName = std::format( "PathTracerDenoise{}", program->Name ) } );
                if ( !pipeline )
                    return Common::MakeError( pipeline.GetError() );
                program->PassPipeline = pipeline.GetValue();
            }

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

        // THE NEURAL RADIANCE CACHE'S PIECES, made on the first Nrc frame: the cache (its programs, the NN layer's
        // kernel path) and the trace built with PATH_TRACING_NRC=1 over the same hit groups as EnsurePipeline's.
        [[nodiscard]] Common::BoolResultStr EnsureRadianceCache()
        {
            if ( !m_RadianceCache )
            {
                auto cache = std::make_unique<Engine::Neural::NeuralRadianceCache>(
                     ::Desert::EngineContext::GetInstance().GetCapabilities().Neural );
                if ( const auto made = cache->Initialize(); !made )
                {
                    m_CacheShaderRefusal = cache->GetShaderRefusal();
                    return made;
                }
                m_RadianceCache = std::move( cache );
            }
            if ( m_TracePipelineNrc && m_PipelineTemplatesNrc == m_PipelineTemplates )
                return BOOLSUCCESS;
            auto*                 shaders = Runtime::ResourceRegistry::GetShaderService();
            Core::ShaderVariant   variant;
            variant.Defines = { "PATH_TRACING_NRC=1" };
            constexpr const char* kTraceProgram = "PathTracer/PathTracing";
            auto                  acquired      = shaders->AcquireVariant( kTraceProgram, variant );
            if ( !acquired )
            {
                m_CacheShaderRefusal =
                     Engine::Neural::NeuralShaderRefusal{ .Program = kTraceProgram, .Variant = variant };
                return Common::MakeFormattedError(
                     "PathTracer: the radiance-cache trace ('{}' with PATH_TRACING_NRC=1) was refused: {}",
                     kTraceProgram, acquired.GetError() );
            }
            m_TraceShaderNrc = acquired.ExtractValue();
            RayTracingPipelineSpecification spec{
                 .Program = m_TraceShaderNrc, .HitGroups = {}, .DebugName = "PathTracerNrc" };
            for ( const std::string& name : m_PipelineTemplates )
            {
                auto hit = shaders->GetByName( std::format( "{}/RayTracing.Hit", name ) );
                if ( !hit || !hit->IsCompiled() )
                    return Common::MakeFormattedError(
                         "PathTracer: the hit group of surface template '{}' is not compiled", name );
                spec.HitGroups.push_back( std::move( hit ) );
            }
            auto pipeline = RayTracingPipeline::Create( spec );
            if ( !pipeline )
                return Common::MakeError( pipeline.GetError() );
            m_TracePipelineNrc     = pipeline.GetValue();
            m_PipelineTemplatesNrc = m_PipelineTemplates;
            return BOOLSUCCESS;
        }

        // The inputs a cache refusal is held under besides PathTracing.RadianceCache itself (THE NEURAL RADIANCE
        // CACHE above).
        struct CacheInputs
        {
            std::optional<Common::Settings::NeuralKernelPath>
                     ForcedPath;     ///< MachineSettings::ForcedNeuralKernelPath
            uint64_t Generation = 0; ///< ShaderService::RecompileGeneration

            bool operator==( const CacheInputs& ) const = default;
        };

        // CacheInputs as they are this frame.
        [[nodiscard]] static CacheInputs CurrentCacheInputs()
        {
            return CacheInputs{ .ForcedPath = Common::Settings::MachineSettings::Get().ForcedNeuralKernelPath,
                                .Generation =
                                     Runtime::ResourceRegistry::GetShaderService()->RecompileGeneration() };
        }

        // The cache's refusal @p reason this frame under @p inputs: held as the standing refusal (no build is
        // attempted while it stands) and logged when it is the one to report. ShaderService::ClaimRefusalReport
        // answers for a refused shader variant (so a recompile that refuses again is said again); any other is
        // said unless it is the retried refusal's reason again.
        void RefuseRadianceCache( const std::string& reason, const CacheInputs& inputs )
        {
            m_CacheRefusal = CacheRefusal{
                 .Reason = reason, .Shader = std::exchange( m_CacheShaderRefusal, {} ), .Inputs = inputs };
            bool report = false;
            if ( m_CacheRefusal->Shader )
                report = Runtime::ResourceRegistry::GetShaderService()->ClaimRefusalReport(
                     m_CacheRefusal->Shader->Program, m_CacheRefusal->Shader->Variant );
            else
                report = !m_RetriedCacheRefusal || m_RetriedCacheRefusal->Reason != reason;
            m_RetriedCacheRefusal.reset();
            if ( report )
                LOG_WARN( "[PathTracer] PathTracing.RadianceCache Nrc refused, the paths run without the cache "
                          "(Off): {}",
                          reason );
        }

        // A new world: nothing summed belongs to it (IRenderSystem::OnSceneReplaced).
        void OnSceneReplaced() override
        {
            Restart();
            // What the cache learned is the old world's light.
            if ( m_RadianceCache )
                m_RadianceCache->Reset();
        }

        // A camera cut (IRenderSystem::OnTemporalHistoryReset).
        void OnTemporalHistoryReset() override
        {
            Restart();
        }

        // GRAPH BUILD: this frame's path tracing into @p sceneColor (the scene target's colour 0, single-sampled;
        // native scale, or the render extent under a denoising upscaler): the restart decision, the sky and the
        // trace when paths are still owed, the denoiser and the resolve, which run every frame. An error names
        // what was refused; nothing is declared then. The result is this frame's guide sums when the paths went
        // to the upscaler raw (FrameInputs::RawToUpscaler; the upscaler demodulates with them), else invalid.
        [[nodiscard]] Common::ResultStr<PathTracedGuides> AddPasses( RDG::Builder&         graph,
                                                                     const RDG::TextureRef sceneColor,
                                                                     const ViewFrame& frame, const FrameInputs& inputs,
                                                                     const Settings& settings )
        {
            using Refused = PathTracedGuides;
            if ( !m_TracePipeline || !m_ResolvePipeline )
                return Common::MakeFormattedError<Refused>(
                     "PathTracer: the pipelines were not made (Initialize, EnsurePipeline)" );
            if ( inputs.HitGroups == nullptr )
                return Common::MakeFormattedError<Refused>( "PathTracer: the frame has no hit group table" );
            if ( !inputs.Emissive.Valid )
                return Common::MakeFormattedError<Refused>( "PathTracer: the frame has no emissive triangle list" );
            const auto sceneDesc = graph.GetTextureDesc( sceneColor );
            if ( !sceneDesc )
                return Common::MakeFormattedError<Refused>( "{}", sceneDesc.GetError() );
            if ( sceneDesc.GetValue().Samples != 1 )
                return Common::MakeFormattedError<Refused>(
                     "PathTracer: the scene colour has {} samples; the path tracer writes one value per pixel and "
                     "refuses a multisampled target",
                     sceneDesc.GetValue().Samples );
            const uint32_t width  = sceneDesc.GetValue().Size.Width;
            const uint32_t height = sceneDesc.GetValue().Size.Height;
            if ( width == 0u || height == 0u )
                return Common::MakeFormattedError<Refused>( "PathTracer: the scene colour has no pixels" );
            const auto envDesc = graph.GetTextureDesc( inputs.EnvSpecular );
            if ( !envDesc )
                return Common::MakeFormattedError<Refused>( "{}", envDesc.GetError() );

            m_TraceFailed = false;
            // THE NEURAL RADIANCE CACHE: made on the first Nrc frame; a cache that cannot run leaves the paths
            // without it (Off, the reason held and logged once: RefuseRadianceCache), never the frame refused. A
            // held refusal is retried only once an input it was refused under changed.
            m_NrcRequested = settings.RadianceCache == Common::Scalability::PathTracingRadianceCacheMethod::Nrc;
            const CacheInputs cacheInputs = CurrentCacheInputs();
            if ( m_CacheRefusal && ( !m_NrcRequested || m_CacheRefusal->Inputs != cacheInputs ) )
                m_RetriedCacheRefusal = std::exchange( m_CacheRefusal, std::nullopt );
            bool nrc = m_NrcRequested && !m_CacheRefusal;
            if ( nrc )
                if ( const auto ready = EnsureRadianceCache(); !ready )
                {
                    RefuseRadianceCache( ready.GetError(), cacheInputs );
                    nrc = false;
                }
            // The cache's targets for this frame's trace, asked before the sum's restart decision so a refusal
            // leaves this frame's paths without the cache. Under Nrc the sum restarts every frame, so the frame
            // traces min(PathsPerFrame, SamplesPerPixel) paths (none: nothing to cache, no targets).
            std::optional<Engine::Neural::NrcTraceTargets> nrcTargets;
            if ( const uint32_t nrcPaths = std::min( settings.PathsPerFrame, settings.SamplesPerPixel );
                 nrc && nrcPaths > 0u )
            {
                auto targets = m_RadianceCache->AddTraceTargets( graph, width, height, nrcPaths, m_NrcFrames++ );
                if ( !targets )
                {
                    RefuseRadianceCache( std::format( "PathTracer: {}", targets.GetError() ), cacheInputs );
                    nrc = false;
                }
                else
                    nrcTargets = targets.ExtractValue();
            }
            const bool resized =
                 !m_Accumulation.Built || m_Accumulation.Width != width || m_Accumulation.Height != height;
            const bool exposureLeft =
                 settings.MaxPathIntensity > 0.0f &&
                 !( std::abs( std::log2( frame.PreExposure / m_ClampPreExposure ) ) <= kClampExposureStops );
            // Every restart but the camera's motion: the previous sum is not this frame's scene (THE DENOISER'S
            // HISTORY empties with it).
            const bool sumKept = !resized && m_Stamp.ReadableIn( frame ) && settings.MaxBounces == m_MaxBounces &&
                                 inputs.SceneHash == m_SceneHash && !inputs.DynamicGeometry &&
                                 settings.MaxPathIntensity == m_MaxPathIntensity && !exposureLeft &&
                                 !inputs.RawToUpscaler && !m_RawToUpscaler && nrc == m_Nrc;
            const bool cameraMoved = frame.ViewProjection != frame.PrevViewProjection;
            // Under Nrc the sum restarts every frame (THE NEURAL RADIANCE CACHE above) and keeps the history.
            const bool restart     = !sumKept || cameraMoved || nrc;
            const bool denoise     = settings.Denoiser != Common::Scalability::PathTracingDenoiserMethod::Off &&
                                 !inputs.RawToUpscaler;
            // The previous frame's sum is folded into the history and reprojected (THE DENOISER'S HISTORY).
            // A history made in this graph (the denoiser just turned on) is written by the combine before any read.
            const bool reproject = denoise && sumKept && ( cameraMoved || nrc ) && m_Summed > 0u && m_History.Built &&
                                   m_History.Width == width && m_History.Height == height;
            if ( !denoise || !sumKept )
            {
                if ( m_HistoryValid && denoise )
                    LOG_INFO( "[PathTracer] denoiser history reset (resized {}, stamp {}, bounces {}, scene {}, "
                              "dynamic geometry {}, clamp {}, exposure {}, raw to upscaler {}, radiance cache {})",
                              resized, !m_Stamp.ReadableIn( frame ), settings.MaxBounces != m_MaxBounces,
                              inputs.SceneHash != m_SceneHash, inputs.DynamicGeometry,
                              settings.MaxPathIntensity != m_MaxPathIntensity, exposureLeft,
                              inputs.RawToUpscaler || m_RawToUpscaler, nrc != m_Nrc );
                m_HistoryValid = false;
            }
            if ( restart )
            {
                m_Summed = 0;
                ++m_Restarts;
                m_ClampPreExposure = frame.PreExposure;
            }
            m_MaxBounces                       = settings.MaxBounces;
            m_MaxPathIntensity                 = settings.MaxPathIntensity;
            m_SceneHash                        = inputs.SceneHash;
            m_RawToUpscaler                     = inputs.RawToUpscaler;
            m_Nrc                               = nrc;
            const AccumulationRefs accumulation = AcquireAccumulation( graph, width, height );
            const HistoryRefs      history = denoise ? AcquireHistory( graph, width, height ) : HistoryRefs{};
            const SnapshotRefs     snapshot =
                 reproject ? AddDenoiseSnapshotPass( graph, accumulation, history, width, height ) : SnapshotRefs{};

            const uint32_t owed   = settings.SamplesPerPixel > m_Summed ? settings.SamplesPerPixel - m_Summed : 0u;
            const uint32_t traced = std::min( settings.PathsPerFrame, owed );
            if ( traced > 0u )
            {
                const SkyTargets sky = AddSkyPasses( graph, inputs, envDesc.GetValue().Size.Width );
                // Under Nrc the sum restarted, so traced is the nrcPaths the targets were made for.
                if ( nrcTargets )
                {
                    AddTracePass( graph, accumulation, sky, inputs, width, height, traced, &*nrcTargets );
                    const Engine::Neural::NrcCompositeTargets composite{
                         .Radiance            = accumulation.Radiance,
                         .DemodulatedDiffuse  = accumulation.DemodulatedDiffuse,
                         .DemodulatedSpecular = accumulation.DemodulatedSpecular,
                         .PathIntensityClamp =
                              m_MaxPathIntensity > 0.0f ? m_MaxPathIntensity / m_ClampPreExposure : 0.0f };
                    if ( const auto cached = m_RadianceCache->AddCachePasses( graph, *nrcTargets, composite );
                         !cached )
                        RefuseRadianceCache( std::format( "PathTracer: {}", cached.GetError() ), cacheInputs );
                    else
                        m_RetriedCacheRefusal.reset(); // the cache ran: a later refusal is a new one, and is said
                }
                else
                    AddTracePass( graph, accumulation, sky, inputs, width, height, traced, nullptr );
                m_Summed += traced;
            }
            if ( reproject )
            {
                AddDenoiseTemporalPass( graph, inputs.Camera, accumulation, snapshot, history, width, height );
                m_HistoryValid = true;
            }
            const RDG::TextureRef image =
                 denoise ? AddDenoisePasses( graph, accumulation, history, settings.AntiFirefly, width, height )
                         : accumulation.Radiance;
            AddResolvePass( graph, image, sceneColor, frame.FrameIndex, frame.PreExposure );
            // Raw to the upscaler the sum is this frame's paths alone (sumKept is false), so its guide sums are this
            // frame's - the ones the colour just resolved was made with.
            if ( !inputs.RawToUpscaler )
                return Common::MakeSuccess( PathTracedGuides{} );
            return Common::MakeSuccess( PathTracedGuides{ .Albedo   = accumulation.GuideAlbedo,
                                                          .Normal   = accumulation.GuideNormal,
                                                          .Specular = accumulation.GuideSpecular,
                                                          .Hit      = accumulation.GuideHit } );
        }

        // Paths per pixel summed in the view's accumulation (the overlay's progress).
        [[nodiscard]] uint32_t GetSummedSamples() const
        {
            return m_Summed;
        }

        // PathTracing.RadianceCache as the last frame ran it: Requested the setting, Effective what the paths ran
        // (Off when the cache was refused), Reason the standing refusal (empty when the cache ran or was not
        // asked), RetryOn what the held refusal waits for - the inputs whose change builds the cache again, with
        // the values it holds at (empty with no refusal).
        struct RadianceCacheStatus
        {
            Common::Scalability::PathTracingRadianceCacheMethod Requested =
                 Common::Scalability::PathTracingRadianceCacheMethod::Off;
            Common::Scalability::PathTracingRadianceCacheMethod Effective =
                 Common::Scalability::PathTracingRadianceCacheMethod::Off;
            std::string Reason;
            std::string RetryOn;
        };
        [[nodiscard]] RadianceCacheStatus GetRadianceCacheStatus() const
        {
            using Method = Common::Scalability::PathTracingRadianceCacheMethod;
            RadianceCacheStatus status{ .Requested = m_NrcRequested ? Method::Nrc : Method::Off,
                                        .Effective = m_Nrc ? Method::Nrc : Method::Off };
            if ( m_NrcRequested && m_CacheRefusal )
            {
                const CacheInputs& held   = m_CacheRefusal->Inputs;
                const char*        forced = "none";
                if ( held.ForcedPath )
                    forced = Engine::NeuralKernelPathName( *held.ForcedPath );
                status.Reason  = m_CacheRefusal->Reason;
                status.RetryOn = std::format( "PathTracing.RadianceCache set away from Nrc and back; MachineSettings "
                                              "ForcedNeuralKernelPath changed (held at {}); a shader recompile "
                                              "(ShaderService::RecompileGeneration, held at {})",
                                              forced, held.Generation );
            }
            return status;
        }

        // The neural radiance cache's read-back (DesertCtl state "nrc"); nullptr while no Nrc frame made one.
        [[nodiscard]] const Engine::Neural::NrcStats* GetRadianceCacheStats() const
        {
            return m_RadianceCache ? &m_RadianceCache->GetStats() : nullptr;
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
            glm::uvec4 Camera; // 1: camera rays through the pixel centre (FrameInputs::RawToUpscaler), unused x3
            // NeuralRadianceCache.hpp NrcTraceParams (zero without the cache).
            glm::uvec4 NrcParams;
            glm::vec4  NrcSpread;
        };
        static_assert( sizeof( TracePush ) == 128, "the Vulkan guaranteed push-constant size" );
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
        // PathTracerDenoise.shader "Snapshot" / "Combine" (x = 1: the history holds one) and "Filter" (x = the tap
        // spacing in pixels).
        struct DenoisePush
        {
            glm::uvec4 Value;
        };
        // One pass of PathTracerDenoise.shader: its program, pipeline and block layout.
        struct DenoiseProgram
        {
            const char*                      Name = "";
            std::shared_ptr<Shader>          PassShader;
            std::shared_ptr<ComputePipeline> PassPipeline;
            mutable ShaderBindingLayoutCache PassLayout;
        };
        // The view's sums in this graph: the radiance (rgb, a = paths summed) and the denoiser's guide and lobe
        // sums (Common/PathTracingDenoise.glslh).
        struct AccumulationRefs
        {
            RDG::TextureRef Radiance;
            RDG::TextureRef GuideAlbedo;
            RDG::TextureRef GuideNormal;
            RDG::TextureRef GuideSpecular;
            RDG::TextureRef GuideHit;
            RDG::TextureRef DemodulatedDiffuse;
            RDG::TextureRef DemodulatedSpecular;
        };
        struct AccumulationTarget
        {
            RDG::ExternalTexture Texture;
            RDG::ExternalTexture GuideAlbedo;
            RDG::ExternalTexture GuideNormal;
            RDG::ExternalTexture GuideSpecular;
            RDG::ExternalTexture GuideHit;
            RDG::ExternalTexture DemodulatedDiffuse;
            RDG::ExternalTexture DemodulatedSpecular;
            bool                 Built  = false;
            uint32_t             Width  = 0;
            uint32_t             Height = 0;
        };
        // THE DENOISER'S HISTORY (PathTracerDenoise.shader): per lobe the base illumination (rgb, a = its length in
        // paths) and the moments (diffuse m2, specular m2, diffuse fast, specular fast) the history adds to the
        // accumulation at the current pose.
        struct HistoryRefs
        {
            RDG::TextureRef Diffuse;
            RDG::TextureRef Specular;
            RDG::TextureRef Moments;
        };
        struct HistoryTarget
        {
            RDG::ExternalTexture Diffuse;
            RDG::ExternalTexture Specular;
            RDG::ExternalTexture Moments;
            bool                 Built  = false;
            uint32_t             Width  = 0;
            uint32_t             Height = 0;
        };
        // "Denoise Snapshot"'s output: the previous frame's integrated signal at the previous pose and its guide.
        struct SnapshotRefs
        {
            RDG::TextureRef Diffuse;
            RDG::TextureRef Specular;
            RDG::TextureRef Moments;
            RDG::TextureRef Guide;
        };

        static uint32_t Groups( const uint32_t texels )
        {
            return ( texels + 7u ) / 8u; // LocalSize(8, 8, 1) in the sky and the denoiser passes
        }

        // SVGF's a-trous iterations (Schied et al. 2017, 4.3): taps 1, 2, 4, 8, 16 pixels apart, a 5 x 5 kernel
        // each, so the last one reaches 32 pixels - the footprint the paper filters one path per pixel with.
        static constexpr uint32_t kDenoiseIterations = 5;

        void Restart()
        {
            m_Summed = 0;
            m_Stamp.Invalidate();
        }

        // The view's accumulation in this graph: registered when it exists at this size, created and extracted
        // (kept for the next frames' traces and resolves) when it does not. A new one is always written whole
        // before it is read: its frame restarts, so the trace overwrites (Counts.z = 0) every pixel.
        AccumulationRefs AcquireAccumulation( RDG::Builder& graph, const uint32_t width, const uint32_t height )
        {
            const auto& frames = ::Desert::Engine::FrameManager::GetInstance();
            m_RetiringAccumulation.Collect( frames.GetAbsoluteFrameCount(), frames.GetMaxFramesInFlight() );
            if ( m_Accumulation.Built && m_Accumulation.Width == width && m_Accumulation.Height == height )
                return { .Radiance = graph.RegisterExternal( m_Accumulation.Texture, "PathTracing.Accumulation" ),
                         .GuideAlbedo =
                              graph.RegisterExternal( m_Accumulation.GuideAlbedo, "PathTracing.GuideAlbedo" ),
                         .GuideNormal =
                              graph.RegisterExternal( m_Accumulation.GuideNormal, "PathTracing.GuideNormal" ),
                         .GuideSpecular =
                              graph.RegisterExternal( m_Accumulation.GuideSpecular, "PathTracing.GuideSpecular" ),
                         .GuideHit = graph.RegisterExternal( m_Accumulation.GuideHit, "PathTracing.GuideHit" ),
                         .DemodulatedDiffuse = graph.RegisterExternal( m_Accumulation.DemodulatedDiffuse,
                                                                       "PathTracing.DemodulatedDiffuse" ),
                         .DemodulatedSpecular = graph.RegisterExternal( m_Accumulation.DemodulatedSpecular,
                                                                        "PathTracing.DemodulatedSpecular" ) };
            // The previous size's sums are parked, not dropped: the frames recorded before this one may still be
            // tracing into them or resolving them on the GPU.
            for ( RDG::ExternalTexture* texture :
                  { &m_Accumulation.Texture, &m_Accumulation.GuideAlbedo, &m_Accumulation.GuideNormal,
                    &m_Accumulation.GuideSpecular, &m_Accumulation.GuideHit, &m_Accumulation.DemodulatedDiffuse,
                    &m_Accumulation.DemodulatedSpecular } )
            {
                if ( texture->Physical )
                    m_RetiringAccumulation.Park( std::move( *texture ), frames.GetAbsoluteFrameCount() );
                *texture = {};
            }
            m_Stamp.Invalidate();
            const RDG::TextureDesc desc{ .Size   = { .Width = width, .Height = height },
                                         .Format = ViewTargetFormats::kPathTracingAccumulation };
            const AccumulationRefs created{ .Radiance    = graph.CreateTexture( desc, "PathTracing.Accumulation" ),
                                            .GuideAlbedo = graph.CreateTexture( desc, "PathTracing.GuideAlbedo" ),
                                            .GuideNormal = graph.CreateTexture( desc, "PathTracing.GuideNormal" ),
                                            .GuideSpecular =
                                                 graph.CreateTexture( desc, "PathTracing.GuideSpecular" ),
                                            .GuideHit = graph.CreateTexture( desc, "PathTracing.GuideHit" ),
                                            .DemodulatedDiffuse =
                                                 graph.CreateTexture( desc, "PathTracing.DemodulatedDiffuse" ),
                                            .DemodulatedSpecular =
                                                 graph.CreateTexture( desc, "PathTracing.DemodulatedSpecular" ) };
            // Later graphs trace into all seven, the denoiser reads them, the resolve samples the radiance.
            graph.Extract( created.Radiance, m_Accumulation.Texture, RDG::Access::SampledGraphics,
                           { RDG::Access::StorageWrite, RDG::Access::StorageRead, RDG::Access::SampledGraphics } );
            graph.Extract( created.GuideAlbedo, m_Accumulation.GuideAlbedo, RDG::Access::StorageRead,
                           { RDG::Access::StorageWrite, RDG::Access::StorageRead } );
            for ( const auto& [ref, target] : { std::pair{ created.GuideNormal, &m_Accumulation.GuideNormal },
                                                std::pair{ created.GuideSpecular, &m_Accumulation.GuideSpecular },
                                                std::pair{ created.GuideHit, &m_Accumulation.GuideHit },
                                                std::pair{ created.DemodulatedDiffuse,
                                                           &m_Accumulation.DemodulatedDiffuse },
                                                std::pair{ created.DemodulatedSpecular,
                                                           &m_Accumulation.DemodulatedSpecular } } )
                graph.Extract( ref, *target, RDG::Access::StorageRead,
                               { RDG::Access::StorageWrite, RDG::Access::StorageRead } );
            m_Accumulation.Built  = true;
            m_Accumulation.Width  = width;
            m_Accumulation.Height = height;
            return created;
        }

        // THE DENOISER'S HISTORY in this graph, as AcquireAccumulation: a new one holds no history (m_HistoryValid
        // false), and "Denoise Combine" clears it before anything reads it.
        HistoryRefs AcquireHistory( RDG::Builder& graph, const uint32_t width, const uint32_t height )
        {
            if ( m_History.Built && m_History.Width == width && m_History.Height == height )
                return { .Diffuse  = graph.RegisterExternal( m_History.Diffuse, "PathTracing.HistoryDiffuse" ),
                         .Specular = graph.RegisterExternal( m_History.Specular, "PathTracing.HistorySpecular" ),
                         .Moments  = graph.RegisterExternal( m_History.Moments, "PathTracing.HistoryMoments" ) };
            const auto& frames = ::Desert::Engine::FrameManager::GetInstance();
            for ( RDG::ExternalTexture* texture : { &m_History.Diffuse, &m_History.Specular, &m_History.Moments } )
            {
                if ( texture->Physical )
                    m_RetiringAccumulation.Park( std::move( *texture ), frames.GetAbsoluteFrameCount() );
                *texture = {};
            }
            m_HistoryValid = false;
            const RDG::TextureDesc desc{ .Size   = { .Width = width, .Height = height },
                                         .Format = ViewTargetFormats::kPathTracingDenoise };
            const HistoryRefs created{ .Diffuse  = graph.CreateTexture( desc, "PathTracing.HistoryDiffuse" ),
                                       .Specular = graph.CreateTexture( desc, "PathTracing.HistorySpecular" ),
                                       .Moments  = graph.CreateTexture( desc, "PathTracing.HistoryMoments" ) };
            // Later graphs fold, reproject and combine them (read and written in place).
            for ( const auto& [ref, target] : { std::pair{ created.Diffuse, &m_History.Diffuse },
                                                std::pair{ created.Specular, &m_History.Specular },
                                                std::pair{ created.Moments, &m_History.Moments } } )
                graph.Extract( ref, *target, RDG::Access::StorageWrite,
                               { RDG::Access::StorageWrite, RDG::Access::StorageRead } );
            m_History.Built  = true;
            m_History.Width  = width;
            m_History.Height = height;
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
        // @p nrc: this frame's cache targets (the PATH_TRACING_NRC=1 trace writes them), or nullptr.
        void AddTracePass( RDG::Builder& graph, const AccumulationRefs& accumulation, const SkyTargets& sky,
                           const FrameInputs& inputs, const uint32_t width, const uint32_t height,
                           const uint32_t traced, const Engine::Neural::NrcTraceTargets* nrc )
        {
            const std::shared_ptr<Shader>&             shader   = nrc ? m_TraceShaderNrc : m_TraceShader;
            const std::shared_ptr<RayTracingPipeline>& pipeline = nrc ? m_TracePipelineNrc : m_TracePipeline;
            ShaderBindingLayoutCache&                  layout   = nrc ? m_TraceLayoutNrc : m_TraceLayout;
            const TracePush push{
                 glm::uvec4( inputs.PointLightCount, inputs.SpotLightCount, m_Summed, m_MaxBounces ),
                 glm::uvec4( sky.Side, sky.Levels, m_Restarts, traced ),
                 glm::vec4( inputs.SunSinHalfAngle, inputs.PixelSpread, inputs.Light1SinHalfAngle,
                            m_MaxPathIntensity > 0.0f ? m_MaxPathIntensity / m_ClampPreExposure : 0.0f ),
                 inputs.Emissive.StaticParams(),
                 inputs.Emissive.DynamicParams(),
                 glm::uvec4( inputs.RawToUpscaler ? 1u : 0u, 0u, 0u, 0u ),
                 nrc ? nrc->Push.Params : glm::uvec4( 0u ),
                 nrc ? nrc->Push.Spread : glm::vec4( 0.0f ) };
            graph.AddPass(
                 "PathTracing: Trace", RDG::PassFlags::Compute,
                 [&]( RDG::PassBuilder& pass )
                 {
                     constexpr RDG::Access kR = RDG::Access::StorageRead;
                     auto&& block = pass.Bindings( layout.Get( shader ), RDG::OtherRouteFill{} );
                     if ( nrc )
                         block.Storage( PathTracingTraceShaderParameters::NrcQueriesUB, nrc->Queries,
                                        RDG::Access::StorageWrite )
                              .Storage( PathTracingTraceShaderParameters::NrcTrainingSamplesUB, nrc->TrainingSamples,
                                        RDG::Access::StorageWrite )
                              .Storage( PathTracingTraceShaderParameters::NrcSelfQueriesUB, nrc->SelfQueries,
                                        RDG::Access::StorageWrite )
                              .Storage( PathTracingTraceShaderParameters::NrcCountersUB, nrc->Counters,
                                        RDG::Access::StorageWrite );
                     block
                          .Uniform( PathTracingTraceShaderParameters::CameraUB, inputs.Camera )
                          .AccelerationStructure( PathTracingTraceShaderParameters::SceneTlas, inputs.Tlas )
                          .Storage( PathTracingTraceShaderParameters::AccumulationTexture, accumulation.Radiance,
                                    RDG::Access::StorageWrite )
                          .Storage( PathTracingTraceShaderParameters::GuideAlbedoTexture, accumulation.GuideAlbedo,
                                    RDG::Access::StorageWrite )
                          .Storage( PathTracingTraceShaderParameters::GuideNormalTexture, accumulation.GuideNormal,
                                    RDG::Access::StorageWrite )
                          .Storage( PathTracingTraceShaderParameters::GuideSpecularTexture,
                                    accumulation.GuideSpecular, RDG::Access::StorageWrite )
                          .Storage( PathTracingTraceShaderParameters::GuideHitTexture, accumulation.GuideHit,
                                    RDG::Access::StorageWrite )
                          .Storage( PathTracingTraceShaderParameters::DemodulatedDiffuseTexture,
                                    accumulation.DemodulatedDiffuse, RDG::Access::StorageWrite )
                          .Storage( PathTracingTraceShaderParameters::DemodulatedSpecularTexture,
                                    accumulation.DemodulatedSpecular, RDG::Access::StorageWrite )
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
                          .Texture( PathTracingTraceShaderParameters::CloudShadowMapTexture, inputs.CloudShadowMap,
                                    RDG::Access::SampledCompute, RDG::SubresourceRange::All() )
                          .Sampler( PathTracingTraceShaderParameters::LinearRepeatSampler,
                                    RDG::SamplerDesc::LinearRepeat() )
                          .Uniform( PathTracingTraceShaderParameters::CloudShadowUB, inputs.CloudShadow )
                          .PushConstantBytes( static_cast<uint32_t>( sizeof( TracePush ) ) );
                 },
                 [this, push, pipeline, hits = inputs.HitGroups, width,
                  height]( RDG::PassContext& context ) -> Common::BoolResultStr
                 {
                     DESERT_PROFILE_PASS( "PathTracing: Trace" );
                     RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                     bindings.PushConstants( &push, static_cast<uint32_t>( sizeof( push ) ) );
                     Common::BoolResultStr traced =
                          Renderer::TraceRays( bindings, *pipeline, *hits, width, height );
                     // The sum no longer holds what m_Summed counts: the resolve does not stamp it, so the next
                     // frame restarts (PassHistoryStamp not readable).
                     if ( !traced )
                         m_TraceFailed = true;
                     return traced;
                 } );
        }

        // The exec of one PathTracerDenoise.shader pass: its push (when the pass has one) and one dispatch over the
        // view.
        Common::BoolResultStr DispatchDenoise( RDG::PassContext& context, const DenoiseProgram& program,
                                               const DenoisePush* push, const uint32_t width,
                                               const uint32_t height ) const
        {
            RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
            if ( push != nullptr )
                bindings.PushConstants( push, static_cast<uint32_t>( sizeof( DenoisePush ) ) );
            return Renderer::GetInstance().DispatchCompute( bindings, *program.PassPipeline, Groups( width ),
                                                            Groups( height ), 1 );
        }

        // "PathTracing: Denoise Snapshot", BEFORE the trace of a moving frame (THE DENOISER'S HISTORY): the
        // previous frame's sum folded into the history at the previous pose, with its guides.
        SnapshotRefs AddDenoiseSnapshotPass( RDG::Builder& graph, const AccumulationRefs& accumulation,
                                             const HistoryRefs& history, const uint32_t width,
                                             const uint32_t height )
        {
            constexpr RDG::Access  kR = RDG::Access::StorageRead;
            constexpr RDG::Access  kW = RDG::Access::StorageWrite;
            const RDG::TextureDesc desc{ .Size   = { .Width = width, .Height = height },
                                         .Format = ViewTargetFormats::kPathTracingDenoise };
            const SnapshotRefs     snapshot{
                     .Diffuse  = graph.CreateTexture( desc, "PathTracing.SnapshotDiffuse" ),
                     .Specular = graph.CreateTexture( desc, "PathTracing.SnapshotSpecular" ),
                     .Moments  = graph.CreateTexture( desc, "PathTracing.SnapshotMoments" ),
                     .Guide    = graph.CreateTexture( desc, "PathTracing.SnapshotGuide" ) };
            const DenoisePush push{ glm::uvec4( m_HistoryValid ? 1u : 0u, 0u, 0u, 0u ) };
            graph.AddPass(
                 "PathTracing: Denoise Snapshot", RDG::PassFlags::Compute,
                 [&]( RDG::PassBuilder& pass )
                 {
                     using P = PathTracingDenoiseSnapshotShaderParameters;
                     pass.Bindings( m_DenoiseSnapshot.PassLayout.Get( m_DenoiseSnapshot.PassShader ),
                                    Renderer::GetPipelineRouteFill( *m_DenoiseSnapshot.PassPipeline ) )
                          .Storage( P::GuideAlbedoTexture, accumulation.GuideAlbedo, kR )
                          .Storage( P::GuideNormalTexture, accumulation.GuideNormal, kR )
                          .Storage( P::GuideSpecularTexture, accumulation.GuideSpecular, kR )
                          .Storage( P::GuideHitTexture, accumulation.GuideHit, kR )
                          .Storage( P::DemodulatedDiffuseTexture, accumulation.DemodulatedDiffuse, kR )
                          .Storage( P::DemodulatedSpecularTexture, accumulation.DemodulatedSpecular, kR )
                          .Storage( P::HistoryDiffuseTexture, history.Diffuse, kR )
                          .Storage( P::HistorySpecularTexture, history.Specular, kR )
                          .Storage( P::HistoryMomentsTexture, history.Moments, kR )
                          .Storage( P::SnapshotDiffuseTexture, snapshot.Diffuse, kW )
                          .Storage( P::SnapshotSpecularTexture, snapshot.Specular, kW )
                          .Storage( P::SnapshotMomentsTexture, snapshot.Moments, kW )
                          .Storage( P::SnapshotGuideTexture, snapshot.Guide, kW )
                          .PushConstantBytes( static_cast<uint32_t>( sizeof( DenoisePush ) ) );
                 },
                 [this, push, width, height]( RDG::PassContext& context ) -> Common::BoolResultStr
                 {
                     DESERT_PROFILE_PASS( "PathTracing: Denoise Snapshot" );
                     return DispatchDenoise( context, m_DenoiseSnapshot, &push, width, height );
                 } );
            return snapshot;
        }

        // "PathTracing: Denoise Temporal", AFTER the trace of a moving frame: the snapshot reprojected onto this
        // frame's surfaces by the camera's motion (@p camera: the frame's CameraUB, its Prev* fields) - the new
        // history.
        void AddDenoiseTemporalPass( RDG::Builder& graph, const RDG::BufferRef camera,
                                     const AccumulationRefs& accumulation, const SnapshotRefs& snapshot,
                                     const HistoryRefs& history, const uint32_t width, const uint32_t height )
        {
            constexpr RDG::Access kR = RDG::Access::StorageRead;
            constexpr RDG::Access kW = RDG::Access::StorageWrite;
            graph.AddPass(
                 "PathTracing: Denoise Temporal", RDG::PassFlags::Compute,
                 [&]( RDG::PassBuilder& pass )
                 {
                     using P = PathTracingDenoiseTemporalShaderParameters;
                     pass.Bindings( m_DenoiseTemporal.PassLayout.Get( m_DenoiseTemporal.PassShader ),
                                    Renderer::GetPipelineRouteFill( *m_DenoiseTemporal.PassPipeline ) )
                          .Uniform( P::CameraUB, camera )
                          .Storage( P::GuideAlbedoTexture, accumulation.GuideAlbedo, kR )
                          .Storage( P::GuideNormalTexture, accumulation.GuideNormal, kR )
                          .Storage( P::GuideSpecularTexture, accumulation.GuideSpecular, kR )
                          .Storage( P::GuideHitTexture, accumulation.GuideHit, kR )
                          .Storage( P::SnapshotDiffuseTexture, snapshot.Diffuse, kR )
                          .Storage( P::SnapshotSpecularTexture, snapshot.Specular, kR )
                          .Storage( P::SnapshotMomentsTexture, snapshot.Moments, kR )
                          .Storage( P::SnapshotGuideTexture, snapshot.Guide, kR )
                          .Storage( P::HistoryDiffuseTexture, history.Diffuse, kW )
                          .Storage( P::HistorySpecularTexture, history.Specular, kW )
                          .Storage( P::HistoryMomentsTexture, history.Moments, kW );
                 },
                 [this, width, height]( RDG::PassContext& context ) -> Common::BoolResultStr
                 {
                     DESERT_PROFILE_PASS( "PathTracing: Denoise Temporal" );
                     return DispatchDenoise( context, m_DenoiseTemporal, nullptr, width, height );
                 } );
        }

        // "PathTracing: Denoise Combine" + "PathTracing: Denoise AntiFirefly" (@p antiFirefly) + kDenoiseIterations
        // "PathTracing: Denoise Filter" + "PathTracing: Denoise Remodulate" (PathTracerDenoise.shader), every frame:
        // the denoised mean radiance (a = 1) the resolve reads in place of the accumulation.
        RDG::TextureRef AddDenoisePasses( RDG::Builder& graph, const AccumulationRefs& accumulation,
                                          const HistoryRefs& history, const bool antiFirefly, const uint32_t width,
                                          const uint32_t height )
        {
            constexpr RDG::Access  kR = RDG::Access::StorageRead;
            constexpr RDG::Access  kW = RDG::Access::StorageWrite;
            const RDG::TextureDesc desc{ .Size   = { .Width = width, .Height = height },
                                         .Format = ViewTargetFormats::kPathTracingDenoise };
            RDG::TextureRef diffuse  = graph.CreateTexture( desc, "PathTracing.DenoiseDiffuse" );
            RDG::TextureRef specular = graph.CreateTexture( desc, "PathTracing.DenoiseSpecular" );
            const RDG::TextureRef lengths = graph.CreateTexture( desc, "PathTracing.DenoiseHistoryLength" );
            const DenoisePush     combinePush{ glm::uvec4( m_HistoryValid ? 1u : 0u, 0u, 0u, 0u ) };
            graph.AddPass(
                 "PathTracing: Denoise Combine", RDG::PassFlags::Compute,
                 [&]( RDG::PassBuilder& pass )
                 {
                     using P = PathTracingDenoiseCombineShaderParameters;
                     pass.Bindings( m_DenoiseCombine.PassLayout.Get( m_DenoiseCombine.PassShader ),
                                    Renderer::GetPipelineRouteFill( *m_DenoiseCombine.PassPipeline ) )
                          .Storage( P::GuideAlbedoTexture, accumulation.GuideAlbedo, kR )
                          .Storage( P::GuideNormalTexture, accumulation.GuideNormal, kR )
                          .Storage( P::DemodulatedDiffuseTexture, accumulation.DemodulatedDiffuse, kR )
                          .Storage( P::DemodulatedSpecularTexture, accumulation.DemodulatedSpecular, kR )
                          .Storage( P::HistoryDiffuseTexture, history.Diffuse, kW )
                          .Storage( P::HistorySpecularTexture, history.Specular, kW )
                          .Storage( P::HistoryMomentsTexture, history.Moments, kW )
                          .Storage( P::IlluminationDiffuseTexture, diffuse, kW )
                          .Storage( P::IlluminationSpecularTexture, specular, kW )
                          .Storage( P::HistoryLengthTexture, lengths, kW )
                          .PushConstantBytes( static_cast<uint32_t>( sizeof( DenoisePush ) ) );
                 },
                 [this, combinePush, width, height]( RDG::PassContext& context ) -> Common::BoolResultStr
                 {
                     DESERT_PROFILE_PASS( "PathTracing: Denoise Combine" );
                     return DispatchDenoise( context, m_DenoiseCombine, &combinePush, width, height );
                 } );
            // An empty history is cleared by the combine: from here on it is a defined one (of no paths).
            m_HistoryValid = true;

            if ( antiFirefly )
            {
                const RDG::TextureRef diffuseTarget  = graph.CreateTexture( desc, "PathTracing.DenoiseDiffuse" );
                const RDG::TextureRef specularTarget = graph.CreateTexture( desc, "PathTracing.DenoiseSpecular" );
                graph.AddPass(
                     "PathTracing: Denoise AntiFirefly", RDG::PassFlags::Compute,
                     [&]( RDG::PassBuilder& pass )
                     {
                         using P = PathTracingDenoiseAntiFireflyShaderParameters;
                         pass.Bindings( m_DenoiseAntiFirefly.PassLayout.Get( m_DenoiseAntiFirefly.PassShader ),
                                        Renderer::GetPipelineRouteFill( *m_DenoiseAntiFirefly.PassPipeline ) )
                              .Storage( P::GuideAlbedoTexture, accumulation.GuideAlbedo, kR )
                              .Storage( P::GuideNormalTexture, accumulation.GuideNormal, kR )
                              .Storage( P::IlluminationDiffuseTexture, diffuse, kR )
                              .Storage( P::IlluminationSpecularTexture, specular, kR )
                              .Storage( P::FilteredDiffuseTexture, diffuseTarget, kW )
                              .Storage( P::FilteredSpecularTexture, specularTarget, kW );
                     },
                     [this, width, height]( RDG::PassContext& context ) -> Common::BoolResultStr
                     {
                         DESERT_PROFILE_PASS( "PathTracing: Denoise AntiFirefly" );
                         return DispatchDenoise( context, m_DenoiseAntiFirefly, nullptr, width, height );
                     } );
                diffuse  = diffuseTarget;
                specular = specularTarget;
            }

            for ( uint32_t iteration = 0; iteration < kDenoiseIterations; ++iteration )
            {
                const DenoisePush     push{ glm::uvec4( 1u << iteration, 0u, 0u, 0u ) };
                const RDG::TextureRef diffuseTarget  = graph.CreateTexture( desc, "PathTracing.DenoiseDiffuse" );
                const RDG::TextureRef specularTarget = graph.CreateTexture( desc, "PathTracing.DenoiseSpecular" );
                graph.AddPass(
                     "PathTracing: Denoise Filter", RDG::PassFlags::Compute,
                     [&]( RDG::PassBuilder& pass )
                     {
                         using P = PathTracingDenoiseFilterShaderParameters;
                         pass.Bindings( m_DenoiseFilter.PassLayout.Get( m_DenoiseFilter.PassShader ),
                                        Renderer::GetPipelineRouteFill( *m_DenoiseFilter.PassPipeline ) )
                              .Storage( P::GuideAlbedoTexture, accumulation.GuideAlbedo, kR )
                              .Storage( P::GuideNormalTexture, accumulation.GuideNormal, kR )
                              .Storage( P::GuideSpecularTexture, accumulation.GuideSpecular, kR )
                              .Storage( P::HistoryLengthTexture, lengths, kR )
                              .Storage( P::IlluminationDiffuseTexture, diffuse, kR )
                              .Storage( P::IlluminationSpecularTexture, specular, kR )
                              .Storage( P::FilteredDiffuseTexture, diffuseTarget, kW )
                              .Storage( P::FilteredSpecularTexture, specularTarget, kW )
                              .PushConstantBytes( static_cast<uint32_t>( sizeof( DenoisePush ) ) );
                     },
                     [this, push, width, height]( RDG::PassContext& context ) -> Common::BoolResultStr
                     {
                         DESERT_PROFILE_PASS( "PathTracing: Denoise Filter" );
                         return DispatchDenoise( context, m_DenoiseFilter, &push, width, height );
                     } );
                diffuse  = diffuseTarget;
                specular = specularTarget;
            }

            const RDG::TextureRef denoised = graph.CreateTexture( desc, "PathTracing.Denoised" );
            graph.AddPass(
                 "PathTracing: Denoise Remodulate", RDG::PassFlags::Compute,
                 [&]( RDG::PassBuilder& pass )
                 {
                     using P = PathTracingDenoiseRemodulateShaderParameters;
                     pass.Bindings( m_DenoiseRemodulate.PassLayout.Get( m_DenoiseRemodulate.PassShader ),
                                    Renderer::GetPipelineRouteFill( *m_DenoiseRemodulate.PassPipeline ) )
                          .Storage( P::AccumulationTexture, accumulation.Radiance, kR )
                          .Storage( P::GuideAlbedoTexture, accumulation.GuideAlbedo, kR )
                          .Storage( P::GuideSpecularTexture, accumulation.GuideSpecular, kR )
                          .Storage( P::DemodulatedDiffuseTexture, accumulation.DemodulatedDiffuse, kR )
                          .Storage( P::DemodulatedSpecularTexture, accumulation.DemodulatedSpecular, kR )
                          .Storage( P::FilteredDiffuseTexture, diffuse, kR )
                          .Storage( P::FilteredSpecularTexture, specular, kR )
                          .Storage( P::DenoisedTexture, denoised, kW );
                 },
                 [this, width, height]( RDG::PassContext& context ) -> Common::BoolResultStr
                 {
                     DESERT_PROFILE_PASS( "PathTracing: Denoise Remodulate" );
                     return DispatchDenoise( context, m_DenoiseRemodulate, nullptr, width, height );
                 } );
            return denoised;
        }

        // "PathTracing: Resolve": sum / count over the scene colour, every frame (also once converged), stored
        // pre-exposed like every other writer of the scene colour (ViewFrame::PreExposure; UE
        // PathTracingCompositingPixelShader.usf multiplies by View.PreExposure) - the tonemap divides it back out.
        // @p image: the accumulation (sum, count) or the denoiser's output (mean, 1).
        void AddResolvePass( RDG::Builder& graph, const RDG::TextureRef image, const RDG::TextureRef sceneColor,
                             const uint64_t frameIndex, const float preExposure )
        {
            const ResolvePush push { preExposure };
            graph.AddPass(
                 "PathTracing: Resolve", RDG::PassFlags::Raster,
                 [&]( RDG::PassBuilder& pass )
                 {
                     pass.Bindings( m_ResolveLayout.Get( m_ResolveShader ), RDG::OtherRouteFill{} )
                          .Texture( PathTracingResolveShaderParameters::AccumulationTexture, image,
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
        DenoiseProgram                      m_DenoiseSnapshot{ .Name = "Snapshot" };
        DenoiseProgram                      m_DenoiseTemporal{ .Name = "Temporal" };
        DenoiseProgram                      m_DenoiseCombine{ .Name = "Combine" };
        DenoiseProgram                      m_DenoiseAntiFirefly{ .Name = "AntiFirefly" };
        DenoiseProgram                      m_DenoiseFilter{ .Name = "Filter" };
        DenoiseProgram                      m_DenoiseRemodulate{ .Name = "Remodulate" };
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
        HistoryTarget      m_History;
        bool m_HistoryValid = false; // m_History holds THE DENOISER'S HISTORY of the current sum's pose
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
        bool m_RawToUpscaler = false; // the sum holds one frame's pixel-centre paths for the upscaler
        // THE NEURAL RADIANCE CACHE: the cache, the PATH_TRACING_NRC=1 trace and its hit groups' templates.
        bool                                                  m_Nrc       = false; // the sum's frame ran the cache
        bool m_NrcRequested = false; // the last frame's Settings::RadianceCache was Nrc
        // The cache's standing refusal (RefuseRadianceCache); none while the cache runs. It holds while Inputs are
        // this frame's and the cache stays requested (THE NEURAL RADIANCE CACHE above).
        struct CacheRefusal
        {
            std::string                                         Reason;
            std::optional<Engine::Neural::NeuralShaderRefusal> Shader; ///< the variant the shader service refused
            CacheInputs                                         Inputs; ///< what it was refused under
        };
        std::optional<CacheRefusal> m_CacheRefusal;
        // The refusal an input change dropped, until the retry it allowed ends: refused again for the same reason
        // it is not said again; the cache running clears it, so a later breakage is said.
        std::optional<CacheRefusal>                           m_RetriedCacheRefusal;
        std::optional<Engine::Neural::NeuralShaderRefusal> m_CacheShaderRefusal; // EnsureRadianceCache's, if any
        uint32_t                                              m_NrcFrames = 0;     // the training offset's seed
        std::unique_ptr<Engine::Neural::NeuralRadianceCache> m_RadianceCache;
        std::shared_ptr<Shader>                               m_TraceShaderNrc;
        std::shared_ptr<RayTracingPipeline>                   m_TracePipelineNrc;
        std::vector<std::string>                              m_PipelineTemplatesNrc;
        mutable ShaderBindingLayoutCache                      m_TraceLayoutNrc;
    };
} // namespace Desert::Graphic::System
