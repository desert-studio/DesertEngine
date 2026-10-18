#pragma once

// GPU-GENERATED INSTANCES (GRASS-GPU): an instanced mesh whose transforms a compute pass writes this frame and
// whose instance count the GPU states in an indexed-indirect argument record per section. The producer (the
// landscape grass, GrassRenderer) owns the buffers and registers them in the frame graph; the mesh renderer draws
// them through the instanced cells exactly as an ISM, reading the count with vkCmdDrawIndexedIndirect.

#include <Engine/Graphic/RDG/RDGResources.hpp>

#include <cstdint>

namespace Desert::Graphic
{
    // The frame-graph names of one producer's two buffers, valid from the producer's compute node on (it is added
    // before the cascades and the geometry, so every draw node that reads them declares the reads after it).
    struct GpuInstanceRefs
    {
        RDG::BufferRef Instances; // mat4 per instance, read by the vertex stage as InstanceTransforms
        RDG::BufferRef Args;      // GpuInstanceArgs header + one indexed-indirect record per submesh
    };

    // The argument buffer: a 16-byte header (count the generator appended, capacity, submesh count, 0) and one
    // VkDrawIndexedIndirectCommand (indexCount, instanceCount, firstIndex, vertexOffset, firstInstance) per
    // submesh of the mesh at the drawn level. Programs/Grass/GrassFinalize.shader writes instanceCount.
    inline constexpr uint64_t kGpuInstanceArgsHeaderBytes = 16;
    inline constexpr uint64_t kGpuInstanceArgsRecordBytes = 20;
    [[nodiscard]] constexpr uint64_t GpuInstanceArgsOffset( uint32_t submesh )
    {
        return kGpuInstanceArgsHeaderBytes + kGpuInstanceArgsRecordBytes * submesh;
    }
} // namespace Desert::Graphic
