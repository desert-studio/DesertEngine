#pragma once

#include <Common/Settings/CapabilityCatalog.hpp>
#include <Common/Settings/RenderMethods.hpp>
#include <Common/Settings/Scalability.hpp>
#include <Engine/Core/ShaderCompiler/ShaderVariant.hpp>
#include <Engine/Graphic/RDG/RDGResources.hpp>
#include <Engine/Graphic/Systems/RayTracing/RayTracingSceneMetadata.hpp>

#include <array>
#include <cstdint>
#include <string>
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
// THE CONTRACT IS NEUTRAL TO THE TRACER (GI-TRACER-CONTRACT, agreed with the Windows side 10-15). What differs
// between the walks lives behind four names and nowhere else:
//   * the shader permutation — TracerVariant( Pipeline ): the one place Pipeline::Tracer becomes a define;
//   * the tracer's graph reads — TracerFrame + DeclareTracerReads( block, frame ): a consumer pass declares its
//     own reads and then this, never a distance-field texture or a TLAS by name;
//   * the ray bias and the screen-match tolerance — GIRayBias( position, normal, direction ), defined by the
//   tracer
//     (UE: the SDF tracer's surface bias vs. the hardware tracer's RayBias), so the shared hit lighting knows no
//     SDF;
//   * visibility — GITraceVisibility( origin, direction, maxT ), declared in Common/GITracing.glslh.
// No consumer (C++ pass or shader) branches on the tracer.
//
// WHERE THIS DIFFERS FROM UE (for the owner): UE's surface-cache lighting of a hit is black where the hit has no
// card coverage (FSurfaceCacheSample.bValid = false). GIHitRadiance lights such a hit in place on BOTH tracers
// (Graphic/GlobalIllumination/SurfaceCache.hpp, header note), so coverage holes cost precision, never light.
namespace Desert::Graphic::RDG
{
    class BindingBlockBuilder;
}

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
        bool Traced = false; // false: none of the below runs (the other GI methods)
        // THE one statement of which tracer walks the rays: TracerVariant and TracerFrame read it, nothing else.
        GlobalIllumination::Tracer Tracer = GlobalIllumination::Tracer::SoftwareDistanceField;
        HitLighting                Hits   = HitLighting::SurfaceCache;
        // The caches and the gather the traced methods share. Both traced methods need all three today; they are
        // separate flags because GI-FINAL-GATHER's first step runs without screen probes (decision 1, 10-15).
        bool SurfaceCache  = false;
        bool RadianceCache = false;
        bool ScreenProbes  = false;

        constexpr bool operator==( const Pipeline& ) const = default;
    };

    // PURE. @p method = the EFFECTIVE method (ResolvedMethod::Effective), never the request; @p hitLightingAllowed
    // = Scalability GlobalIllumination.HitLighting; @p deviceRayQuery = the device offers inline ray tracing
    // (CapabilityCatalog::RayTracingModes, filled from DeviceCaps RayQuery). Hit lighting is a hardware-tracer
    // mode: the software walk ends at a distance-field surface that carries no triangle, hence no material to
    // evaluate. Without ray query neither the hardware tracer nor hit lighting is switched on, whatever the method
    // says — ResolveRenderMethods lowers such a method first, this keeps the pipeline right on its own.
    [[nodiscard]] constexpr Pipeline PipelineFor( Common::Scalability::GIMethod method, bool hitLightingAllowed,
                                                  bool deviceRayQuery )
    {
        using Common::Scalability::GIMethod;
        if ( method != GIMethod::SoftwareTraced && method != GIMethod::HardwareTraced )
            return Pipeline{};

        Pipeline pipeline;
        pipeline.Traced        = true;
        pipeline.SurfaceCache  = true;
        pipeline.RadianceCache = true;
        pipeline.ScreenProbes  = true;
        if ( method == GIMethod::HardwareTraced && deviceRayQuery )
        {
            pipeline.Tracer = Tracer::HardwareRayQuery;
            pipeline.Hits   = hitLightingAllowed ? HitLighting::InPlace : HitLighting::SurfaceCache;
        }
        return pipeline;
    }

    // PURE. The same, with the hit-lighting allowance read from the machine's quality and ray query from the
    // device's catalogue.
    [[nodiscard]] inline Pipeline PipelineFor( Common::Scalability::GIMethod                 method,
                                               const Common::Scalability::ResolvedQuality&   quality,
                                               const Common::Scalability::CapabilityCatalog& device )
    {
        using Common::Scalability::CapabilityCatalog;
        using Common::Scalability::RayTracingMode;
        return PipelineFor( method, quality.As<int>( Common::Scalability::Parameter::HitLightingAllowed ) != 0,
                            CapabilityCatalog::Offers( device.RayTracingModes, RayTracingMode::RayQuery ) );
    }

    // PURE. Whether this pipeline's tracer reads the global distance field — what the frame's owner asks before it
    // brings the field up for GI (the software walk marches it; the ray-query walk does not need it).
    [[nodiscard]] constexpr bool TracerReadsGlobalDistanceField( const Pipeline& pipeline )
    {
        return pipeline.Traced && pipeline.Tracer == Tracer::SoftwareDistanceField;
    }

    // The permutation a tracing consumer program is compiled under: the tracer's define and the hit-lighting
    // define. Every pass that traces acquires its program with this (ShaderService::AcquireVariant).
    [[nodiscard]] inline Core::ShaderVariant TracerVariant( const GlobalIllumination::Pipeline& pipeline )
    {
        return Core::ShaderVariant{ .Defines = { std::string( TracerDefine( pipeline.Tracer ) ),
                                                 std::string( HitLightingDefine( pipeline.Hits ) ) } };
    }

    // THIS FRAME'S RESOURCES OF THE ACTIVE TRACER — what DeclareTracerReads binds, with the pipeline that chose
    // the tracer (so the permutation and the reads cannot disagree). The frame's owner (SceneRenderer) fills it
    // once; a consumer hands it to TracerVariant / TracerFrameMissing / DeclareTracerReads and reads no member
    // itself.
    //   SoftwareDistanceField  the four global distance field clipmaps (GLOBAL-SDF; their placement rides in the
    //                          GI frame record, GIFrame.gdfLevels);
    //   HardwareRayQuery       the view's scene TLAS (RayTracingSceneFrame::Tlas, RT-SCENE), its per-segment
    //                          geometry and material records (RayTracingSceneFrame::Metadata, RT-SHADOWS: the hit's
    //                          vertex streams and alpha test), and the surface cache's MeshCards index per TLAS
    //                          instance custom index (GI-SURFACE-CACHE; one uint per instance, DECISIONS-1015 S4).
    struct TracerFrame
    {
        GlobalIllumination::Pipeline        Pipeline{};
        std::array<RDG::TextureRef, 4>      GlobalDistanceField{};
        RDG::BufferRef                      Tlas;
        RayTracing::RayTracingMetadataFrame Metadata;
        RDG::BufferRef                      InstanceMeshCards;
    };

    // The shader names of the hardware tracer's four reads (Common/GITracer.glslh declares them at
    // GI_TRACER_BINDING + 0..3; the record blocks are Common/RayQueryVisibility.glslh's, shared with RT-SHADOWS).
    inline constexpr std::string_view kHardwareTracerTlasName              = "u_SceneTLAS";
    inline constexpr std::string_view kHardwareTracerGeometryName          = "RayTracingGeometryRecords";
    inline constexpr std::string_view kHardwareTracerMaterialsName         = "RayTracingMaterialRecords";
    inline constexpr std::string_view kHardwareTracerInstanceMeshCardsName = "GIInstanceMeshCards";

    // Empty when @p frame holds every resource its tracer reads; otherwise what is missing (for the consumer's
    // one error line).
    [[nodiscard]] std::string_view TracerFrameMissing( const TracerFrame& frame );

    // Declares the tracer's reads on a consumer pass's binding block, under the shader names the tracer include
    // (Common/GITracer.glslh) declares. The consumer's program must be compiled with TracerVariant.
    void DeclareTracerReads( RDG::BindingBlockBuilder& block, const TracerFrame& frame );
} // namespace Desert::Graphic::GlobalIllumination
