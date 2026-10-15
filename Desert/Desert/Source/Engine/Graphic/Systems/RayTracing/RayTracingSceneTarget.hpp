#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>

#include <cstdint>
#include <memory>

// THE DEVICE-FACING SIDE OF ONE VIEW'S TLAS (RenderCore): the interface the API implements (VulkanRayTracingScene)
// and the scene renderer (RayTracingSceneRenderer.hpp, Renderer) drives, plus the instance-row layout both encode.
// Kept apart from the renderer so the API layer never includes it.
namespace Desert::Graphic::RayTracing
{
    inline constexpr uint64_t kInstanceRowBytes       = 64; // VkAccelerationStructureInstanceKHR
    inline constexpr uint64_t kInstanceTransformBytes = 48;

    // What the API needs to build a TLAS of a given instance count (vkGetAccelerationStructureBuildSizesKHR).
    struct TlasBuildSizes
    {
        uint64_t ScratchBytes = 0; // already includes the scratch-address alignment slack
    };

    // The API side of one view's TLAS: the acceleration structure and its storage, alive across frames.
    class RayTracingSceneTarget
    {
    public:
        virtual ~RayTracingSceneTarget() = default;

        // Makes the TLAS able to hold @p instanceCount instances (recreating it when it cannot) and answers the
        // scratch this frame's build needs. Errors: the device cannot build acceleration structures, or the
        // storage could not be created.
        [[nodiscard]] virtual Common::ResultStr<TlasBuildSizes> Prepare( uint32_t instanceCount ) = 0;
        // The TLAS storage as a graph external; it carries its own state across graphs.
        [[nodiscard]] virtual RDG::ExternalBuffer& GetStorage() = 0;
        // Records the build of @p instanceCount rows of @p instances into the TLAS, inside the pass's exec.
        [[nodiscard]] virtual Common::BoolResultStr RecordBuild( const RDG::PassContext&   context,
                                                                 const RDG::BufferBinding& instances,
                                                                 const RDG::BufferBinding& scratch,
                                                                 uint32_t                  instanceCount ) = 0;
    };

    // The API's TLAS object for one view (VulkanRayTracingScene.cpp).
    [[nodiscard]] std::unique_ptr<RayTracingSceneTarget> CreateRayTracingSceneTarget();
} // namespace Desert::Graphic::RayTracing
