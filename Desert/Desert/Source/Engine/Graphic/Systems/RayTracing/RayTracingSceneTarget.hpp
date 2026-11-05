#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>

#include <cstdint>
#include <memory>
#include <span>

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
        // true: the persistent instance rows (RayTracingSceneTarget::GetInstances) were (re)created by this
        // Prepare and hold nothing yet: every CPU row goes up this frame, not only the changed ones.
        bool InstancesRecreated = false;
    };

    // One run of consecutive instance rows a scatter writes (RayTracingSceneTarget::RecordInstanceScatter): rows
    // [FirstRow, FirstRow + RowCount) of the persistent rows, taken in order from the packed staging rows.
    struct InstanceRowRun
    {
        uint32_t FirstRow = 0;
        uint32_t RowCount = 0;
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
        // THE PERSISTENT INSTANCE ROWS (UE FRayTracingScene's instance buffer): the rows the TLAS is built from,
        // kept across frames, so a frame writes only the rows that changed (AddRayTracingScenePass). Prepare makes
        // them hold @p instanceCount rows (at least one; TlasBuildSizes::InstancesRecreated when it had to make
        // a new buffer). Written by copies (CopyDst) and by the GPU-instance pass (StorageWrite), read by the
        // build (AccelStructBuildInput); a graph external that carries its own state across graphs.
        [[nodiscard]] virtual RDG::ExternalBuffer& GetInstances() = 0;
        // Device bytes of the instance rows: their ledger row (StorageBuffer).
        [[nodiscard]] virtual uint64_t GetInstanceMemoryBytes() const = 0;
        // Records the copy of @p runs, packed one after another from the start of @p staging, into their rows of
        // @p instances, inside the scatter pass's exec (@p staging CopySrc, @p instances CopyDst).
        [[nodiscard]] virtual Common::BoolResultStr
        RecordInstanceScatter( const RDG::PassContext& context, const RDG::BufferBinding& staging,
                               const RDG::BufferBinding& instances, std::span<const InstanceRowRun> runs ) = 0;
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
