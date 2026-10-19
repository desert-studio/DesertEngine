#pragma once

#include <array>
#include <cstdint>
#include <string_view>

// THE THREE RENDER METHODS — how a view lights indirectly, reflects and shadows (RENDER-ARCH, owner 10-13).
//
// ONE CHOICE, THREE INPUTS, ONE PLACE. The look is authored per view (PostProcessSettings:
// GlobalIlluminationMethod, ReflectionsMethod, ShadowsMethod on a PostProcessVolume), the machine sets a ceiling
// (Scalability "GlobalIllumination.Method" / "Reflections.Method" / "Shadows.Method"), the device offers what it
// can (the CapabilityCatalog's RayTracingModes). What a frame renders is the MINIMUM of the three, walked down to
// a method this build implements — Graphic::ResolveRenderMethods, the one point; a downgrade is one log line and
// one row in the viewport overlay. UE spreads the same choice over four places (Project Settings, the post-process
// Method, r.Lumen.HardwareRayTracing, sg.GlobalIlluminationQuality) and a view can end up in a mode none of
// them states.
//
// THE ORDER OF EACH ENUM IS ITS LADDER: a later value costs more and needs more; a downgrade walks towards the
// front. Values are stored by number in scene files and by name in Scalability.json, so a new method is
// APPENDED where its cost puts it only together with a scene migration.
//
// DECLARED IS NOT IMPLEMENTED. Every method of the programme is declared now, so the files, the Scalability table
// and the downgrade chain have their final shape; a method whose renderer has not landed is refused with an
// error wherever a view's method is SET (the scene loader, Details, scripts — RefuseGIMethod and its siblings,
// attached to the reflected fields) and is never the result of a resolution. It is not a stub: nothing renders
// "as if" it were that method.
//
// WHY IT LIVES IN COMMON. Scalability (Common) names the ceilings, the engine's PostProcessSettings authors the
// request; both must spell the same enum.
namespace Common::Scalability
{
    enum class GIMethod : int
    {
        Off = 0,     // sky light and ambient occlusion only
        Lightmaps,   // baked by the path tracer (LIGHTMAPS)                          — not implemented
        ScreenSpace, // gather from the lit G-buffer on screen
        // The world radiance cache read per pixel, no screen probes (GI-PROBE-SW; UE Lumen's irradiance field
        // gather): the cheap traced path, every light and the sky bounced off screen too. It took the number of
        // the reflective shadow maps it replaced (3); every method above moved down one, and scene v64
        // (SceneMigrator, MigrateGIMethodLadderV63ToV64) re-stated the numbers older files wrote.
        ProbeVolume,
        SoftwareTraced, // global SDF + world radiance cache + screen probes (GLOBAL-SDF, GI-RADIANCE-CACHE,
                        // GI-FINAL-GATHER)
        HardwareTraced, // ray query: screen probes + radiance cache (GI-HWRT); the tracer record carries the frame's
                        // TLAS, segment records and instance -> surface-cache rows (MakeTracerFrame); RT device
        // THE PATH TRACER (PATHTRACER; UE's Path Tracer, Renderer/Private/PathTracing): the view's whole lit image
        // is a progressive Monte Carlo estimate over the same ray tracing pipeline and hit groups (next event
        // estimation + MIS, the sky importance-sampled), accumulated per view. GI and reflections are ONE
        // integrator there, so a frame path-traces only when both axes resolve to PathTraced
        // (Graphic::ResolveRenderMethods couples them). Needs the ray tracing PIPELINE (NeedsRayTracingPipeline).
        // The frame: SceneRenderer::AddFramePathTracing (System::PathTracer) in place of the lit passes.
        PathTraced,
    };

    enum class ReflectionMethod : int
    {
        Off = 0,        // the sky's prefiltered environment only
        Captures,       // sky + reflection captures, parallax-corrected (ReflectionCaptureRenderer; UE's
                        // reflection environment with r.SSR off)
        ScreenSpace,    // screen-space trace composed over the captures; a miss shows the captures, then the sky
                        // (UE r.ReflectionMethod 2: SSR, then the reflection environment)
        SoftwareTraced, // the screen trace continued through the GI's tracer - mesh SDFs, then the global SDF -,
                        // a hit lit from the surface cache, a miss answered by the captures, then the sky; past
                        // UE's MaxRoughnessToTrace the screen alone (REFL-SW; UE Lumen reflections)
        HardwareTraced, // SoftwareTraced's pipeline with the ray-query tracer: the screen trace continued into the
                        // view's TLAS (alpha-tested), a hit lit from the surface cache, a miss answered by the
                        // captures, then the sky (RT-REFL; UE Lumen reflections, hardware ray tracing); RT device
        PathTraced,     // the path tracer, GIMethod::PathTraced's other half (one integrator); RT pipeline device
    };

    enum class ShadowMethod : int
    {
        CascadedMaps = 0, // the sun's cascaded shadow maps
        VirtualMaps,      // virtual shadow maps (SHADOW-VSM)                            — not implemented
        HardwareTraced,   // ray-query visibility (RT-SHADOWS: RayTracedSunShadow); RT device
        Stochastic,       // many-light ReSTIR DI + ray-query visibility (MEGALIGHTS)    — not implemented
    };

    // By name: Scalability.json, the log line and the overlay print these, one list per enum.
    inline constexpr std::array<std::string_view, 7> kGIMethodNames{
         "Off", "Lightmaps", "ScreenSpace", "ProbeVolume", "SoftwareTraced", "HardwareTraced", "PathTraced" };
    inline constexpr std::array<std::string_view, 6> kReflectionMethodNames{
         "Off", "Captures", "ScreenSpace", "SoftwareTraced", "HardwareTraced", "PathTraced" };
    inline constexpr std::array<std::string_view, 4> kShadowMethodNames{ "CascadedMaps", "VirtualMaps",
                                                                         "HardwareTraced", "Stochastic" };
    static_assert( static_cast<std::size_t>( GIMethod::PathTraced ) + 1 == kGIMethodNames.size() );
    static_assert( static_cast<std::size_t>( ReflectionMethod::PathTraced ) + 1 == kReflectionMethodNames.size() );
    static_assert( static_cast<std::size_t>( ShadowMethod::Stochastic ) + 1 == kShadowMethodNames.size() );

    // ---- What this build renders. The ONE table per enum; a method's task flips its row. ----------------------
    // GI HardwareTraced is NOT here: GITracing's ray-query tracer and its shader permutation exist, but the frame's
    // tracer record (SceneRenderer::MakeTracerFrame) fills no TLAS, ray-tracing records or the surface cache's
    // instance -> MeshCards table, so TracerFrameMissing would refuse every tracing pass (RefuseGIMethod says so).
    [[nodiscard]] constexpr bool IsImplemented( GIMethod method )
    {
        return method == GIMethod::Off || method == GIMethod::ScreenSpace || method == GIMethod::ProbeVolume ||
               method == GIMethod::SoftwareTraced || method == GIMethod::HardwareTraced ||
               method == GIMethod::PathTraced;
    }
    [[nodiscard]] constexpr bool IsImplemented( ReflectionMethod method )
    {
        return method == ReflectionMethod::Off || method == ReflectionMethod::Captures ||
               method == ReflectionMethod::ScreenSpace || method == ReflectionMethod::SoftwareTraced ||
               method == ReflectionMethod::HardwareTraced || method == ReflectionMethod::PathTraced;
    }
    [[nodiscard]] constexpr bool IsImplemented( ShadowMethod method )
    {
        return method == ShadowMethod::CascadedMaps || method == ShadowMethod::HardwareTraced;
    }
    // The two shadow supplements beside the method (PostProcessSettings ContactShadows / DistanceFieldShadows):
    // contact shadows (SHADOW-CONTACT) and distance-field shadows (GLOBAL-SDF) — neither implemented yet.
    inline constexpr bool kContactShadowsImplemented       = false;
    inline constexpr bool kDistanceFieldShadowsImplemented = false;

    // ---- What a method needs from the device: inline ray tracing (CapabilityCatalog RayQuery). ----------------
    [[nodiscard]] constexpr bool NeedsRayQuery( GIMethod method )
    {
        return method == GIMethod::HardwareTraced || method == GIMethod::PathTraced;
    }
    [[nodiscard]] constexpr bool NeedsRayQuery( ReflectionMethod method )
    {
        return method == ReflectionMethod::HardwareTraced || method == ReflectionMethod::PathTraced;
    }
    [[nodiscard]] constexpr bool NeedsRayQuery( ShadowMethod method )
    {
        return method == ShadowMethod::HardwareTraced || method == ShadowMethod::Stochastic;
    }

    // ---- ... and the ray tracing PIPELINE (CapabilityCatalog RayTracingPipeline: ray generation, hit groups, SBT;
    // never on MoltenVK): the path tracer's paths shade every hit with its surface's own hit group. ---------------
    [[nodiscard]] constexpr bool NeedsRayTracingPipeline( GIMethod method )
    {
        return method == GIMethod::PathTraced;
    }
    [[nodiscard]] constexpr bool NeedsRayTracingPipeline( ReflectionMethod method )
    {
        return method == ReflectionMethod::PathTraced;
    }
    [[nodiscard]] constexpr bool NeedsRayTracingPipeline( ShadowMethod /*method*/ )
    {
        return false;
    }

    // ---- What a method needs from the render path: the G-buffer (Core::RenderPath::Deferred). ------------------
    // These methods work per screen pixel from the G-buffer's depth and normal (screen-space GI/reflections, the
    // irradiance field gather, the traced methods' ray origins, the sun's screen shadow mask), so the forward path (no
    // G-buffer, or a debug view that forces it) has no producer for them. UE's forward renderer drops the same set
    // (no SSR, no SSGI, no Lumen, ray-traced shadows fall back to shadow maps).
    [[nodiscard]] constexpr bool NeedsGBuffer( GIMethod method )
    {
        return method == GIMethod::ScreenSpace || method == GIMethod::ProbeVolume ||
               method == GIMethod::SoftwareTraced || method == GIMethod::HardwareTraced ||
               method == GIMethod::PathTraced;
    }
    [[nodiscard]] constexpr bool NeedsGBuffer( ReflectionMethod method )
    {
        // Captures too: the captures are composited by the deferred lighting only (the forward pass reads the sky).
        return method == ReflectionMethod::Captures || method == ReflectionMethod::ScreenSpace ||
               method == ReflectionMethod::SoftwareTraced ||
               method == ReflectionMethod::HardwareTraced || method == ReflectionMethod::PathTraced;
    }
    [[nodiscard]] constexpr bool NeedsGBuffer( ShadowMethod method )
    {
        return method == ShadowMethod::HardwareTraced || method == ShadowMethod::Stochastic;
    }

    // ---- The refusal every setter of a VIEW'S method applies (scene loader, Details, scripts) ----------------
    // Attached to the reflected fields as PROPERTY( Refuse( ... ) ); the value is the stored integer. Empty =
    // admitted; otherwise the reason, one sentence. A number that is no enumerator is refused by the reflection
    // layer itself, before these run.
    [[nodiscard]] std::string_view RefuseGIMethod( std::int64_t value );
    [[nodiscard]] std::string_view RefuseReflectionMethod( std::int64_t value );
    [[nodiscard]] std::string_view RefuseShadowMethod( std::int64_t value );
    [[nodiscard]] std::string_view RefuseContactShadows( std::int64_t value );
    [[nodiscard]] std::string_view RefuseDistanceFieldShadows( std::int64_t value );
} // namespace Common::Scalability
