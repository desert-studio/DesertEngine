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

    // The argument buffer: a 16-byte header (bucket count, total capacity, submesh count, last drawn count), one
    // VkDrawIndexedIndirectCommand (indexCount, instanceCount, firstIndex, vertexOffset, firstInstance) per
    // submesh of the mesh at the drawn level, then per BUCKET four words (count the generator appended, first
    // instance, capacity, 0). A bucket is a range of Instances: a mesh has one, drawn by every section; a grass
    // blade strip one per level, drawn by the submesh of that level (GRASS-BLADE). Programs/Grass/
    // GrassFinalize.shader writes each record's instanceCount and firstInstance from its bucket.
    inline constexpr uint64_t kGpuInstanceArgsHeaderBytes = 16;
    inline constexpr uint64_t kGpuInstanceArgsRecordBytes = 20;
    [[nodiscard]] constexpr uint64_t GpuInstanceArgsOffset( uint32_t submesh )
    {
        return kGpuInstanceArgsHeaderBytes + kGpuInstanceArgsRecordBytes * submesh;
    }
} // namespace Desert::Graphic
