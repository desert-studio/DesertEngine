#pragma once

#include <Engine/Graphic/Systems/RayTracing/RayTracingGeometryVariants.hpp>
#include <Engine/Graphic/Systems/RayTracing/RayTracingScene.hpp>
#include <Engine/Graphic/Systems/Scene/Mesh/MeshRenderer.hpp>

#include <glm/glm.hpp>

#include <array>
#include <span>
#include <string>
#include <vector>

// THE GATHER (UE FRayTracingScene gathering every primitive's static / dynamic ray-tracing instances): everything
// the raster queued this frame becomes TLAS primitives, INDEPENDENT OF THE VIEW FRUSTUM — a shadow or a reflection
// ray hits what the camera does not see. The only culls are the raster's own distance limits (an ISM's
// CullDistance, scaled by the lens like the raster's), never a frustum.
//   * static + generic records: ONE primitive per GPU-scene row; the records sharing a row (the lit draw and the
//     per-slot material draws of one mesh) OR their visible submeshes, so a mesh split over material slots traces
//     whole. A row hiding some submeshes traces its (mesh, mask) variant BLAS; hiding all is NothingVisible.
//   * ISM / foliage: one primitive per instance, its own row (InstancedMeshRenderData::FirstMotionRow), sharing
//     the mesh's BLAS. Wind / WPO is not evaluated: the instance transform as authored (UE default).
//   * GPU-instanced (the landscape grass): one primitive per producer (its sections merged) holding the mesh's
//     BLAS and segment run; the TLAS rows are written on the GPU from the transforms and the drawn count the
//     raster's indirect draw reads (RayTracingGpuInstanceSource). One shared BLAS per variety at the drawn level,
//     no per-instance deformation (UE default: ray tracing ignores WPO). A blade strip has no BLAS yet.
//   * skinned: one primitive per record, its BLAS the per-frame dynamic one (SelectDynamicGeometry).
namespace Desert::Graphic::RayTracing
{
    struct RayTracingGatherInputs
    {
        std::span<const System::MeshRenderer::StaticMeshRenderData>    Static;
        std::span<const System::MeshRenderer::GenericMeshRenderData>   Generic; // only VisibleInRayTracing enter
        std::span<const System::MeshRenderer::InstancedMeshRenderData> Instanced;
        std::span<const uint32_t>                                      InstancedMotionRows;
        std::span<const System::MeshRenderer::SkinnedMeshRenderData>   Skinned;
        std::span<const System::MeshRenderer::GpuInstancedRenderData>  GpuInstanced; // the landscape grass
        glm::vec3 ViewOrigin{};             // cm; for the ISM distance cull only
        float     DrawDistanceScale = 1.0f; // Graphic::DrawDistanceScale of the view's lens
    };

    // RayTracingGatherResult: RayTracingScene.hpp (held by RayTracingSceneRenderer without this header).
    void GatherRayTracingPrimitives( const RayTracingGatherInputs& inputs, RayTracingGeometryVariants& variants,
                                     RayTracingGatherResult& out );
} // namespace Desert::Graphic::RayTracing
