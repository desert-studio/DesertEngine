#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Engine/Graphic/GpuDriven/DrawCommandCache.hpp>
#include <Engine/Graphic/GpuDriven/InstanceCulling.hpp>

#include <cstdint>

namespace Desert::Graphic
{
    class RendererAPI;
}

namespace Desert::Graphic::RDG
{
    class PassBindings;
}

// GPU-DRIVEN contract (C0) — how a pass ISSUES its culled bins (UE FInstanceCullingDrawParams +
// SubmitDrawCommands / SubmitGPUInstancedMeshDrawCommandsRange).
//
// ONE table of commands on every platform (GpuIndirectDraw[Bins], 32-byte stride, bins sorted by IndirectGroup),
// ONE cull on the GPU (decision 10-27: no device is refused, no CPU draw path is kept, as in UE). What differs is
// only how the table is READ, chosen ONCE per device by SelectIndirectSubmission — no second path:
//  - DrawIndexedIndirectCount (drawIndirectCount = 1, desktop Vulkan 1.2): "GPUDriven: Compact" moves each
//    group's non-empty bins to the front of the group's range and writes GroupDrawCounts[group];
//    vkCmdDrawIndexedIndirectCount( args, group.FirstBin * stride, counts, group * 4, group.BinCount, stride ).
//  - DrawIndexedIndirectFixed (drawIndirectCount = 0, multiDrawIndirect = 1: Mac M1 Pro / MoltenVK 1.4.3): the
//    same table uncompacted, a bin the cull left empty keeps InstanceCount = 0;
//    vkCmdDrawIndexedIndirect( args, group.FirstBin * stride, group.BinCount, stride ), split at
//    maxDrawIndirectCount.
//  - DrawIndexedIndirectPerRecord (multiDrawIndirect = 0): the fixed table, one vkCmdDrawIndexedIndirect of one
//    record per bin (the spec's drawCount <= 1 without multiDrawIndirect).
// Every mode needs drawIndirectFirstInstance (firstInstance = the bin's FirstVisible) and shaderDrawParameters,
// present on every target device.
//
// Replaces the per-submesh CPU funnel RenderMesh -> WalkMeshSubmeshes -> DrawIndexedCounted
// (API/Vulkan/VulkanRenderer.cpp:794, 887-917, 1812-1825) and RenderMeshIndirect (:919-960) for the binned passes.
namespace Desert::Graphic::GpuDriven
{
    enum class IndirectSubmission : uint8_t
    {
        DrawIndexedIndirectCount,
        DrawIndexedIndirectFixed,
        DrawIndexedIndirectPerRecord,
    };

    // The device facts the choice reads: VkPhysicalDeviceVulkan12Features::drawIndirectCount,
    // VkPhysicalDeviceFeatures::multiDrawIndirect and VkPhysicalDeviceLimits::maxDrawIndirectCount. Filled from
    // Engine::DeviceCapabilities by its owner (phase CULL adds the two bits there; this struct is how they reach
    // the selection, never a second query of the device).
    struct IndirectDrawCapabilities
    {
        bool     DrawIndirectCount    = false;
        bool     MultiDrawIndirect    = false;
        uint32_t MaxDrawIndirectCount = 1;
    };

    // How every binned pass of the device reads its table: the mode and the most records one call may name.
    struct IndirectSubmissionMode
    {
        IndirectSubmission Submission      = IndirectSubmission::DrawIndexedIndirectPerRecord;
        uint32_t           MaxDrawsPerCall = 1;
    };

    // THE one decision point: drawIndirectCount ? Count : multiDrawIndirect ? Fixed : PerRecord.
    [[nodiscard]] IndirectSubmissionMode SelectIndirectSubmission( const IndirectDrawCapabilities& capabilities );

    // Records one pass's groups from a phase's CulledDraws inside the pass's exec: per group, bind its pipeline
    // and geometry buffers, then the draws as above. The pass declared IndirectDraws (and GroupDrawCounts on the
    // count path) as Access::IndirectArgs and VisibleInstances + the GpuScene / pass tables as storage reads.
    // Refused for an undeclared buffer, GroupDrawCounts missing on the count path or present on the others, or a
    // group without pipeline or geometry.
    [[nodiscard]] Common::BoolResultStr RecordIndirectGroups( RendererAPI& api, const RDG::PassBindings& bindings,
                                                              const IndirectSubmissionMode& mode,
                                                              const PassDrawTable&          pass,
                                                              const CulledDraws&            draws );
} // namespace Desert::Graphic::GpuDriven
