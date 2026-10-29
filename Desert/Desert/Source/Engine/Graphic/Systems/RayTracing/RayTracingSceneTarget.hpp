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

    // How this frame's TLAS is made, and the scratch it needs (vkGetAccelerationStructureBuildSizesKHR).
    struct TlasBuildSizes
    {
        uint64_t ScratchBytes = 0; // already includes the scratch-address alignment slack
        // true: the TLAS is UPDATED in place from its last build (VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR:
        // the rows' transforms, BLASes and masks may differ, their count may not); ScratchBytes is the update's.
        // false: built from nothing.
        bool Update = false;
    };

    // The API side of one view's TLAS: the acceleration structure and its storage, alive across frames.
    class RayTracingSceneTarget
    {
    public:
        virtual ~RayTracingSceneTarget() = default;

        // Makes the TLAS able to hold @p instanceCount instances (recreating it when it cannot) and answers how
        // this frame makes it: an UPDATE when @p update asks for one (the caller's rows are its last build's rows,
        // moved: RayTracingSceneRenderer) and the TLAS can take it - the storage was not recreated and its last
        // recorded build had exactly @p instanceCount instances -, a build otherwise. Errors: the device cannot
        // build acceleration structures, or the storage could not be created.
        [[nodiscard]] virtual Common::ResultStr<TlasBuildSizes> Prepare( uint32_t instanceCount, bool update ) = 0;
        // The TLAS storage as a graph external; it carries its own state across graphs.
        [[nodiscard]] virtual RDG::ExternalBuffer& GetStorage() = 0;
        // Device bytes of the TLAS storage: its ledger row (TopLevelAccelerationStructure), the one statement of
        // the allocation (the storage's graph desc is the size the build asked for, not what the device holds).
        [[nodiscard]] virtual uint64_t GetMemoryBytes() const = 0;
        // Records the build of @p instanceCount rows of @p instances into the TLAS, inside the pass's exec; @p
        // update: the update Prepare answered (TlasBuildSizes::Update) instead of a build.
        [[nodiscard]] virtual Common::BoolResultStr RecordBuild( const RDG::PassContext&   context,
                                                                 const RDG::BufferBinding& instances,
                                                                 const RDG::BufferBinding& scratch,
                                                                 uint32_t instanceCount, bool update ) = 0;
    };

    // The API's TLAS object for one view (VulkanRayTracingScene.cpp).
    [[nodiscard]] std::unique_ptr<RayTracingSceneTarget> CreateRayTracingSceneTarget();
} // namespace Desert::Graphic::RayTracing
