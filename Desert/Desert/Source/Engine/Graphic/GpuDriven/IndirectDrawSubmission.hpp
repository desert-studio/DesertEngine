#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Engine/Graphic/GpuDriven/DrawCommandCache.hpp>
#include <Engine/Graphic/GpuDriven/InstanceCulling.hpp>

#include <cstdint>

namespace Desert::Engine
{
    struct DeviceCapabilities;
}

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
// ONE table of commands on every platform (GpuIndirectDraw[Bins], 32-byte stride, bins sorted by IndirectGroup).
// Two ways to read it, chosen ONCE per device from DeviceCapabilities (Engine/Core/Device.hpp) — no second path:
//  - DrawIndexedIndirectCount (drawIndirectCount = 1, desktop Vulkan 1.2): "GPUDriven: Compact" moves each
//    group's non-empty bins to the front of the group's range and writes GroupDrawCounts[group];
//    vkCmdDrawIndexedIndirectCount( args, group.FirstBin * stride, counts, group * 4, group.BinCount, stride ).
//  - DrawIndexedIndirectFixed (drawIndirectCount = 0, multiDrawIndirect = 1: Mac M1 / MoltenVK 1.4.2): the same
//    table uncompacted, an empty bin keeps InstanceCount = 0;
//    vkCmdDrawIndexedIndirect( args, group.FirstBin * stride, group.BinCount, stride ).
// Both need drawIndirectFirstInstance (firstInstance = the bin's FirstVisible) and shaderDrawParameters, present
// on every target device. No device is refused for missing drawIndirectCount / multiDrawIndirect (decision 10-27):
// SelectIndirectSubmission picks the mode from DeviceCapabilities (phase CULL adds the two capability bits).
//
// Replaces the per-submesh CPU funnel RenderMesh -> WalkMeshSubmeshes -> DrawIndexedCounted
// (API/Vulkan/VulkanRenderer.cpp:794, 887-917, 1812-1825) and RenderMeshIndirect (:919-960) for the binned passes.
namespace Desert::Graphic::GpuDriven
{
    enum class IndirectSubmission : uint8_t
    {
        DrawIndexedIndirectCount,
        DrawIndexedIndirectFixed,
    };

    // THE one decision point: DeviceCapabilities::SupportsDrawIndirectCount ? Count : Fixed.
    [[nodiscard]] IndirectSubmission SelectIndirectSubmission( const Engine::DeviceCapabilities& capabilities );

    // Records one pass's groups from a phase's CulledDraws inside the pass's exec: per group, bind its pipeline
    // and geometry buffers, then one multi-draw as above. The pass declared IndirectDraws / GroupDrawCounts as
    // Access::IndirectArgs and VisibleInstances + the GpuScene / pass tables as storage reads. Refused for an
    // undeclared buffer or a group whose pipeline the PipelineCache no longer holds.
    [[nodiscard]] Common::BoolResultStr RecordIndirectGroups( RendererAPI& api, const RDG::PassBindings& bindings,
                                                              IndirectSubmission   submission,
                                                              const PassDrawTable& pass,
                                                              const CulledDraws&   draws );
} // namespace Desert::Graphic::GpuDriven
