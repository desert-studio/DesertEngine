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
    };

    // THE VIEW'S PERSISTENT ROW BUFFERS (UE FRayTracingScene's instance buffer and its scene metadata): kept
    // across frames, so a frame writes only the rows that changed since the last upload that executed
    // (AddSceneRowUpload, RayTracingSceneRenderer.hpp) and nothing on a still frame.
    enum class SceneRowBuffer : uint8_t
    {
        Instances = 0,      // the TLAS instance rows: read by the build by address (AccelStructBuildInput)
        PrimitiveInstances, // RayTracingScene.PrimitiveInstances (StorageRead)
        GeometryRecords,    // RayTracingScene.GeometryRecords (RayTracingSceneMetadata.hpp, StorageRead)
        MaterialRecords,    // RayTracingScene.MaterialRecords (StorageRead)
        Count
    };

    // One run of consecutive rows a scatter writes (RayTracingSceneTarget::RecordRowScatter): rows
    // [FirstRow, FirstRow + RowCount) of a row buffer, taken in order from the packed staging rows.
    struct SceneRowRun
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
        // Makes the persistent @p buffer hold at least @p bytes (never less than one row of any kind): true when
        // it had to make a new buffer, which holds nothing yet (every row goes up this frame). A buffer grows to
        // the next power of two and never shrinks while the target lives. Written by copies (CopyDst; the
        // Instances rows after the CPU ones also by the GPU-instance pass, StorageWrite), read by the build
        // (Instances, AccelStructBuildInput) and by ray-query passes (StorageRead).
        [[nodiscard]] virtual Common::ResultStr<bool> EnsureRows( SceneRowBuffer buffer, uint64_t bytes ) = 0;
        // @p buffer as a graph external that carries its own state across graphs; meaningful after EnsureRows.
        [[nodiscard]] virtual RDG::ExternalBuffer& GetRows( SceneRowBuffer buffer ) = 0;
        // Device bytes of @p buffer: its ledger row (StorageBuffer); 0 before its first EnsureRows.
        [[nodiscard]] virtual uint64_t GetRowMemoryBytes( SceneRowBuffer buffer ) const = 0;
        // Graphs that ENDED holding @p buffer (its external's RecordFinalState, called after EndGraph: an
        // abandoned graph never counts); monotonic for the target's life, kept across a recreated buffer. An
        // upload is done once this moved past its value when the upload was declared (SceneRowUploadState).
        [[nodiscard]] virtual uint64_t GetRowGraphsEnded( SceneRowBuffer buffer ) const = 0;
        // Records the copy of @p runs of @p rowBytes rows, packed one after another from the start of @p staging,
        // into their rows of @p rows, inside the scatter pass's exec (@p staging CopySrc, @p rows CopyDst).
        [[nodiscard]] virtual Common::BoolResultStr RecordRowScatter( const RDG::PassContext&      context,
                                                                      const RDG::BufferBinding&    staging,
                                                                      const RDG::BufferBinding&    rows,
                                                                      std::span<const SceneRowRun> runs,
                                                                      uint32_t                     rowBytes ) = 0;
        // Device bytes of the TLAS storage: its ledger row (TopLevelAccelerationStructure), the one statement of
        // the allocation (the storage's graph desc is the size the build asked for, not what the device holds).
        [[nodiscard]] virtual uint64_t GetMemoryBytes() const = 0;
        // Records the build of @p instanceCount rows of @p instances into the TLAS, inside the pass's exec; @p
        // update: the update Prepare answered (TlasBuildSizes::Update) instead of a build. The recorded build is
        // one a later Prepare may answer an update over only once its graph ended (the storage external's
        // RecordFinalState); after an abandoned graph the next Prepare answers a build.
        [[nodiscard]] virtual Common::BoolResultStr RecordBuild( const RDG::PassContext&   context,
                                                                 const RDG::BufferBinding& instances,
                                                                 const RDG::BufferBinding& scratch,
                                                                 uint32_t instanceCount, bool update ) = 0;
    };

    // The API's TLAS object for one view (VulkanRayTracingScene.cpp).
    [[nodiscard]] std::unique_ptr<RayTracingSceneTarget> CreateRayTracingSceneTarget();
} // namespace Desert::Graphic::RayTracing
