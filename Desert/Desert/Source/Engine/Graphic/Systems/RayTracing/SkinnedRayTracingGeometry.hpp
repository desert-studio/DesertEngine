#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/Systems/RayTracing/RayTracingDynamicGeometry.hpp>

#include <cstdint>
#include <memory>

namespace Desert
{
    class SkinnedMesh;
}

// THE DEVICE-FACING SIDE OF ONE SKIN-CACHE ENTRY (RenderCore): the interface the API implements
// (VulkanSkinnedRayTracingGeometry) and the skin cache (RayTracingSkinCache.hpp, Renderer) drives. Kept apart from
// the cache so the API layer never includes the renderer.
namespace Desert::Graphic::RayTracing
{
    // What one primitive's build costs this frame: the scratch it needs (alignment slack included) and whether it
    // refits the BLAS already built (true) or builds it anew (false).
    struct SkinnedBlasPrepared
    {
        uint64_t ScratchBytes = 0;
        bool     Refit        = false;
    };

    // The API side of one skinned primitive's cache entry: its cached positions and its BLAS, alive across frames.
    class SkinnedRayTracingGeometry : public RayTracingDynamicGeometryTarget
    {
    public:
        // Makes the positions buffer and the BLAS fit @p mesh with @p hiddenSubmeshes left out, recreating both
        // when the mesh's vertex/index buffers, its vertex count or the mask changed. Errors: the device cannot
        // build acceleration structures, the mesh is not resident or has no visible triangle, an allocation
        // failed.
        [[nodiscard]] virtual Common::ResultStr<SkinnedBlasPrepared> Prepare( const SkinnedMesh& mesh,
                                                                              uint64_t hiddenSubmeshes ) = 0;
        // Graph external carrying its own state across graphs: the positions the skin pass writes (the BLAS
        // storage and its build are the dynamic-geometry target's; the build reads these positions).
        [[nodiscard]] virtual RDG::ExternalBuffer& GetPositions() = 0;
        // Bytes of the cached positions buffer (stats overlay).
        [[nodiscard]] virtual uint64_t GetPositionsBytes() const = 0;
    };

    // The API's skin-cache entry (VulkanSkinnedRayTracingGeometry.cpp).
    [[nodiscard]] std::unique_ptr<SkinnedRayTracingGeometry> CreateSkinnedRayTracingGeometry();
} // namespace Desert::Graphic::RayTracing
