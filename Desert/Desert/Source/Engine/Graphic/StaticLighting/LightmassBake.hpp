#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Common/Core/UUID.hpp>
#include <Engine/Assets/StaticLighting/LightingBuildData.hpp>
#include <Engine/Assets/StaticLighting/StaticLightingInputs.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/Systems/RayTracing/RayTracingSceneRenderer.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

// LIGHTMASS CONTRACT (owner backlog LIGHTMASS, 10-21) — THE BAKE. Header contract only; no definitions yet.
//
// The model is UE 5's GPU LIGHTMASS (Plugins/Experimental/GPULightmass: FGPULightmass, FLightmapRenderer,
// FVolumetricLightmapRenderer, LightmapPathTracing.usf), not CPU Lightmass (the Swarm-distributed photon mapper
// with irradiance caching). GPU Lightmass is a path tracer over the same ray tracing scene the renderer's path
// tracer uses, run from lightmap texels instead of camera pixels — and this engine already has that path tracer
// (System::PathTracer: NEE + MIS, the importance-sampled sky, emissive triangles, the surface templates' hit
// groups). The baker REUSES, it does not fork:
//   - the TLAS/BLAS of RayTracingSceneRenderer (RayTracingSceneFrame), restricted to CastStaticShadow
//     primitives by the instance mask (a bake-only mask bit, beside RayTracing::kMaskTranslucent);
//   - the hit-group table (RayTracingHitGroupTable): materials shade a bake hit exactly as a path-traced pixel;
//   - EmissiveLightsFrame (emissive surfaces are light sources of the bake, UE's bUseEmissiveForStaticLighting);
//   - the path tracer's sky preparation (PathTracerSky.shader Prepare/Reduce) and its NEE light sampling;
//   - the shared path integrator include (Common/PathTracing*.glslh), entered at a texel instead of a pixel.
// What is NEW: the texel G-buffer (each primitive rasterised into its lightmap UV space: world position, normal,
// tangent frame per texel, UE's FLightmapGBuffer), the lightmap/shadowmap/volume accumulation targets, the
// stationary shadow pass (UE: N shadow rays to the light's area per texel, a distance field written), the
// denoise, and the encode into LightingBuildDataAsset.
//
// WHERE IT RUNS (UE: GPU Lightmass is progressive inside the editor's frame): an editor-only job that adds
// passes to the editor's frame graph each frame within a time budget (LightmassSettings::FrameBudgetMs) until
// converged, the viewport staying interactive; the scene is FROZEN for the bake's static set (an edit to a
// Static primitive or light restarts it, as UE's realtime preview does). No separate process, no Swarm.
namespace Desert::Graphic::StaticLighting
{
    // The level's bake settings (UE: AWorldSettings::LightmassSettings, FLightmassWorldInfoSettings, plus GPU
    // Lightmass's UGPULightmassSettings). Q1: a member `Lightmass` of Core::SceneSettings — the World Settings —
    // drawn by the World Settings panel; no separate settings component or asset. Their hash is
    // LightingBuildDataAsset::SettingsHash and StaticLightingInputs::SettingsHash.
    struct LightmassSettings
    {
        uint32_t GISamplesPerTexel        = 512;  // GPU Lightmass GISamples
        uint32_t StationaryShadowSamples  = 128;  // GPU Lightmass StationaryLightShadowSamples
        uint32_t MaxBounces               = 8;    // the path tracer's; CPU Lightmass's NumIndirectLightingBounces
        float    StaticLightingLevelScale = 1.0f; // UE: scales every lightmap resolution of the level
        bool     BuildVolumetricLightmap  = true;
        float    VolumetricLightmapDetailCellSize = 200.0f; // cm, UE's default
        uint32_t VolumetricLightmapMaxBricksMB    = 128;    // UE VolumetricLightmapMaximumBrickMemoryMb
        bool     UseSkyOcclusion                  = true;
        bool     Denoise                          = true; // Q3: Intel Open Image Denoise (ThirdParty, CPU), as UE GPU Lightmass
        float    FrameBudgetMs                    = 30.0f;
    };

    enum class BakeStage : uint8_t
    {
        Idle = 0,
        GatherScene,       // the static set, texel layout and atlas packing (CPU, JobSystem)
        TexelGBuffer,      // primitives rasterised into lightmap space
        Lightmaps,         // GI + direct of Static lights, sky occlusion, per texel
        StationaryShadows, // distance-field shadow maps of stationary lights
        VolumetricLightmap,
        Denoise, // Q3: OIDN over the read-back lightmap and VLM layers (JobSystem); no engine-written denoiser
        Encode, // compression + LightingBuildDataAsset written, the scene points at it
        Done,
        Failed,
        Cancelled,
    };

    struct BakeProgress
    {
        BakeStage Stage                   = BakeStage::Idle;
        float     StageFraction           = 0.0f; // 0..1 of the current stage
        uint64_t  TexelsTotal             = 0;
        uint64_t  TexelsConverged         = 0;
        uint32_t  StationaryOverlapLosers = 0; // lights past the 4-channel limit
    };

    // What the bake gathered as the static set is Assets::StaticLightingInputs (StaticLightingInputs.hpp): the same
    // gather and hash the cook's gate uses (Q8), so there is no second record type here.

    // ONE BAKE: created ONLY by the editor's Build > Build Lighting, driven by the editor frame. Q8: the cook never
    // bakes; it packages the saved build and is refused by Assets::CheckLightingBuildCurrent when it is stale.
    class ILightmassBake
    {
    public:
        virtual ~ILightmassBake() = default;

        // Each editor frame: adds this frame's bake passes to the graph within the budget, reading the frame's
        // ray tracing scene. Returns false once the bake is Done / Failed / Cancelled (no pass added).
        virtual bool AddFramePasses( RDG::Builder& graph, const System::RayTracingSceneFrame& scene ) = 0;

        [[nodiscard]] virtual BakeProgress Progress() const = 0;
        virtual void                       Cancel()         = 0;

        // Done: the written build (already saved; the scene names its GUID). Failed: why.
        [[nodiscard]] virtual Common::ResultStr<std::shared_ptr<Assets::LightingBuildDataAsset>>
        Result() const = 0;

        [[nodiscard]] virtual const Assets::StaticLightingInputs& Inputs() const = 0;
    };

    // The GPU Lightmass baker over the hardware ray tracing pipeline. Refused (an error naming the missing
    // capability, NeedsRayTracingPipeline) on a device without it: the bake has no software fallback (UE's CPU
    // Lightmass is not ported — LIGHTMASS.md "Not ported").
    [[nodiscard]] Common::ResultStr<std::unique_ptr<ILightmassBake>>
    CreateGpuLightmassBake( const LightmassSettings& settings, std::string outputPath );
} // namespace Desert::Graphic::StaticLighting
