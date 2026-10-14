#pragma once

#include <Common/Settings/RenderMethods.hpp>
#include <Common/Settings/Scalability.hpp>

#include <cstdint>
#include <string_view>

// HOW A GI RAY IS TRACED AND LIT — the interface (GI-CACHE-IFACE). Port of UE Lumen's tracing contract:
// LumenTracingUtils.ush / LumenSoftwareRayTracing.ush (FConeTraceInput, FConeTraceResult: the software path),
// LumenHardwareRayTracingCommon.ush (FRayTracedLightingContext / Result, ELumenHardwareRayTracingLightingMode:
// the hardware path), LumenSurfaceCacheSampling.ush (EvaluateRayHitFromSurfaceCache: lighting a hit).
//
// ONE ENTRY, TWO TRACERS. Every consumer (radiance-cache probes, screen probes, rough reflections) calls ONE
// GLSL function, GIRayRadiance (Common/GITracing.glslh), and is compiled once per tracer: the shader permutation
// define of Tracer below selects which include provides GITraceRay — the global SDF march (GLOBAL-SDF,
// Common/GlobalDistanceField.glslh) or the inline ray query over the TLAS (RT-SCENE). Both hand their hit to
// the SAME GIHitRadiance; only the walk differs. A miss is the sky. This is UE's shape too (the tracer is an
// include, not a pass), kept because it traces inline: no ray buffer is written and re-read between passes.
//
// WHAT GIMethod RUNS ON THIS (Common/Settings/RenderMethods.hpp, resolved by Graphic::ResolveRenderMethods):
//   SoftwareTraced  — global SDF + surface cache + world radiance cache + screen probes. The path of every GPU
//                     without ray tracing and of Mac; the main path for most players (owner 10-15).
//   HardwareTraced  — ray query + the same caches; a hit is read from the surface cache, or lit IN PLACE with
//                     its full material when the machine allows hit lighting (Scalability
//                     GlobalIllumination.HitLighting; UE r.Lumen.HardwareRayTracing.HitLighting.Allowed, Epic+).
//   Off / Lightmaps / ScreenSpace / ReflectiveShadowMaps / ProbeVolume / PathTraced — none of this runs.
//
// WHERE THIS DIFFERS FROM UE (for the owner): UE's surface-cache lighting of a hit is black where the hit has no
// card coverage (FSurfaceCacheSample.bValid = false). GIHitRadiance lights such a hit in place on BOTH tracers
// (Graphic/GlobalIllumination/SurfaceCache.hpp, header note), so coverage holes cost precision, never light.
namespace Desert::Graphic::GlobalIllumination
{
    // What walks the ray. The permutation define is the one thing a consumer shader is compiled with.
    enum class Tracer : uint8_t
    {
        SoftwareDistanceField = 0, // global SDF (+ mesh SDFs near the origin), UE Lumen software tracing
        HardwareRayQuery,          // rayQueryEXT over the scene TLAS, UE Lumen hardware ray tracing (inline)
    };

    [[nodiscard]] constexpr std::string_view TracerDefine( Tracer tracer )
    {
        return tracer == Tracer::HardwareRayQuery ? "DESERT_GI_TRACER_HARDWARE" : "DESERT_GI_TRACER_SOFTWARE";
    }

    // How a hit becomes radiance (UE ELumenHardwareRayTracingLightingMode: LightingFromSurfaceCache /
    // EvaluateMaterialAndDirectLighting). A hit outside card coverage is lit in place under either mode.
    enum class HitLighting : uint8_t
    {
        SurfaceCache = 0, // FinalLighting of the card texels at the hit
        InPlace,          // the hit's material + every light with a shadow ray + the radiance cache (HW only)
    };

    [[nodiscard]] constexpr std::string_view HitLightingDefine( HitLighting mode )
    {
        return mode == HitLighting::InPlace ? "DESERT_GI_HIT_LIGHTING_IN_PLACE" : "DESERT_GI_HIT_LIGHTING_CACHE";
    }

    // What a resolved GI method runs of this machinery.
    struct Pipeline
    {
        bool        Traced = false; // false: none of the below runs (the other GI methods)
        Tracer      Walk   = Tracer::SoftwareDistanceField;
        HitLighting Hits   = HitLighting::SurfaceCache;
        // The caches and the gather the traced methods share. Both traced methods need all three today; they are
        // separate flags because GI-FINAL-GATHER's first step runs without screen probes (decision 1, 10-15).
        bool SurfaceCache  = false;
        bool RadianceCache = false;
        bool ScreenProbes  = false;

        constexpr bool operator==( const Pipeline& ) const = default;
    };

    // PURE. @p method = the EFFECTIVE method (ResolvedMethod::Effective), never the request; @p hitLightingAllowed
    // = Scalability GlobalIllumination.HitLighting. Hit lighting is a hardware-tracer mode: the software walk
    // ends at a distance-field surface that carries no triangle, hence no material to evaluate.
    [[nodiscard]] constexpr Pipeline PipelineFor( Common::Scalability::GIMethod method, bool hitLightingAllowed )
    {
        using Common::Scalability::GIMethod;
        if ( method != GIMethod::SoftwareTraced && method != GIMethod::HardwareTraced )
            return Pipeline{};

        Pipeline pipeline;
        pipeline.Traced        = true;
        pipeline.SurfaceCache  = true;
        pipeline.RadianceCache = true;
        pipeline.ScreenProbes  = true;
        if ( method == GIMethod::HardwareTraced )
        {
            pipeline.Walk = Tracer::HardwareRayQuery;
            pipeline.Hits = hitLightingAllowed ? HitLighting::InPlace : HitLighting::SurfaceCache;
        }
        return pipeline;
    }

    // PURE. The same, with the hit-lighting allowance read from the machine's quality.
    [[nodiscard]] inline Pipeline PipelineFor( Common::Scalability::GIMethod               method,
                                               const Common::Scalability::ResolvedQuality& quality )
    {
        return PipelineFor( method, quality.As<int>( Common::Scalability::Parameter::HitLightingAllowed ) != 0 );
    }
} // namespace Desert::Graphic::GlobalIllumination
