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
#include <Engine/Graphic/Denoise/OidnDenoiser.hpp>
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
#include <cstring>
#include <chrono>
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
    struct PathTracingLightGridShaderParameters
    {
        DESERT_SHADER_PARAMETER( StorageBuffer, PointLightsUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, SpotLightsUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, PathTracingLightGridUB );
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
        DESERT_SHADER_PARAMETER( StorageBuffer, PathTracingLightGridUB );
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
        DESERT_SHADER_PARAMETER( StorageBuffer, NrcTraceParamsUB );
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

    struct PathTracingOidnPackShaderParameters
    {
        DESERT_SHADER_PARAMETER( StorageTexture, AccumulationTexture );
        DESERT_SHADER_PARAMETER( StorageTexture, GuideAlbedoTexture );
        DESERT_SHADER_PARAMETER( StorageTexture, GuideNormalTexture );
        DESERT_SHADER_PARAMETER( StorageTexture, GuideSpecularTexture );
        DESERT_SHADER_PARAMETER( StorageBuffer, PathTracingOidnPackUB );
    };

    struct PathTracingOidnUnpackShaderParameters
    {
        DESERT_SHADER_PARAMETER( StorageBuffer, PathTracingOidnOutputUB );
        DESERT_SHADER_PARAMETER( StorageTexture, OidnResultTexture );
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
    // a skipped frame), the camera moved (ViewProjection != PrevViewProjection), MaxBounces or LightSampling
    // changed, geometry was
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
    // THE LIGHT CHOICE (PathTracing.LightSampling, Settings::LightSampling): each vertex samples ONE of the sun,
    // the sky, the moon and the local lights (UE: one light chosen by its estimate). AllLights evaluates every
    // light's estimate at the vertex (UE EstimateLight), O(lights) per vertex; Grid (NVIDIA RTXDI ReGIR) builds a
    // world grid of light reservoirs around the camera each traced frame ("PathTracing: Light Grid",
    // PathTracerLightGrid.shader: kLightGridSlots per cell, each the weighted-reservoir pick of
    // kLightGridCandidates uniformly drawn local lights by their luminance at the cell), and a vertex resamples a
    // few slots of its cell by the light's unshadowed f * cos (PathTracer.shader PathTracingChooseLights). Both
    // are unbiased and converge to the same image; a vertex outside the grid takes AllLights. A change of the
    // method restarts the sum (each sum is one estimator).
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
    // THE DENOISER (PathTracing.Denoiser = Off | ReLAX | RR | Auto | OIDN, PathTracingDenoiserMethod; UE
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
    //  - OIDN (UE's OpenImageDenoise plugin): OIDN denoises the frame's estimate once it is FINAL - the sum holds
    //    PathTracing.SamplesPerPixel paths, or under Nrc every frame (the sum restarts each frame, THE NEURAL
    //    RADIANCE CACHE below; UE's real-time path tracer denoises every frame). Until then the view is ReLAX's,
    //    as above. A final estimate not denoised yet is denoised IN ITS OWN FRAME'S GRAPH (AddOidnPasses):
    //    "PathTracing: OIDN Pack" writes the mean radiance and the paths' albedo (both lobes' reflectance, 1 for a
    //    path that met no surface) and normal (PathTracerDenoise.shader "OidnPack"; means over the sum's paths, so
    //    valid from one path per pixel) into a buffer shared with OIDN (Renderer::CreateSharedBuffer: exported
    //    Vulkan memory, imported by OIDN's CUDA / HIP / SYCL device); "PathTracing: OIDN", an ExternalQueue pass,
    //    splits the frame's submission there on a shared timeline semaphore, and Graphic::Denoise::OidnDenoiser
    //    filters it (High, hdr, cleanAux guides) on the GPU whose UUID is the Vulkan device's, enqueued and not
    //    waited; "PathTracing: OIDN Unpack" writes "PathTracing.OidnResult", which the resolve of the SAME frame
    //    reads, the ReLAX passes not run. A converged still sum is denoised once and its image kept; under Nrc
    //    each frame's estimate is, with no latency. An estimate that is no longer final, or a sum not kept (a cut,
    //    a resize, the scene - anything but the camera's motion and Nrc's own restart), drops the image. A refusal
    //    (CapabilityCatalog::Sharing None, the library missing, no OIDN GPU device for this GPU, an export, import
    //    or enqueue error) is logged with its reason, kept in OidnDenoiser::Status (the control channel's device
    //    state) and the view shows the raw accumulation - never a silent ReLAX in its place, and never a host
    //    copy. UE's OpenImageDenoise plugin reads the image back and blocks; this path neither copies nor stalls.
    //    While OIDN's image is shown, THE DENOISER'S HISTORY is dropped (the ReLAX passes
    //    do not run), so the first frame after it that ReLAX shows starts ReLAX's history afresh, as after a cut.
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
    // away from Nrc and back), MachineSettings::ForcedNeuralKernelPath, or a recompile of a program the cache is
    // built from (ShaderService::ProgramGeneration of NeuralRadianceCache::kPrograms, the radiance-cache trace's
    // program and its hit groups' surface templates; the rule of
    // SceneRenderer::RetryRefusedProgramsAfterRecompile: the same bytes fail the same way, and another shader's
    // bytes do not change them) - so a refused cache costs one build attempt, not one per frame, and a
    // hot-reloaded fix or a changed path takes it back on the next frame. A retry refused again for the same
    // reason is not said again. The frame's data-shape refusals (AddTraceTargets, AddCachePasses) are the same
    // held refusal: AddTraceTargets is asked before the sum's restart decision, so a refusal there leaves this
    // frame's paths without the cache; AddCachePasses cannot refuse a batch (Initialize validated every one), and
    // a refusal it still gives leaves the passes it added - the sum it composited into is this frame's only (it
    // restarts every Nrc frame), and the next frame, without the cache, restarts it and empties the history (a
    // method change). A RUNNING cache follows the inputs that made its weights: it is built under
    // MachineSettings::ForcedNeuralKernelPath and the ProgramGeneration of its own programs (NeuralRadianceCache::
    // kPrograms: its passes and its MLP's kernels - CacheInputs::SameCache), and the first Nrc frame either
    // differs at rebuilds it once
    // - the old cache retired (FrameRetireQueue: frames in flight still read its buffers and pipelines), a new one
    // made with the new path (fresh weights and hash table: the old ones were trained by other kernels), the sum
    // restarted and the history emptied (it holds the old cache's light) - so switching the path while the cache
    // runs takes effect on the next frame; GetRadianceCacheStatus counts the rebuilds and the cache's stats name
    // the path it runs.
    //
    // Not here (each is UE's and a task of its own): rough glass (the
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

        // PathTracing.LightSampling (THE LIGHT CHOICE above).
        enum class LightSamplingMethod : uint32_t
        {
            AllLights = 0,
            Grid      = 1,
        };

        // THE LIGHT CHOICE's grid: kLightGridCellsXZ x kLightGridCellsY x kLightGridCellsXZ cells of
        // kLightGridCellSide around the camera (snapped to the cell side, so a still camera keeps its cells),
        // kLightGridSlots reservoirs per cell, each picked from kLightGridCandidates lights.
        static constexpr float    kLightGridCellSide   = 400.0f; // cm
        static constexpr uint32_t kLightGridCellsXZ    = 32u;    // cells along x and z
        static constexpr uint32_t kLightGridCellsY     = 16u;    // cells along y (up)
        static constexpr uint32_t kLightGridSlots      = 32u;
        static constexpr uint32_t kLightGridCandidates = 32u;

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
            // PathTracing.LightSampling (THE LIGHT CHOICE above).
            LightSamplingMethod LightSampling = LightSamplingMethod::AllLights;
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

            m_LightGridShader = shaders->GetByName( "PathTracerLightGrid/Build" );
            if ( !m_LightGridShader )
                return Common::MakeError( "PathTracer: the light grid program 'PathTracerLightGrid/Build' is not "
                                          "registered" );
            const auto lightGrid =
                 ComputePipeline::Create( { .Shader = m_LightGridShader, .DebugName = "PathTracerLightGrid" } );
            if ( !lightGrid )
                return Common::MakeError( lightGrid.GetError() );
            m_LightGridPipeline = lightGrid.GetValue();

            for ( DenoiseProgram* program :
                  { &m_DenoiseSnapshot, &m_DenoiseTemporal, &m_DenoiseCombine, &m_DenoiseAntiFirefly,
                    &m_DenoiseFilter, &m_DenoiseRemodulate, &m_DenoiseOidnPack, &m_DenoiseOidnUnpack } )
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

        // The radiance-cache trace's program (built with PATH_TRACING_NRC=1 by EnsureRadianceCache).
        static constexpr const char* kTraceProgram = "PathTracer/PathTracing";

        // A registered program and its ShaderService::ProgramGeneration.
        struct ProgramStamp
        {
            std::string Program;
            uint64_t    Generation = 0;

            bool operator==( const ProgramStamp& ) const = default;
        };
        // "<program> <generation>, ..." (the log and state nrc).
        [[nodiscard]] static std::string FormatStamps( const std::vector<ProgramStamp>& stamps )
        {
            std::string text;
            for ( const ProgramStamp& stamp : stamps )
                text += std::format( "{}{} {}", text.empty() ? "" : ", ", stamp.Program, stamp.Generation );
            return text;
        }

        // The inputs a cache is built (and a cache refusal held) under besides PathTracing.RadianceCache itself
        // (THE NEURAL RADIANCE CACHE above).
        struct CacheInputs
        {
            std::optional<Common::Settings::NeuralKernelPath>
                 ForcedPath;                  ///< MachineSettings::ForcedNeuralKernelPath
            std::vector<ProgramStamp> Cache; ///< NeuralRadianceCache::kPrograms: the code that trains the weights
            std::vector<ProgramStamp> Trace; ///< kTraceProgram and the hit groups' surface templates

            // Everything: a held refusal may be any of these programs' (EnsureRadianceCache builds them all).
            bool operator==( const CacheInputs& ) const = default;
            // What the running cache's weights depend on: the kernel path and the cache's own programs. The trace
            // feeding it recompiling does not make the learned light wrong (its pipeline follows its shader on the
            // hot-reload poll, Graphic::AnyPipelineBehindItsShader).
            [[nodiscard]] bool SameCache( const CacheInputs& other ) const
            {
                return ForcedPath == other.ForcedPath && Cache == other.Cache;
            }
        };

        // CacheInputs as they are this frame (after EnsurePipeline: the hit groups are m_PipelineTemplates').
        [[nodiscard]] CacheInputs CurrentCacheInputs() const
        {
            const Runtime::ShaderService* shaders = Runtime::ResourceRegistry::GetShaderService();
            CacheInputs inputs{ .ForcedPath = Common::Settings::MachineSettings::Get().ForcedNeuralKernelPath };
            for ( const char* program : Engine::Neural::NeuralRadianceCache::kPrograms )
                inputs.Cache.push_back( { program, shaders->ProgramGeneration( program ) } );
            inputs.Trace.push_back( { kTraceProgram, shaders->ProgramGeneration( kTraceProgram ) } );
            for ( const std::string& surfaceTemplate : m_PipelineTemplates )
                inputs.Trace.push_back( { surfaceTemplate, shaders->ProgramGeneration( surfaceTemplate ) } );
            return inputs;
        }

        // THE NEURAL RADIANCE CACHE'S PIECES, made on the first Nrc frame: the cache (its programs, the NN layer's
        // kernel path) and the trace built with PATH_TRACING_NRC=1 over the same hit groups as EnsurePipeline's.
        // @p inputs: what this frame builds it under (m_RadianceCacheInputs, THE NEURAL RADIANCE CACHE above).
        [[nodiscard]] Common::BoolResultStr EnsureRadianceCache( const CacheInputs& inputs )
        {
            const auto& frames = ::Desert::Engine::FrameManager::GetInstance();
            m_RetiringCaches.Collect( frames.GetAbsoluteFrameCount(), frames.GetMaxFramesInFlight() );
            if ( !m_RadianceCache )
            {
                auto cache = std::make_unique<Engine::Neural::NeuralRadianceCache>(
                     ::Desert::EngineContext::GetInstance().GetCapabilities().Neural );
                if ( const auto made = cache->Initialize(); !made )
                {
                    m_CacheShaderRefusal = cache->GetShaderRefusal();
                    return made;
                }
                m_RadianceCache       = std::move( cache );
                m_RadianceCacheInputs = inputs;
            }
            if ( m_TracePipelineNrc && m_PipelineTemplatesNrc == m_PipelineTemplates )
                return BOOLSUCCESS;
            auto*                 shaders = Runtime::ResourceRegistry::GetShaderService();
            Core::ShaderVariant   variant;
            variant.Defines = { "PATH_TRACING_NRC=1" };
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

        // The running cache was built under other inputs than this frame's @p inputs: it is retired (kept alive
        // until no frame in flight reads it) and EnsureRadianceCache makes the new one this frame; the sum restarts
        // and the history empties, both made with the old cache's light.
        void RetireRadianceCache( const CacheInputs& inputs )
        {
            const auto&       frames = ::Desert::Engine::FrameManager::GetInstance();
            const std::string from   = m_RadianceCache->GetStats().KernelPath;
            const auto forcedName = []( const std::optional<Common::Settings::NeuralKernelPath>& forced )
            { return forced ? Engine::NeuralKernelPathName( *forced ) : "none"; };
            LOG_INFO( "[PathTracer] radiance cache rebuilt (ForcedNeuralKernelPath {} -> {}, program generations "
                      "{} -> {}); it ran the {} path",
                      forcedName( m_RadianceCacheInputs.ForcedPath ), forcedName( inputs.ForcedPath ),
                      FormatStamps( m_RadianceCacheInputs.Cache ), FormatStamps( inputs.Cache ), from );
            m_RetiringCaches.Park( std::move( m_RadianceCache ), frames.GetAbsoluteFrameCount() );
            ++m_RadianceCacheRebuilds;
            Restart();
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
            if ( nrc && m_RadianceCache && !m_RadianceCacheInputs.SameCache( cacheInputs ) )
                RetireRadianceCache( cacheInputs );
            if ( nrc )
                if ( const auto ready = EnsureRadianceCache( cacheInputs ); !ready )
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
                                 settings.LightSampling == m_LightSampling && inputs.SceneHash == m_SceneHash &&
                                 !inputs.DynamicGeometry && settings.MaxPathIntensity == m_MaxPathIntensity &&
                                 !exposureLeft && !inputs.RawToUpscaler && !m_RawToUpscaler && nrc == m_Nrc;
            const bool cameraMoved = frame.ViewProjection != frame.PrevViewProjection;
            // Under Nrc the sum restarts every frame (THE NEURAL RADIANCE CACHE above) and keeps the history.
            const bool restart     = !sumKept || cameraMoved || nrc;
            const bool hadSum      = m_Summed > 0u; // the previous frame's, before a restart empties it
            // Why the sum is not kept, stated against the previous frame's inputs before they are replaced below.
            const bool bouncesLeft    = settings.MaxBounces != m_MaxBounces;
            const bool samplingLeft   = settings.LightSampling != m_LightSampling;
            const bool sceneLeft      = inputs.SceneHash != m_SceneHash;
            const bool clampLeft      = settings.MaxPathIntensity != m_MaxPathIntensity;
            const bool rawLeft        = inputs.RawToUpscaler || m_RawToUpscaler;
            const bool nrcLeft        = nrc != m_Nrc;
            const bool stampUnreadable = !m_Stamp.ReadableIn( frame );
            if ( restart )
            {
                m_Summed = 0;
                ++m_Restarts;
                m_ClampPreExposure = frame.PreExposure;
            }
            m_MaxBounces       = settings.MaxBounces;
            m_LightSampling    = settings.LightSampling;
            m_MaxPathIntensity = settings.MaxPathIntensity;
            m_SceneHash        = inputs.SceneHash;
            m_RawToUpscaler    = inputs.RawToUpscaler;
            m_Nrc              = nrc;
            const uint32_t owed   = settings.SamplesPerPixel > m_Summed ? settings.SamplesPerPixel - m_Summed : 0u;
            const uint32_t traced = std::min( settings.PathsPerFrame, owed );

            // The frame's estimate is final when no later frame adds to it: the sum reached SamplesPerPixel, or
            // under Nrc every frame that traces (the sum restarts each frame, THE NEURAL RADIANCE CACHE above).
            const uint32_t pathsAfter = m_Summed + traced;
            const bool     estimateFinal =
                 nrc ? traced > 0u : pathsAfter > 0u && pathsAfter >= settings.SamplesPerPixel;
            // PathTracing.Denoiser OIDN (THE DENOISER above): what this frame shows - ReLAX's image while the sum
            // grows or OIDN's is not back yet, OIDN's once it is, the raw sum after a refusal.
            const OidnView oidnView =
                 UpdateOidn( settings, inputs.RawToUpscaler, sumKept, estimateFinal, pathsAfter, width, height );
            const bool denoise = settings.Denoiser != Common::Scalability::PathTracingDenoiserMethod::Off &&
                                 !inputs.RawToUpscaler && oidnView == OidnView::Denoiser;
            // The previous frame's sum is folded into the history and reprojected (THE DENOISER'S HISTORY).
            // A history made in this graph (the denoiser just turned on) is written by the combine before any read.
            const bool reproject = denoise && sumKept && ( cameraMoved || nrc ) && hadSum && m_History.Built &&
                                   m_History.Width == width && m_History.Height == height;
            if ( !denoise || !sumKept )
            {
                if ( m_HistoryValid && denoise )
                    LOG_INFO( "[PathTracer] denoiser history reset (resized {}, stamp {}, bounces {}, light sampling "
                              "{}, scene {}, dynamic geometry {}, clamp {}, exposure {}, raw to upscaler {}, radiance "
                              "cache {})",
                              resized, stampUnreadable, bouncesLeft, samplingLeft, sceneLeft, inputs.DynamicGeometry,
                              clampLeft, exposureLeft, rawLeft, nrcLeft );
                m_HistoryValid = false;
            }
            const AccumulationRefs accumulation = AcquireAccumulation( graph, width, height );
            const HistoryRefs      history = denoise ? AcquireHistory( graph, width, height ) : HistoryRefs{};
            const SnapshotRefs     snapshot =
                 reproject ? AddDenoiseSnapshotPass( graph, accumulation, history, width, height ) : SnapshotRefs{};

            if ( traced > 0u )
            {
                const SkyTargets       sky  = AddSkyPasses( graph, inputs, envDesc.GetValue().Size.Width );
                const LightGridTargets grid = AddLightGridPass( graph, inputs, frame, settings.LightSampling );
                // Under Nrc the sum restarted, so traced is the nrcPaths the targets were made for.
                if ( nrcTargets )
                {
                    AddTracePass( graph, accumulation, sky, grid, inputs, width, height, traced, &*nrcTargets );
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
                    AddTracePass( graph, accumulation, sky, grid, inputs, width, height, traced, nullptr );
                m_Summed += traced;
            }
            if ( reproject )
            {
                AddDenoiseTemporalPass( graph, inputs.Camera, accumulation, snapshot, history, width, height );
                m_HistoryValid = true;
            }
            RDG::TextureRef image = accumulation.Radiance;
            if ( oidnView == OidnView::Result )
                image = AddOidnPasses( graph, accumulation, width, height );
            else if ( denoise )
                image = AddDenoisePasses( graph, accumulation, history, settings.AntiFirefly, width, height );
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
            uint32_t    Rebuilds = 0; ///< running caches rebuilt because their inputs changed (RetireRadianceCache)
            /// The ProgramGeneration of NeuralRadianceCache::kPrograms the running cache was built under (a held
            /// refusal: refused under; neither: empty) - Rebuilds moves exactly when one of these does (or the
            /// kernel path).
            std::vector<ProgramStamp> ProgramGenerations;
        };
        [[nodiscard]] RadianceCacheStatus GetRadianceCacheStatus() const
        {
            using Method = Common::Scalability::PathTracingRadianceCacheMethod;
            RadianceCacheStatus status{ .Requested = m_NrcRequested ? Method::Nrc : Method::Off,
                                        .Effective = m_Nrc ? Method::Nrc : Method::Off,
                                        .Rebuilds  = m_RadianceCacheRebuilds };
            if ( m_RadianceCache )
                status.ProgramGenerations = m_RadianceCacheInputs.Cache;
            if ( m_NrcRequested && m_CacheRefusal )
            {
                const CacheInputs& held   = m_CacheRefusal->Inputs;
                const char*        forced = "none";
                if ( held.ForcedPath )
                    forced = Engine::NeuralKernelPathName( *held.ForcedPath );
                status.Reason             = m_CacheRefusal->Reason;
                status.ProgramGenerations = held.Cache;
                status.RetryOn            = std::format(
                     "PathTracing.RadianceCache set away from Nrc and back; MachineSettings ForcedNeuralKernelPath "
                     "changed (held at {}); a recompile of one of its programs (ShaderService::ProgramGeneration, "
                     "held at {}, {})",
                     forced, FormatStamps( held.Cache ), FormatStamps( held.Trace ) );
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
            glm::vec4  LightGrid;     // LightGridTargets::Grid
            glm::uvec4 LightGridSize; // LightGridTargets::Size
        };
        static_assert( sizeof( TracePush ) <= 128u,
                       "the trace's push constants exceed Vulkan's guaranteed 128 bytes" );
        // PathTracerLightGrid.shader "PushConstants".
        struct LightGridPush
        {
            glm::vec4  Grid;   // the min corner (cm), the cell side (cm)
            glm::uvec4 Size;   // cells along x, y, z, slots per cell
            glm::uvec4 Counts; // point lights, spot lights, candidates per slot, the frame's seed
        };
        // This frame's light grid as the trace reads it: Size.x = 0 is no grid (Buffer then one empty slot).
        struct LightGridTargets
        {
            RDG::BufferRef Buffer;
            glm::vec4      Grid{ 0.0f };
            glm::uvec4     Size{ 0u };
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
        static constexpr uint32_t kLightGridGroupSize = 64u; // LocalSize(64, 1, 1) in PathTracerLightGrid.shader

        // SVGF's a-trous iterations (Schied et al. 2017, 4.3): taps 1, 2, 4, 8, 16 pixels apart, a 5 x 5 kernel
        // each, so the last one reaches 32 pixels - the footprint the paper filters one path per pixel with.
        static constexpr uint32_t kDenoiseIterations = 5;

        void Restart()
        {
            m_Summed = 0;
            m_Stamp.Invalidate();
        }

        // PathTracing.Denoiser OIDN (THE DENOISER above): what the frame resolves.
        enum class OidnView : uint8_t
        {
            Denoiser, // ReLAX's image (the sum is still growing)
            Result,   // "PathTracing.OidnResult", OIDN's image of the final estimate, written this frame or kept
            Raw,      // the accumulation: OIDN refused (logged, OidnDenoiser::Status().Error)
        };
        enum class OidnPhase : uint8_t
        {
            Idle,    // no OIDN image of the current estimates
            Shown,   // m_Oidn.Result holds OIDN's image of m_Oidn.ShownKey's estimate (or this graph writes it)
            Refused, // OIDN refused: the raw sum is shown until the estimates are dropped
        };
        // One final estimate: the restart that began it, the paths summed in it, its size.
        struct OidnKey
        {
            uint32_t Restart = 0;
            uint32_t Paths   = 0;
            uint32_t Width   = 0;
            uint32_t Height  = 0;
            bool     operator==( const OidnKey& ) const = default;
        };
        // The memory OIDN shares with the graph (Renderer::CreateSharedBuffer, SharedGpuMemory.hpp) and the
        // timeline that orders the two queues, bound to the denoiser for one size.
        struct OidnState
        {
            OidnPhase                          Phase = OidnPhase::Idle;
            OidnKey                            ShownKey;             // the estimate whose image Phase Shown shows
            bool                               RunThisFrame = false; // UpdateOidn asked this graph for the run
            Denoise::OidnDenoiser*             Denoiser     = nullptr;
            std::shared_ptr<SharedGpuBuffer>   Input;  // OidnPack's three float3 planes: colour, albedo, normal
            std::shared_ptr<SharedGpuBuffer>   Output; // OIDN's float3 image
            std::shared_ptr<SharedGpuTimeline> Timeline;
            uint32_t                           BoundWidth  = 0; // the size Input / Output are bound for (0: none)
            uint32_t                           BoundHeight = 0;
            RDG::ExternalBuffer                InputImport; // this graph's imports of Input / Output
            RDG::ExternalBuffer                OutputImport;
            RDG::ExternalTexture               Result; // "PathTracing.OidnResult"
            uint32_t                           ResultWidth  = 0;
            uint32_t                           ResultHeight = 0;
        };

        // Forgets the image of the estimates; the shared memory and the result texture are kept for reuse.
        void DropOidnEstimates()
        {
            m_Oidn.Phase    = OidnPhase::Idle;
            m_Oidn.ShownKey = {};
        }

        // Unbinds the denoiser and drops the shared memory (its Vulkan objects go through the frames-in-flight
        // queue, OIDN's imports are released after OIDN's stream finished: OidnDenoiser::ReleaseShared).
        void ReleaseOidnShared()
        {
            if ( m_Oidn.Denoiser )
                m_Oidn.Denoiser->ReleaseShared();
            m_Oidn.Input.reset();
            m_Oidn.Output.reset();
            m_Oidn.BoundWidth  = 0;
            m_Oidn.BoundHeight = 0;
        }

        // Drops every OIDN resource: the denoiser is not OIDN, or the paths go to the upscaler raw.
        void ReleaseOidn()
        {
            DropOidnEstimates();
            m_Oidn.RunThisFrame = false;
            ReleaseOidnShared();
            m_Oidn.Timeline.reset();
            m_Oidn.Denoiser = nullptr;
            if ( m_Oidn.Result.Physical )
                m_RetiringAccumulation.Park( std::move( m_Oidn.Result ),
                                             ::Desert::Engine::FrameManager::GetInstance().GetAbsoluteFrameCount() );
            m_Oidn.Result       = {};
            m_Oidn.ResultWidth  = 0;
            m_Oidn.ResultHeight = 0;
        }

        // The denoiser, the shared buffers of this size and the timeline, bound once (again after a resize). An
        // error names what refused: GpuSharing None, the denoiser (no OIDN GPU device for this GPU), an export or
        // an import.
        Common::BoolResultStr EnsureOidnShared( const uint32_t width, const uint32_t height )
        {
            if ( m_Oidn.Denoiser && m_Oidn.Input && m_Oidn.Output && m_Oidn.Timeline &&
                 m_Oidn.BoundWidth == width && m_Oidn.BoundHeight == height )
                return Common::MakeSuccess( true );
            const auto& capabilities = EngineContext::GetInstance().GetCapabilities();
            const auto  denoiser =
                 Denoise::OidnDenoiser::Acquire( capabilities.DeviceUuid, capabilities.Catalog.Sharing );
            if ( !denoiser )
                return Common::MakeError( denoiser.GetError() );
            ReleaseOidnShared();
            m_Oidn.Denoiser = denoiser.GetValue();

            const uint64_t plane = static_cast<uint64_t>( width ) * height * 3u * sizeof( float );
            auto           input = Renderer::CreateSharedBuffer( "PathTracing.OidnInput", 3u * plane );
            if ( !input )
                return Common::MakeError( input.GetError() );
            auto output = Renderer::CreateSharedBuffer( "PathTracing.OidnOutput", plane );
            if ( !output )
                return Common::MakeError( output.GetError() );
            if ( !m_Oidn.Timeline )
            {
                auto timeline = Renderer::CreateSharedTimeline( "PathTracing.OidnTimeline" );
                if ( !timeline )
                    return Common::MakeError( timeline.GetError() );
                m_Oidn.Timeline = timeline.GetValue();
            }
            const auto inputHandle    = input.GetValue()->ExportHandle();
            const auto outputHandle   = output.GetValue()->ExportHandle();
            const auto timelineHandle = m_Oidn.Timeline->ExportHandle();
            for ( const auto* handle : { &inputHandle, &outputHandle, &timelineHandle } )
                if ( !*handle )
                    return Common::MakeError( handle->GetError() );

            Denoise::SharedDenoiseImages images{
                 .Width    = width,
                 .Height   = height,
                 .Buffers  = { { .Handle = inputHandle.GetValue(), .Bytes = 3u * plane },
                               { .Handle = outputHandle.GetValue(), .Bytes = plane } },
                 .Color    = { .Buffer = 0, .ByteOffset = 0 },
                 .Albedo   = { .Buffer = 0, .ByteOffset = plane },
                 .Normal   = { .Buffer = 0, .ByteOffset = 2u * plane },
                 .Output   = { .Buffer = 1, .ByteOffset = 0 },
                 .Timeline = timelineHandle.GetValue() };
            if ( const Common::BoolResultStr bound =
                      m_Oidn.Denoiser->BindShared( images, Denoise::ImageDenoiseQuality::High );
                 !bound )
                return bound;
            m_Oidn.Input       = input.GetValue();
            m_Oidn.Output      = output.GetValue();
            m_Oidn.BoundWidth  = width;
            m_Oidn.BoundHeight = height;
            return Common::MakeSuccess( true );
        }

        // A refusal: logged with its reason, kept in OidnDenoiser::Status, and the raw sum is shown until the
        // estimates are dropped - never a silent ReLAX in its place.
        OidnView RefuseOidn( const OidnKey& key, const std::string& reason )
        {
            LOG_ERROR(
                 "[PathTracing] OIDN did not denoise the {}x{} image of {} paths per pixel: {}; the view shows "
                 "the raw accumulation",
                 key.Width, key.Height, key.Paths, reason );
            Denoise::OidnDenoiser::NoteError( reason );
            m_Oidn.Phase        = OidnPhase::Refused;
            m_Oidn.RunThisFrame = false;
            return OidnView::Raw;
        }

        // GRAPH BUILD, after the restart decision: the OIDN step of the frame. The rule is one for both modes -
        // OIDN denoises the frame's estimate once it is final (@p estimateFinal: SamplesPerPixel paths summed, or
        // under Nrc every frame that traces), IN THIS FRAME'S GRAPH (AddOidnPasses: pack, OIDN between two
        // submissions on the shared timeline, unpack), and the frame resolves that image. A converged still sum is
        // denoised once and its image kept; under Nrc every frame is, with no latency. An estimate that is not
        // final, or a sum that was not kept (@p sumKept false: a cut, a resize, the scene - anything but the
        // camera's motion and Nrc's own restart), drops the image: it is of another scene.
        OidnView UpdateOidn( const Settings& settings, const bool rawToUpscaler, const bool sumKept,
                             const bool estimateFinal, const uint32_t pathsAfter, const uint32_t width,
                             const uint32_t height )
        {
            m_Oidn.RunThisFrame = false;
            if ( settings.Denoiser != Common::Scalability::PathTracingDenoiserMethod::OIDN || rawToUpscaler )
            {
                if ( m_Oidn.Phase != OidnPhase::Idle || m_Oidn.Denoiser || m_Oidn.Timeline ||
                     m_Oidn.Result.Physical )
                    ReleaseOidn();
                return OidnView::Denoiser;
            }
            if ( !sumKept || !estimateFinal )
                DropOidnEstimates();
            if ( !estimateFinal )
                return OidnView::Denoiser; // converging: ReLAX's image
            if ( m_Oidn.Phase == OidnPhase::Refused )
                return OidnView::Raw;

            const OidnKey key{ .Restart = m_Restarts, .Paths = pathsAfter, .Width = width, .Height = height };
            if ( m_Oidn.Phase == OidnPhase::Shown && m_Oidn.ShownKey == key )
            {
                if ( m_Oidn.Result.Physical && m_Oidn.ResultWidth == width && m_Oidn.ResultHeight == height )
                    return OidnView::Result; // the image of this estimate, kept
                return RefuseOidn( key, "the denoised image of this estimate was lost before it was shown again" );
            }
            if ( const Common::BoolResultStr ready = EnsureOidnShared( width, height ); !ready )
                return RefuseOidn( key, ready.GetError() );
            m_Oidn.Phase        = OidnPhase::Shown;
            m_Oidn.ShownKey     = key;
            m_Oidn.RunThisFrame = true;
            return OidnView::Result;
        }

        // "PathTracing.OidnResult" in this graph: registered when it exists at this size, else created and
        // extracted (kept for the frames that show the same estimate).
        RDG::TextureRef AcquireOidnResult( RDG::Builder& graph, const uint32_t width, const uint32_t height )
        {
            if ( m_Oidn.Result.Physical && m_Oidn.ResultWidth == width && m_Oidn.ResultHeight == height )
                return graph.RegisterExternal( m_Oidn.Result, "PathTracing.OidnResult" );
            if ( m_Oidn.Result.Physical )
                m_RetiringAccumulation.Park(
                     std::move( m_Oidn.Result ),
                     ::Desert::Engine::FrameManager::GetInstance().GetAbsoluteFrameCount() );
            m_Oidn.Result = {};
            const RDG::TextureDesc desc{ .Size   = { .Width = width, .Height = height },
                                         .Format = ViewTargetFormats::kPathTracingDenoise };
            const RDG::TextureRef  result = graph.CreateTexture( desc, "PathTracing.OidnResult" );
            graph.Extract( result, m_Oidn.Result, RDG::Access::SampledGraphics,
                           { RDG::Access::StorageWrite, RDG::Access::SampledGraphics } );
            m_Oidn.ResultWidth  = width;
            m_Oidn.ResultHeight = height;
            return result;
        }

        // OIDN's image of the frame's final estimate, IN THIS GRAPH (THE DENOISER above). On a frame that runs it
        // (m_Oidn.RunThisFrame), after this frame's trace:
        //  1. "PathTracing: OIDN Pack" writes the estimate as OIDN's three float3 planes (PathTracerDenoise.shader
        //     "OidnPack") into the shared input buffer;
        //  2. "PathTracing: OIDN" is an ExternalQueue pass (RDG-EXTERNAL): the submission so far signals the
        //     timeline's odd value, OIDN (CUDA / HIP / SYCL, enqueued by the exec, not waited) waits it, filters
        //     the shared input into the shared output and signals the even value, and the rest of the frame waits
        //     that on the GPU - no host stall, no copy;
        //  3. "PathTracing: OIDN Unpack" turns the float3 output into "PathTracing.OidnResult" (RGBA32F, a = 1,
        //  the
        //     resolve's (mean, 1) form).
        // A frame that shows a kept image only registers the result. @p raw is never shown here: a refusal turned
        // the view to Raw before this graph was built.
        RDG::TextureRef AddOidnPasses( RDG::Builder& graph, const AccumulationRefs& accumulation,
                                       const uint32_t width, const uint32_t height )
        {
            const RDG::TextureRef result = AcquireOidnResult( graph, width, height );
            if ( !m_Oidn.RunThisFrame )
                return result;

            constexpr RDG::Access kR = RDG::Access::StorageRead;
            constexpr RDG::Access kW = RDG::Access::StorageWrite;
            (void)m_Oidn.Input->Import( m_Oidn.InputImport );   // never fails: the buffers are bound
            (void)m_Oidn.Output->Import( m_Oidn.OutputImport ); // (EnsureOidnShared made them this size)
            const RDG::BufferRef input  = graph.RegisterExternal( m_Oidn.InputImport, "PathTracing.OidnInput" );
            const RDG::BufferRef output = graph.RegisterExternal( m_Oidn.OutputImport, "PathTracing.OidnOutput" );

            graph.AddPass(
                 "PathTracing: OIDN Pack", RDG::PassFlags::Compute,
                 [&]( RDG::PassBuilder& pass )
                 {
                     using P = PathTracingOidnPackShaderParameters;
                     pass.Bindings( m_DenoiseOidnPack.PassLayout.Get( m_DenoiseOidnPack.PassShader ),
                                    Renderer::GetPipelineRouteFill( *m_DenoiseOidnPack.PassPipeline ) )
                          .Storage( P::AccumulationTexture, accumulation.Radiance, kR )
                          .Storage( P::GuideAlbedoTexture, accumulation.GuideAlbedo, kR )
                          .Storage( P::GuideNormalTexture, accumulation.GuideNormal, kR )
                          .Storage( P::GuideSpecularTexture, accumulation.GuideSpecular, kR )
                          .Storage( P::PathTracingOidnPackUB, input, kW );
                 },
                 [this, width, height]( RDG::PassContext& context ) -> Common::BoolResultStr
                 {
                     DESERT_PROFILE_PASS( "PathTracing: OIDN Pack" );
                     return DispatchDenoise( context, m_DenoiseOidnPack, nullptr, width, height );
                 } );

            const RDG::ExternalQueueSync sync = m_Oidn.Timeline->NextPair();
            const OidnKey                key  = m_Oidn.ShownKey;
            const uint64_t frame = ::Desert::Engine::FrameManager::GetInstance().GetAbsoluteFrameCount();
            graph.AddPass(
                 "PathTracing: OIDN", RDG::PassFlags::Copy,
                 [input, output, sync]( RDG::PassBuilder& pass )
                 {
                     // OIDN reads and writes the shared memory as raw bytes: Copy accesses.
                     pass.Read( input, RDG::Access::CopySrc );
                     pass.Write( output, RDG::Access::CopyDst );
                     pass.ExternalQueue( sync );
                 },
                 [this, sync, key, frame]( RDG::PassContext& ) -> Common::BoolResultStr
                 {
                     DESERT_PROFILE_PASS( "PathTracing: OIDN" );
                     const auto            start    = std::chrono::steady_clock::now();
                     Common::BoolResultStr enqueued = Common::MakeError( "OIDN: no denoiser is bound" );
                     if ( m_Oidn.Denoiser )
                         enqueued = m_Oidn.Denoiser->EnqueueShared( sync.Signal.Value, sync.Wait.Value );
                     if ( !enqueued )
                     {
                         // The pass faults and the graph is abandoned: the recorder signals the odd value from the
                         // host, the even one is never waited (the next pair is above it). The next frame shows
                         // the raw sum and says why.
                         LOG_ERROR(
                              "[PathTracing] OIDN did not denoise the {}x{} image of {} paths per pixel: {}; "
                              "the view shows the raw accumulation",
                              key.Width, key.Height, key.Paths, enqueued.GetError() );
                         Denoise::OidnDenoiser::NoteError( enqueued.GetError() );
                         m_Oidn.Phase = OidnPhase::Refused;
                         return enqueued;
                     }
                     const double ms =
                          std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - start )
                               .count();
                     Denoise::OidnDenoiser::NoteRun( key.Paths, key.Width, key.Height, ms );
                     LOG_INFO(
                          "[PathTracing] OIDN denoised the {}x{} image of {} paths per pixel (restart {}) on {} "
                          "in frame {} (enqueued in {:.2f} ms, timeline {} -> {})",
                          key.Width, key.Height, key.Paths, key.Restart, m_Oidn.Denoiser->GetDeviceName(), frame,
                          ms, sync.Signal.Value, sync.Wait.Value );
                     return enqueued;
                 } );

            graph.AddPass(
                 "PathTracing: OIDN Unpack", RDG::PassFlags::Compute,
                 [&]( RDG::PassBuilder& pass )
                 {
                     using P = PathTracingOidnUnpackShaderParameters;
                     pass.Bindings( m_DenoiseOidnUnpack.PassLayout.Get( m_DenoiseOidnUnpack.PassShader ),
                                    Renderer::GetPipelineRouteFill( *m_DenoiseOidnUnpack.PassPipeline ) )
                          .Storage( P::PathTracingOidnOutputUB, output, kR )
                          .Storage( P::OidnResultTexture, result, kW );
                 },
                 [this, width, height]( RDG::PassContext& context ) -> Common::BoolResultStr
                 {
                     DESERT_PROFILE_PASS( "PathTracing: OIDN Unpack" );
                     return DispatchDenoise( context, m_DenoiseOidnUnpack, nullptr, width, height );
                 } );
            return result;
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

        // "PathTracing: Light Grid" (THE LIGHT CHOICE above): this frame's grid around the camera when @p sampling
        // is Grid and the scene has local lights; otherwise one empty slot the trace binds and never reads (Size.x
        // = 0). The seed moves with every traced frame (the restarts and the paths summed), so each frame's slots
        // are independent draws and the sum converges.
        LightGridTargets AddLightGridPass( RDG::Builder& graph, const FrameInputs& inputs, const ViewFrame& frame,
                                           const LightSamplingMethod sampling )
        {
            LightGridTargets grid;
            const uint32_t   lights = inputs.PointLightCount + inputs.SpotLightCount;
            if ( sampling != LightSamplingMethod::Grid || lights == 0u )
            {
                grid.Buffer =
                     graph.CreateBuffer( RDG::BufferDesc{ sizeof( glm::uvec2 ) }, "PathTracing.LightGrid" );
                const glm::uvec2 empty{ 0xFFFFFFFFu, 0u };
                graph.QueueBufferUpload( grid.Buffer, std::as_bytes( std::span( &empty, 1 ) ) );
                return grid;
            }
            const glm::uvec3 cellCount( kLightGridCellsXZ, kLightGridCellsY, kLightGridCellsXZ );
            const glm::vec3  cells( cellCount );
            const glm::vec3  corner =
                 ( glm::floor( frame.CameraPosition / kLightGridCellSide ) - glm::floor( cells * 0.5f ) ) *
                 kLightGridCellSide;
            grid.Grid = glm::vec4( corner, kLightGridCellSide );
            grid.Size = glm::uvec4( cellCount, kLightGridSlots );
            const uint64_t slots =
                 static_cast<uint64_t>( cellCount.x ) * cellCount.y * cellCount.z * kLightGridSlots;
            grid.Buffer =
                 graph.CreateBuffer( RDG::BufferDesc{ slots * sizeof( glm::uvec2 ) }, "PathTracing.LightGrid" );

            const LightGridPush push{ grid.Grid, grid.Size,
                                      glm::uvec4( inputs.PointLightCount, inputs.SpotLightCount,
                                                  kLightGridCandidates, m_Restarts * 0x9E3779B9u + m_Summed ) };
            graph.AddPass(
                 "PathTracing: Light Grid", RDG::PassFlags::Compute,
                 [&]( RDG::PassBuilder& pass )
                 {
                     pass.Bindings( m_LightGridLayout.Get( m_LightGridShader ),
                                    Renderer::GetPipelineRouteFill( *m_LightGridPipeline ) )
                          .Storage( PathTracingLightGridShaderParameters::PointLightsUB, inputs.PointLights,
                                    RDG::Access::StorageRead )
                          .Storage( PathTracingLightGridShaderParameters::SpotLightsUB, inputs.SpotLights,
                                    RDG::Access::StorageRead )
                          .Storage( PathTracingLightGridShaderParameters::PathTracingLightGridUB, grid.Buffer,
                                    RDG::Access::StorageWrite )
                          .PushConstantBytes( static_cast<uint32_t>( sizeof( LightGridPush ) ) );
                 },
                 [this, push, slots]( RDG::PassContext& context ) -> Common::BoolResultStr
                 {
                     DESERT_PROFILE_PASS( "PathTracing: Light Grid" );
                     RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                     bindings.PushConstants( &push, static_cast<uint32_t>( sizeof( push ) ) );
                     return Renderer::GetInstance().DispatchCompute(
                          bindings, *m_LightGridPipeline,
                          static_cast<uint32_t>( ( slots + kLightGridGroupSize - 1u ) / kLightGridGroupSize ), 1,
                          1 );
                 } );
            return grid;
        }

        // "PathTracing: Trace": @p traced paths per pixel added to the accumulation (one launch per pixel).
        // @p nrc: this frame's cache targets (the PATH_TRACING_NRC=1 trace writes them), or nullptr.
        void AddTracePass( RDG::Builder& graph, const AccumulationRefs& accumulation, const SkyTargets& sky,
                           const LightGridTargets& grid, const FrameInputs& inputs, const uint32_t width,
                           const uint32_t height, const uint32_t traced, const Engine::Neural::NrcTraceTargets* nrc )
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
                 grid.Grid,
                 grid.Size };
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
                                        RDG::Access::StorageWrite )
                              .Storage( PathTracingTraceShaderParameters::NrcTraceParamsUB, nrc->Params, kR );
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
                          .Storage( PathTracingTraceShaderParameters::PathTracingLightGridUB, grid.Buffer, kR )
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
        std::shared_ptr<Shader>             m_LightGridShader;
        std::shared_ptr<Shader>             m_SkyReduceShader;
        std::shared_ptr<Shader>             m_ResolveShader;
        DenoiseProgram                      m_DenoiseSnapshot{ .Name = "Snapshot" };
        DenoiseProgram                      m_DenoiseTemporal{ .Name = "Temporal" };
        DenoiseProgram                      m_DenoiseCombine{ .Name = "Combine" };
        DenoiseProgram                      m_DenoiseAntiFirefly{ .Name = "AntiFirefly" };
        DenoiseProgram                      m_DenoiseFilter{ .Name = "Filter" };
        DenoiseProgram                      m_DenoiseRemodulate{ .Name = "Remodulate" };
        DenoiseProgram                      m_DenoiseOidnPack{ .Name = "OidnPack" };
        DenoiseProgram                      m_DenoiseOidnUnpack{ .Name = "OidnUnpack" };
        std::shared_ptr<RayTracingPipeline> m_TracePipeline;
        std::vector<std::string>            m_PipelineTemplates;
        std::shared_ptr<ComputePipeline>    m_SkyPreparePipeline;
        std::shared_ptr<ComputePipeline>    m_SkyReducePipeline;
        std::shared_ptr<ComputePipeline>    m_LightGridPipeline;
        std::shared_ptr<GraphicsPipeline>   m_ResolvePipeline;
        // The block layouts, derived from each shader's reflection once per compile (not per frame).
        mutable ShaderBindingLayoutCache m_TraceLayout;
        mutable ShaderBindingLayoutCache m_SkyPrepareLayout;
        mutable ShaderBindingLayoutCache m_SkyReduceLayout;
        mutable ShaderBindingLayoutCache m_LightGridLayout;
        mutable ShaderBindingLayoutCache m_ResolveLayout;

        AccumulationTarget m_Accumulation;
        HistoryTarget      m_History;
        bool m_HistoryValid = false; // m_History holds THE DENOISER'S HISTORY of the current sum's pose
        // Accumulations a resize replaced, kept until no frame in flight can still read them.
        Assets::FrameRetireQueue<RDG::ExternalTexture> m_RetiringAccumulation;
        PassHistoryStamp   m_Stamp;               // the view frame whose resolve last stood behind the sum
        uint32_t           m_Summed      = 0;     // paths per pixel in the sum
        uint32_t           m_MaxBounces  = 0;     // the bounces the sum was traced with
        LightSamplingMethod m_LightSampling =
             LightSamplingMethod::AllLights;          // the light choice the sum was traced with
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
        CacheInputs m_RadianceCacheInputs;       // what m_RadianceCache was built under (EnsureRadianceCache)
        uint32_t    m_RadianceCacheRebuilds = 0; // RadianceCacheStatus::Rebuilds
        // Retired caches (RetireRadianceCache), alive until no frame in flight reads their buffers and pipelines.
        Assets::FrameRetireQueue<std::unique_ptr<Engine::Neural::NeuralRadianceCache>> m_RetiringCaches;
        std::shared_ptr<Shader>                               m_TraceShaderNrc;
        std::shared_ptr<RayTracingPipeline>                   m_TracePipelineNrc;
        std::vector<std::string>                              m_PipelineTemplatesNrc;
        mutable ShaderBindingLayoutCache                      m_TraceLayoutNrc;
        OidnState m_Oidn;             // PathTracing.Denoiser OIDN (THE DENOISER above)
    };
} // namespace Desert::Graphic::System
