#pragma once

#include <Common/Settings/CapabilityCatalog.hpp>
#include <Common/Settings/RenderMethods.hpp>
#include <Common/Settings/Scalability.hpp>
#include <Engine/Core/ShaderCompiler/ShaderVariant.hpp>

#include <array>
#include <vector>
#include <Engine/Graphic/DistanceField/MeshDistanceFieldReads.hpp>
#include <Engine/Graphic/RDG/RDGResources.hpp>
#include <Engine/Graphic/RDG/RDGShaderParameters.hpp>
#include <Engine/Graphic/Systems/RayTracing/RayTracingSceneMetadata.hpp>

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
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
//   ProbeVolume     — global SDF + surface cache + world radiance cache, read per pixel without screen probes
//                     (IrradianceFieldGather.hpp; UE Lumen's irradiance field gather), traced less often.
//   SoftwareTraced  — global SDF + surface cache + world radiance cache + screen probes. The path of every GPU
//                     without ray tracing and of Mac; the main path for most players (owner 10-15).
//   HardwareTraced  — ray query + the same caches; a hit is read from the surface cache, or lit IN PLACE with
//                     its full material when the machine allows hit lighting (Scalability
//                     GlobalIllumination.HitLighting; UE r.Lumen.HardwareRayTracing.HitLighting.Allowed, Epic+).
//   Off / Lightmaps / ScreenSpace / PathTraced — none of this runs.
//
// THE CONTRACT IS NEUTRAL TO THE TRACER (GI-TRACER-CONTRACT, agreed with the Windows side 10-15). What differs
// between the walks lives behind four names and nowhere else:
//   * the shader permutation — TracerVariant( Pipeline ): the one place Pipeline::Tracer becomes a define;
//   * the tracer's graph reads — TracerFrame + DeclareTracerReads( block, frame ): a consumer pass declares its
//     own reads and then this, never a distance-field texture or a TLAS by name;
//   * the ray biases and the screen-match tolerance — GIRayBias( position, normal, direction ), defined by the
//     tracer: a near offset (the ray's origin; the software walk's mesh-SDF near field: a few cm) and a far offset
//     the ray must have risen to before the far field counts a hit (GIFarFieldTMin -> GITraceInput.farTMin; the
//     global SDF's voxel) (UE: the mesh-SDF and global-SDF surface biases vs. the hardware tracer's RayBias for
//     both), so the shared hit lighting knows no SDF;
//   * visibility — GITraceVisibility( origin, direction, farTMin, maxT ), declared in Common/GITracing.glslh.
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
        // The caches and the gather the traced methods share. ProbeVolume runs the two caches without screen
        // probes: its final gather is the radiance cache read per pixel (IrradianceFieldGather).
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
        if ( method != GIMethod::ProbeVolume && method != GIMethod::SoftwareTraced &&
             method != GIMethod::HardwareTraced )
            return Pipeline{};

        Pipeline pipeline;
        pipeline.Traced        = true;
        pipeline.SurfaceCache  = true;
        pipeline.RadianceCache = true;
        pipeline.ScreenProbes  = method != GIMethod::ProbeVolume;
        if ( method == GIMethod::HardwareTraced && deviceRayQuery )
        {
            pipeline.Tracer = Tracer::HardwareRayQuery;
            pipeline.Hits   = hitLightingAllowed ? HitLighting::InPlace : HitLighting::SurfaceCache;
        }
        return pipeline;
    }

    // THE GI METHODS EACH PROGRAM OF THIS MACHINERY SERVES: a program that does not build on the device refuses
    // exactly these by its name (SceneRenderer::RefuseProgramMethods, ProgramRefusals), and the view renders the
    // method below. The surface cache and the world radiance cache serve every traced method; the screen probes
    // the two that gather through them; the irradiance field gather ProbeVolume alone.
    inline constexpr std::array<Common::Scalability::GIMethod, 3> kCacheGIMethods{
         Common::Scalability::GIMethod::ProbeVolume, Common::Scalability::GIMethod::SoftwareTraced,
         Common::Scalability::GIMethod::HardwareTraced };
    inline constexpr std::array<Common::Scalability::GIMethod, 2> kScreenProbeGIMethods{
         Common::Scalability::GIMethod::SoftwareTraced, Common::Scalability::GIMethod::HardwareTraced };
    inline constexpr std::array<Common::Scalability::GIMethod, 1> kIrradianceFieldGIMethods{
         Common::Scalability::GIMethod::ProbeVolume };

    // PURE. The GI methods a Trace program compiled for @p pipeline serves (the radiance cache's when
    // @p screenProbes is false, the screen probes' when true): the hardware permutation serves HardwareTraced
    // alone (PipelineFor picks it only there), the software one every traced method that walks the SDF.
    [[nodiscard]] constexpr std::span<const Common::Scalability::GIMethod>
    TraceProgramGIMethods( const Pipeline& pipeline, const bool screenProbes )
    {
        constexpr std::span<const Common::Scalability::GIMethod> cache( kCacheGIMethods );
        constexpr std::span<const Common::Scalability::GIMethod> probes( kScreenProbeGIMethods );
        if ( pipeline.Tracer == Tracer::HardwareRayQuery )
            return cache.subspan( 2, 1 );                                               // HardwareTraced
        return screenProbes ? probes.first( 1 ) /* SoftwareTraced */ : cache.first( 2 ) /* ProbeVolume, Software */
             ;
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
    // define. Every pass that traces acquires its program with this (ShaderService::AcquireVariant). COOKED
    // (ShaderVariant::ShippedByCook): each consumer lists its programs under kTracerPermutations
    // (TracerProgramVariants), the list the package cook compiles.
    [[nodiscard]] inline Core::ShaderVariant TracerVariant( const GlobalIllumination::Pipeline& pipeline )
    {
        return Core::ShaderVariant{ .Defines       = { std::string( TracerDefine( pipeline.Tracer ) ),
                                                       std::string( HitLightingDefine( pipeline.Hits ) ) },
                                    .ShippedByCook = true };
    }

    // EVERY TRACER PERMUTATION A Pipeline RESOLVES TO (PipelineFor): software + surface cache, hardware +
    // surface cache, hardware + in-place hit lighting (in-place hit lighting is hardware-only). The one list the
    // tracing consumers' cooked variants are enumerated from.
    inline constexpr std::array<GlobalIllumination::Pipeline, 3> kTracerPermutations{
         GlobalIllumination::Pipeline{ .Tracer = GlobalIllumination::Tracer::SoftwareDistanceField,
                                       .Hits   = GlobalIllumination::HitLighting::SurfaceCache },
         GlobalIllumination::Pipeline{ .Tracer = GlobalIllumination::Tracer::HardwareRayQuery,
                                       .Hits   = GlobalIllumination::HitLighting::SurfaceCache },
         GlobalIllumination::Pipeline{ .Tracer = GlobalIllumination::Tracer::HardwareRayQuery,
                                       .Hits   = GlobalIllumination::HitLighting::InPlace } };

    // @p program under TracerVariant of every kTracerPermutations entry - the cooked list of a consumer whose
    // permutation is the tracer's alone (WorldRadianceCache Trace, SurfaceCacheDirectLighting).
    [[nodiscard]] inline std::vector<Core::CookedProgramVariant> TracerProgramVariants( const std::string_view program )
    {
        std::vector<Core::CookedProgramVariant> variants;
        for ( const GlobalIllumination::Pipeline& pipeline : kTracerPermutations )
            variants.push_back( { std::string( program ), TracerVariant( pipeline ) } );
        return variants;
    }

    // THIS FRAME'S RESOURCES OF THE ACTIVE TRACER — what DeclareTracerReads binds, with the pipeline that chose
    // the tracer (so the permutation and the reads cannot disagree). The frame's owner (SceneRenderer) fills it
    // once; a consumer hands it to TracerVariant / TracerFrameMissing / DeclareTracerReads and reads no member
    // itself.
    //   SoftwareDistanceField  the four global distance field clipmaps (GLOBAL-SDF) and their placement, a
    //                          buffer of four vec4 (centre.xyz cm, voxel cm; FrameField::Levels) then the
    //                          near-field froxel grid's placement (TracerSoftwarePayload) — the walk's own
    //                          record, so the neutral GI frame (GIFramePayload) carries nothing of it — the
    //                          mesh fields with the grid's cells (the near field, UE r.Lumen.TraceMeshSDFs)
    //                          and the field's object grid, the rows a hit may belong to (GIHitInstanceRows);
    //   HardwareRayQuery       the view's scene TLAS (RayTracingSceneFrame::Tlas, RT-SCENE), its per-segment
    //                          geometry and material records (RayTracingSceneFrame::Metadata, RT-SHADOWS: the
    //                          hit's vertex streams and alpha test), and the surface cache's instance-table row
    //                          (contract B3) per TLAS instance custom index (one uint per GPU-scene row,
    //                          kNoInstance where the cache holds no such instance;
    //                          SceneRenderer::AddFrameInstanceSurfaceCacheRows). The ray-query walk's own record;
    //                          it reads no distance field data. It fills four of the tracer's six bindings
    //                          (GI_TRACER_BINDING + 0..3); + 4 and + 5 stay unbound.
    struct TracerFrame
    {
        GlobalIllumination::Pipeline   Pipeline{};
        std::array<RDG::TextureRef, 4> GlobalDistanceField{};
        RDG::BufferRef                 GlobalDistanceFieldLevels; // "GITracerSoftwareUB" (TracerSoftwarePayload)
        RDG::TextureRef                GlobalDistanceFieldObjectGrid; // "GlobalDistanceFieldObjectGridTexture"
        DistanceField::MeshDistanceFieldReads MeshDistanceFields;
        RDG::BufferRef                        Tlas;
        RayTracing::RayTracingMetadataFrame   Metadata;
        RDG::BufferRef                        InstanceSurfaceCacheRows; // "GIInstanceSurfaceCacheRows"
    };

    // "GITracerSoftwareUB" — std430, must match the block in Common/GITracer.glslh.
    struct TracerSoftwarePayload
    {
        std::array<glm::vec4, 4>            GlobalDistanceFieldLevels{};
        DistanceField::MeshSDFGridPlacement MeshSDFGrid; // MeshDistanceFieldReads::NearFieldGridPlacement
    };
    static_assert( sizeof( TracerSoftwarePayload ) == 176 );

    // The shader names of the hardware tracer's four reads (Common/GITracer.glslh declares them at
    // GI_TRACER_BINDING + 0..3; the record blocks are Common/RayQueryVisibility.glslh's, shared with RT-SHADOWS).
    inline constexpr std::string_view kHardwareTracerTlasName                     = "SceneTlas";
    inline constexpr std::string_view kHardwareTracerGeometryName                 = "RayTracingGeometryRecords";
    inline constexpr std::string_view kHardwareTracerMaterialsName                = "RayTracingMaterialRecords";
    inline constexpr std::string_view kHardwareTracerInstanceSurfaceCacheRowsName = "GIInstanceSurfaceCacheRows";
    // Their binding form (RDGShaderParameters.hpp), from the names above.
    inline constexpr RDG::AccelerationStructureParameter kHardwareTracerTlas{ kHardwareTracerTlasName };
    inline constexpr RDG::StorageBufferParameter         kHardwareTracerGeometry{ kHardwareTracerGeometryName };
    inline constexpr RDG::StorageBufferParameter         kHardwareTracerMaterials{ kHardwareTracerMaterialsName };
    inline constexpr RDG::StorageBufferParameter         kHardwareTracerInstanceSurfaceCacheRows{
         kHardwareTracerInstanceSurfaceCacheRowsName };

    // THE GI FRAME RECORD — what every GI pass knows of the frame, neutral to the tracer. std430 — must match
    // `struct GIFrame` in Common/GIFrame.glslh. 256 bytes. Uploaded once per frame by the frame's owner and bound
    // as "GIFrameUB" by the radiance cache's passes and the screen probe gather.
    struct GIFramePayload
    {
        glm::mat4 ViewProjection{ 1.0f };    // the jittered matrix the G-buffer was rasterised with
        glm::mat4 InvViewProjection{ 1.0f }; // its inverse
        glm::vec4 Camera{ 0.0f };            // xyz = camera world position (cm)
        glm::vec4 SunTravel{ 0.0f };         // xyz = direction the sunlight travels, w = 1 when there is a sun
        glm::vec4 SunRadiance{ 0.0f };       // rgb = colour * intensity, w = 1 when it casts shadows
        glm::vec4 Light1Travel{ 0.0f };   // atmosphere light 1 (the moon): xyz = travel, w = 1 when there is one
        glm::vec4 Light1Radiance{ 0.0f }; // rgb = colour * intensity, w = 1 when it casts shadows
        glm::vec4 SkyYawCosSin{ 1.0f, 0.0f, 0.0f, 0.0f };
        glm::vec4 SkyGain{ 1.0f };
        glm::vec4 Screen{ 0.0f }; // xy = G-buffer extent (pixels), zw = 1 / that
    };
    static_assert( sizeof( GIFramePayload ) == 256 );

    // Empty when @p frame holds every resource its tracer reads; otherwise what is missing (for the consumer's
    // one error line).
    [[nodiscard]] std::string_view TracerFrameMissing( const TracerFrame& frame );

    // Declares the tracer's reads on a consumer pass's binding block, under the shader names the tracer include
    // (Common/GITracer.glslh) declares. The consumer's program must be compiled with TracerVariant. @p sampled =
    // the stage the consumer samples in (a compute consumer, or a raster one: the surface cache's card lighting).
    void DeclareTracerReads( RDG::BindingBlockBuilder& block, const TracerFrame& frame,
                             RDG::Access sampled = RDG::Access::SampledCompute );

    // THIS FRAME'S SURFACE CACHE AS A HIT READS IT (GIHitRadiance, Common/SurfaceCacheSampling.glslh): the card //
    // records, the instance table of contract B3 (SurfaceCache.hpp; the rows of a hit come from the tracer,
    // GIHitInstanceRows), and the atlas layers a hit
    // samples - FinalLighting (what the hit returns: direct + the indirect of the radiosity slot + emissive),
    // Depth and Opacity (whether the card holds the hit). Filled by the frame's owner from the surface cache's
    // update (SurfaceCacheRenderer::HitReads) before any tracing consumer runs.
    struct SurfaceCacheFrame
    {
        RDG::BufferRef  Cards;        // "SurfaceCardBuffer"
        RDG::BufferRef  Pages;        // "SurfaceCardPageBuffer"
        RDG::BufferRef  MeshCards;    // "SurfaceMeshCardsBuffer"
        RDG::BufferRef  InstanceRows; // "SurfaceCacheInstancesUB"
        RDG::TextureRef FinalLighting;
        RDG::TextureRef Depth;
        RDG::TextureRef Opacity;
    };

    // Empty when @p frame holds every read; otherwise what is missing.
    [[nodiscard]] std::string_view SurfaceCacheFrameMissing( const SurfaceCacheFrame& frame );

    // Declares the surface cache's reads of GIHitRadiance on a tracing consumer's binding block, under the names
    // Common/SurfaceCacheSampling.glslh declares (at GI_SURFACE_CACHE_BINDING .. + 6); @p sampled is the atlas
    // layers' access (SampledGraphics for a raster consumer).
    void DeclareSurfaceCacheReads( RDG::BindingBlockBuilder& block, const SurfaceCacheFrame& frame,
                                   RDG::Access sampled = RDG::Access::SampledCompute );

    // THE DIRECTIONAL LIGHTS A CACHE'S HISTORY WAS LIT BY: the frame sun and atmosphere light 1 (the moon), each
    // as the lit passes receive it (SceneRenderer::GetFrameSun / GetFrameLight1: direction it travels, rgb colour
    // with the intensity in w). A world-space history (the radiance cache's probes, the surface cache's lit pages)
    // blended across a significant change of these keeps the old sun's light in whole cells for seconds: a
    // straight edged patch of another colour that fades slowly (GRASS-SETTLE). UE Lumen's answer is the full
    // update (r.Lumen.RadianceCache.ForceFullUpdate, the scene lighting's full relight): the history is dropped
    // and the first frame after the change is converged from the new light, never blended with the old one.
    struct DirectionalLighting
    {
        glm::vec4 SunDirection{ 0.0f };
        glm::vec4 SunColorIntensity{ 0.0f };
        glm::vec4 Light1Direction{ 0.0f };
        glm::vec4 Light1ColorIntensity{ 0.0f };
    };

    // What a "significant" change is, measured against the lighting the history was last invalidated for (so a
    // slow time-of-day drift invalidates once it has added up, not every frame): the direction turned by more than
    // one degree, or the radiance moved by more than 5% of the brighter of the two.
    inline constexpr float kLightingHistoryCosAngle      = 0.99984770f; // cos( 1 degree )
    inline constexpr float kLightingHistoryRadianceShare = 0.05f;

    // True when a history lit by @p was must be dropped for a frame lit by @p now.
    [[nodiscard]] inline bool InvalidatesLightingHistory( const DirectionalLighting& was,
                                                          const DirectionalLighting& now )
    {
        const auto changed = []( const glm::vec4& wasDirection, const glm::vec4& wasColor,
                                 const glm::vec4& nowDirection, const glm::vec4& nowColor )
        {
            const glm::vec3 a     = glm::vec3( wasColor ) * wasColor.w;
            const glm::vec3 b     = glm::vec3( nowColor ) * nowColor.w;
            const float     scale = std::max( { a.x, a.y, a.z, b.x, b.y, b.z } );
            if ( scale <= 0.0f )
                return false; // dark before and after: nothing it lit is in the history
            if ( glm::length( a - b ) > kLightingHistoryRadianceShare * scale )
                return true;
            const float wasLength = glm::length( glm::vec3( wasDirection ) );
            const float nowLength = glm::length( glm::vec3( nowDirection ) );
            if ( wasLength <= 0.0f || nowLength <= 0.0f )
                return wasLength != nowLength;
            return glm::dot( glm::vec3( wasDirection ) / wasLength, glm::vec3( nowDirection ) / nowLength ) <
                   kLightingHistoryCosAngle;
        };
        return changed( was.SunDirection, was.SunColorIntensity, now.SunDirection, now.SunColorIntensity ) ||
               changed( was.Light1Direction, was.Light1ColorIntensity, now.Light1Direction,
                        now.Light1ColorIntensity );
    }
} // namespace Desert::Graphic::GlobalIllumination
