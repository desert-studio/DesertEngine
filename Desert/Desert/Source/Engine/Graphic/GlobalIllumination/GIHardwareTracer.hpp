#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Engine/Graphic/GlobalIllumination/GITracing.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/RDG/RDGResources.hpp>
#include <Engine/Graphic/Systems/RayTracing/RayTracingSceneMetadata.hpp>
#include <Engine/Graphic/Systems/RayTracing/RayTracingSceneRenderer.hpp>

#include <string_view>

// THE HARDWARE GI TRACER (GI-HWRT) — the C++ half of Tracer::HardwareRayQuery. Follows UE 5.8
// Lumen/LumenHardwareRayTracingCommon.ush (the inline TraceLumenHardwareRay path: a ray query over the scene TLAS
// whose hit is read from the surface cache, the hit normal from FRayTracingSceneMetadata). The GLSL half is
// Shaders/Common/GITraceRayQuery.glslh; it defines GITraceRay, GITraceVisibility and GIRayBias of the GI ray
// contract (Common/GITracing.glslh) for the DESERT_GI_TRACER_HARDWARE permutation.
//
// NO PASS OF ITS OWN. The hardware walk runs inline inside each GI consumer (radiance-cache trace, screen probes,
// surface-cache radiosity, card direct-light shadow), compiled with TracerDefine( Tracer::HardwareRayQuery ). What
// this file adds to such a pass is its READS: the view's TLAS (AccelStructRead, which orders the pass after
// RayTracing::kTlasPassName), the per-segment geometry and material records (RT-SHADOWS' RayTracingMetadataFrame)
// and the surface cache's instance-row table (keyed by the TLAS custom index = the ObjectMotions row). A consumer
// never calls DeclareHardwareTracerReads itself: it calls the tracer-neutral DeclareTracerReads( builder,
// TracerFrame ) (GITracing.hpp), which dispatches here when the pipeline's tracer is the hardware one.
//
// NEVER A SILENT SWITCH. ResolveRenderMethods lowers GIMethod::HardwareTraced to SoftwareTraced on a device
// without ray query (MethodLimit::Device), and RayTracingSceneRequired then builds the TLAS for every frame whose
// GI traces hardware rays. A hardware pipeline that finds no TLAS this frame is therefore a defect, and
// MakeHardwareTracerFrame reports it by name; it never hands the consumer the software walk instead.
namespace Desert::Graphic::GlobalIllumination
{
    // The shader names the hardware permutation reads (RDG resolves set and binding by name).
    inline constexpr std::string_view kHardwareTracerTlasBinding      = "u_SceneTLAS";
    inline constexpr std::string_view kHardwareTracerGeometryBinding  = "u_RayTracingGeometry";
    inline constexpr std::string_view kHardwareTracerMaterialsBinding = "u_RayTracingMaterials";
    inline constexpr std::string_view kHardwareTracerRowCardsBinding  = "u_GIRowToCardRange";

    // The graph resources a DESERT_GI_TRACER_HARDWARE permutation reads this frame. Refs are valid for one graph.
    struct HardwareTracerFrame
    {
        RDG::BufferRef Tlas; // RayTracingSceneFrame::Tlas            -> u_SceneTLAS           (AccelStructRead)
        RDG::BufferRef Geometry;  // RayTracingMetadataFrame::Geometry     -> u_RayTracingGeometry  (StorageRead)
        RDG::BufferRef Materials; // RayTracingMetadataFrame::Materials    -> u_RayTracingMaterials (StorageRead)
        RDG::BufferRef
             RowToCardRange; // the surface cache's instance-row table -> u_GIRowToCardRange   (StorageRead)
    };

    // @p pipeline: the frame's resolved GI pipeline (PipelineFor). @p rowToCardRange: GI-SURFACE-CACHE's table of
    // this frame, one entry per TLAS custom index. Errors, naming the GI method and the missing input, when the
    // pipeline's tracer is not the hardware one (a caller bug: the software permutation reads none of this), when
    // the TLAS was not built this frame, or when the metadata or the row table was not declared.
    [[nodiscard]] Common::ResultStr<HardwareTracerFrame>
    MakeHardwareTracerFrame( const Pipeline& pipeline, const RayTracing::RayTracingSceneFrame& scene,
                             const RayTracing::RayTracingMetadataFrame& metadata, RDG::BufferRef rowToCardRange );

    // The hardware half of DeclareTracerReads: declares the four reads on the pass of @p bindings and binds them
    // to the four names above. The bindless texture set the alpha test samples is bound with the pass's global
    // sets, not here.
    void DeclareHardwareTracerReads( RDG::BindingBlockBuilder& bindings, const HardwareTracerFrame& frame );
} // namespace Desert::Graphic::GlobalIllumination
