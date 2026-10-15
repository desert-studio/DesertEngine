#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Engine/Graphic/Systems/RayTracing/RayTracingDynamicGeometry.hpp>

#include <cstdint>
#include <memory>

namespace Desert
{
    class Mesh;
}

// THE DEVICE-FACING SIDE OF ONE PROCEDURAL BLAS (RenderCore): the interface the API implements
// (VulkanProceduralRayTracingGeometry) and the procedural cache (RayTracingProceduralGeometry.hpp, Renderer) drives.
// Kept apart from the cache so the API layer never includes the renderer.
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
        // Makes the BLAS fit @p mesh's current buffers with @p hiddenSubmeshes left out, recreating it when the
        // vertex/index buffers or the mask changed. Errors: no acceleration-structure device, the mesh is not
        // resident or has no visible triangle, an allocation failed.
        [[nodiscard]] virtual Common::ResultStr<ProceduralBlasPrepared> Prepare( const Mesh& mesh,
                                                                                 uint64_t    hiddenSubmeshes ) = 0;
    };

    // The API's entry (VulkanProceduralRayTracingGeometry.cpp).
    [[nodiscard]] std::unique_ptr<ProceduralRayTracingGeometry> CreateProceduralRayTracingGeometry();
} // namespace Desert::Graphic::RayTracing
