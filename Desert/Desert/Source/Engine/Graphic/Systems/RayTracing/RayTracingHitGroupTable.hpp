#pragma once

#include <Engine/Graphic/Systems/RayTracing/RayTracingLayout.hpp>

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <vector>

// THE FRAME'S HIT GROUP TABLE: the shader binding table's hit region as data (UE: the FRayTracingMeshCommands of
// the frame's scene, one per segment, each binding its material's hit group and its loose parameters). Built from
// the ONE per-segment table (RT-UNIFY): hit record i is segment record i and carries geometry record i, so a TLAS
// instance's SBT offset (its primitive's FirstSegmentRecord) + a hit's geometry index names the same segment for
// the ray tracing pipeline as for the ray-query walk. Data only (RenderCore: Renderer::TraceRays takes it); the
// builder is RayTracingHitGroupBuild.hpp, the backend writes the records after the group handles.
namespace Desert::Graphic::RayTracing
{
    struct RayTracingHitRecord
    {
        uint32_t                HitGroup = 0; // index into RayTracingHitGroupTable::Templates (= the pipeline's)
        RayTracingHitRecordData Data;
    };

    // A template whose hit group is missing this frame, and how many segments fell back from it.
    struct RayTracingHitGroupFallback
    {
        std::string Template;
        uint32_t    Segments = 0;
    };

    struct RayTracingHitGroupTable
    {
        // Hit group i = the `RayTracing.Hit` cell of Templates[i]. Templates[0] is the default surface, always.
        std::vector<std::string>         Templates;
        std::vector<RayTracingHitRecord> Records; // one per segment record, same order
        std::vector<glm::vec4>           Rows;    // the records' material rows end to end (16-byte slots)
        // Segments with no surface template (a whole-entity shader override): shaded by the default surface.
        uint32_t SegmentsOnDefaultSurface = 0;
        // Segments whose template has no compiled hit group (its `RayTracing.Hit` cell was refused or failed to
        // compile): shaded by the default surface's group and row (UE: the fallback material of a mesh command
        // whose material has no ray tracing shaders), never a refused frame. TemplatesWithoutHitGroup names them,
        // each with its own count.
        uint32_t                                SegmentsOnFallbackHitGroup = 0;
        std::vector<RayTracingHitGroupFallback> TemplatesWithoutHitGroup;
    };

} // namespace Desert::Graphic::RayTracing
