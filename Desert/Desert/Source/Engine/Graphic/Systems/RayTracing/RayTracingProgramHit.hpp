#pragma once

#include <Engine/Core/Formats/MaterialParamRow.hpp>
#include <Engine/Graphic/Systems/RayTracing/RayTracingSceneMetadata.hpp>

#include <optional>
#include <string>

namespace Desert::Graphic::RayTracing
{
    // A HIT SHADED BY A PROGRAM'S OWN HIT GROUP (UE: a scene proxy's own ray tracing material - a text render
    // component's font material, as the landscape's tiles have TerrainRayHit): for a mesh drawn with a program
    // that is no surface template (MeshRenderer::GenericMeshRenderData with a ShaderName), the hit group cell
    // "<Template>/RayTracing.Hit" its segments run, the row that cell reads through the hit record, and the alpha
    // test of its segments. The producer that draws the mesh fills it from the values its raster draw carries; the
    // gather hands it to every segment of the record (RayTracingSegmentRecord::ProgramHit).
    struct RayTracingProgramHit
    {
        // Empty: the record has no hit group of its own (its SlotMaterial's, else the default surface's).
        std::string                     Template;
        Core::Formats::MaterialParamRow Row;
        // Set: the segments are NOT opaque - the hit group's any hit runs on a candidate (the pipeline), and an
        // inline ray query tests this record (Common/RayQueryVisibility.glslh) - the same mask both ways.
        std::optional<RayTracingMaterialRecord> Opacity;

        bool operator==( const RayTracingProgramHit& ) const = default;
    };
} // namespace Desert::Graphic::RayTracing
