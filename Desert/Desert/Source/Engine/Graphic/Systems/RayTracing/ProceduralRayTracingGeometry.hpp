#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Engine/Graphic/Systems/RayTracing/RayTracingDynamicGeometry.hpp>

#include <cstdint>
#include <memory>

// THE DEVICE-FACING SIDE OF ONE PROCEDURAL BLAS (RenderCore): the interface the API implements
// (VulkanProceduralRayTracingGeometry) and the procedural cache (RayTracingProceduralGeometry.hpp, Renderer)
// drives. Kept apart from the cache so the API layer never includes the renderer.
namespace Desert::Graphic::RayTracing
{
    struct ProceduralBlasPrepared
    {
        uint64_t ScratchBytes = 0;     // alignment slack included
        bool     NeedsBuild   = false; // the storage holds no build of the current buffers yet
    };

    class ProceduralRayTracingGeometry : public RayTracingDynamicGeometryTarget
    {
    public:
        // Makes the BLAS fit @p input (DescribeStaticMeshBlas of the mesh's current buffers and the instance's
        // masks), recreating it when the vertex/index buffers or the segments changed. Errors: no
        // acceleration-structure device, the buffers are not resident, the input has no segment, an allocation
        // failed.
        [[nodiscard]] virtual Common::ResultStr<ProceduralBlasPrepared>
        Prepare( const RayTracingMeshInput& input ) = 0;
    };

    // The API's entry (VulkanProceduralRayTracingGeometry.cpp).
    [[nodiscard]] std::unique_ptr<ProceduralRayTracingGeometry> CreateProceduralRayTracingGeometry();
} // namespace Desert::Graphic::RayTracing
