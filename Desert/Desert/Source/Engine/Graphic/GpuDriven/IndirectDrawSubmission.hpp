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
// ONE fixed table of commands on every platform (GpuIndirectDraw[Bins], 32-byte stride, bins sorted by
// IndirectGroup), ONE record per bin, ONE cull on the GPU (decision 10-28, replacing 10-27's optional GPU draw
// count: Metal has no draw count read from a buffer, and a compacted table would be a second cull output format).
// The cull writes each bin's InstanceCount (0 = everything culled) and firstInstance into the bin's compacted
// run of VisibleInstances; the CPU knows each group's BinCount and issues exactly that many records. Nothing
// compacts bins, nothing counts draws on the GPU. What differs per device is only how the records are ISSUED,
// chosen ONCE by SelectIndirectSubmission (as UE's RHI does):
//  - DrawIndexedIndirectMulti (multiDrawIndirect = 1): vkCmdDrawIndexedIndirect( args, group.FirstBin * stride,
//    group.BinCount, stride ) per group, split at maxDrawIndirectCount.
//  - DrawIndexedIndirectPerRecord (multiDrawIndirect = 0): one vkCmdDrawIndexedIndirect of one record per bin
//    (the spec's drawCount <= 1 without multiDrawIndirect).
// Both need drawIndirectFirstInstance (firstInstance = the bin's FirstVisible) and shaderDrawParameters, present
// on every target device.
//
// Replaces the per-submesh CPU funnel RenderMesh -> WalkMeshSubmeshes -> DrawIndexedCounted
// (API/Vulkan/VulkanRenderer.cpp:794, 887-917, 1812-1825) and RenderMeshIndirect (:919-960) for the binned passes.
namespace Desert::Graphic::GpuDriven
{
    enum class IndirectSubmission : uint8_t
    {
        DrawIndexedIndirectMulti,
        DrawIndexedIndirectPerRecord,
    };

    // The device facts the choice reads: VkPhysicalDeviceFeatures::multiDrawIndirect and
    // VkPhysicalDeviceLimits::maxDrawIndirectCount. Filled from Engine::DeviceCapabilities by its owner (this
    // struct is how they reach the selection, never a second query of the device).
    struct IndirectDrawCapabilities
    {
        bool     MultiDrawIndirect    = false;
        uint32_t MaxDrawIndirectCount = 1;
    };

    // How every binned pass of the device reads its table: the mode and the most records one call may name.
    struct IndirectSubmissionMode
    {
        IndirectSubmission Submission      = IndirectSubmission::DrawIndexedIndirectPerRecord;
        uint32_t           MaxDrawsPerCall = 1;
    };

    // THE one decision point: multiDrawIndirect ? Multi : PerRecord.
    [[nodiscard]] IndirectSubmissionMode SelectIndirectSubmission( const IndirectDrawCapabilities& capabilities );

    // Records one pass's groups from a phase's CulledDraws inside the pass's exec: per group, bind its pipeline
    // and geometry buffers, then the draws as above. The pass declared IndirectDraws as Access::IndirectArgs and
    // VisibleInstances + the GpuScene / pass tables as storage reads. Refused for an undeclared buffer or a group
    // without pipeline or geometry.
    [[nodiscard]] Common::BoolResultStr RecordIndirectGroups( RendererAPI& api, const RDG::PassBindings& bindings,
                                                              const IndirectSubmissionMode& mode,
                                                              const PassDrawTable&          pass,
                                                              const CulledDraws&            draws );
} // namespace Desert::Graphic::GpuDriven
