#pragma once

#include <cstdint>

// HOW THE DEVICE ISSUES A BINNED PASS'S RECORDS, chosen once (GPU-DRIVEN C0; the contract is in
// IndirectDrawSubmission.hpp). Split from the recording (BINT-RC): the Vulkan backend in RenderCore reads the
// mode, and the recording of the binned passes lives with the mesh renderers in Renderer, above it.
namespace Desert::Graphic::GpuDriven
{
    enum class IndirectSubmission : uint8_t
    {
        DrawIndexedIndirectMulti,
        DrawIndexedIndirectPerRecord,
    };

    // The device facts the choice reads: VkPhysicalDeviceFeatures::multiDrawIndirect and
    // VkPhysicalDeviceLimits::maxDrawIndirectCount. Filled from Engine::GpuCapabilities by its owner (this
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
} // namespace Desert::Graphic::GpuDriven
