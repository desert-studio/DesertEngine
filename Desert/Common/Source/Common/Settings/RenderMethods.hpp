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
        // A reflective shadow map from the sun: off-screen bounce of the sun only. It stays until probe volumes
        // replace it (GI-DDGI deletes it, with a scene migration); it has no place in the 2026 set.
        ReflectiveShadowMaps,
        ProbeVolume,    // DDGI probe volumes (GI-DDGI)                                   — not implemented
        SoftwareTraced, // global SDF + world radiance cache (GLOBAL-SDF, GI/REFL-SW)      — not implemented
        HardwareTraced, // ray query: screen probes + radiance cache (GI-HWRT); RT device — not implemented
        PathTraced,     // reference / Cinematic (PATHTRACER); RT device                  — not implemented
    };

    enum class ReflectionMethod : int
    {
        Off = 0,        // the sky's prefiltered environment only
        Captures,       // sky + reflection captures, parallax-corrected (REFL-CAPTURE)   — not implemented
        ScreenSpace,    // screen-space trace; a miss shows the sky environment
        SoftwareTraced, // global SDF + radiance cache (GI/REFL-SW)                       — not implemented
        HardwareTraced, // screen ray continued into the TLAS (RT-REFL); RT device        — not implemented
        PathTraced,     // reference (PATHTRACER); RT device                              — not implemented
    };

    enum class ShadowMethod : int
    {
        CascadedMaps = 0, // the sun's cascaded shadow maps
        VirtualMaps,      // virtual shadow maps (SHADOW-VSM)                            — not implemented
        HardwareTraced,   // ray-query visibility (RT-SHADOWS); RT device                — not implemented
        Stochastic,       // many-light ReSTIR DI + ray-query visibility (MEGALIGHTS)    — not implemented
    };

    // By name: Scalability.json, the log line and the overlay print these, one list per enum.
    inline constexpr std::array<std::string_view, 8> kGIMethodNames{
         "Off",         "Lightmaps",      "ScreenSpace",    "ReflectiveShadowMaps",
         "ProbeVolume", "SoftwareTraced", "HardwareTraced", "PathTraced" };
    inline constexpr std::array<std::string_view, 6> kReflectionMethodNames{
         "Off", "Captures", "ScreenSpace", "SoftwareTraced", "HardwareTraced", "PathTraced" };
    inline constexpr std::array<std::string_view, 4> kShadowMethodNames{ "CascadedMaps", "VirtualMaps",
                                                                         "HardwareTraced", "Stochastic" };
    static_assert( static_cast<std::size_t>( GIMethod::PathTraced ) + 1 == kGIMethodNames.size() );
    static_assert( static_cast<std::size_t>( ReflectionMethod::PathTraced ) + 1 == kReflectionMethodNames.size() );
    static_assert( static_cast<std::size_t>( ShadowMethod::Stochastic ) + 1 == kShadowMethodNames.size() );

    // ---- What this build renders. The ONE table per enum; a method's task flips its row. ----------------------
    [[nodiscard]] constexpr bool IsImplemented( GIMethod method )
    {
        return method == GIMethod::Off || method == GIMethod::ScreenSpace ||
               method == GIMethod::ReflectiveShadowMaps;
    }
    [[nodiscard]] constexpr bool IsImplemented( ReflectionMethod method )
    {
        return method == ReflectionMethod::Off || method == ReflectionMethod::ScreenSpace;
    }
    [[nodiscard]] constexpr bool IsImplemented( ShadowMethod method )
    {
        return method == ShadowMethod::CascadedMaps;
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

    // ---- Which methods hand the frame a NOISY signal (stochastic rays or taps) that a denoiser cleans: the
    // screen-space denoiser (Graphic::System::ScreenSpaceDenoiser), or the temporal upscaler that takes the
    // signals raw (DLSS Ray Reconstruction, Graphic::ITemporalUpscaler::TakesRawLightingSignals). A method of this
    // list that is not implemented yet is listed by what it will be, so its renderer lands into the same rule.
    [[nodiscard]] constexpr bool IsDenoisedSignal( ReflectionMethod method )
    {
        return method == ReflectionMethod::ScreenSpace || method == ReflectionMethod::SoftwareTraced ||
               method == ReflectionMethod::HardwareTraced || method == ReflectionMethod::PathTraced;
    }
    [[nodiscard]] constexpr bool IsDenoisedSignal( GIMethod method )
    {
        return method == GIMethod::ReflectiveShadowMaps || method == GIMethod::SoftwareTraced ||
               method == GIMethod::HardwareTraced || method == GIMethod::PathTraced;
    }
    [[nodiscard]] constexpr bool IsDenoisedSignal( ShadowMethod method )
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
