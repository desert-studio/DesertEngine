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
    // A blade strip's records come in SETS, one per strip level (GPU-COST-20, UE's shadow LOD bias): record
    // k x levels + s draws bucket s with strip level max(s, k). Set 0 is the camera's (level s for bucket s); a
    // cascade draws the set its texel size asks for (MeshRenderer::BuildGpuInstancedShadowDraws), so a blade is
    // never finer in a shadow than in the view and never finer than the cascade can resolve.
    inline constexpr uint64_t        kGpuInstanceArgsHeaderBytes = 16;
    inline constexpr uint64_t        kGpuInstanceArgsRecordBytes = 20;
    [[nodiscard]] constexpr uint64_t GpuInstanceArgsOffset( uint32_t submesh )
    {
        return kGpuInstanceArgsHeaderBytes + kGpuInstanceArgsRecordBytes * submesh;
    }

    // What a cascade picks a grass blade strip's shadow record set and its caster density by (GPU-COST-20; UE
    // r.Shadow LOD by the shadow view's resolution, grass thinned by the same rule as the camera's). Written by the
    // producer per frame (GrassRenderer::PrepareVariety); Levels 0: not a blade, the cascades draw set 0.
    struct GpuInstanceBladeShadowLod
    {
        float    Height      = 0.0f; // the tallest blade, cm (the shape's Height x the placement's largest scale)
        uint32_t Segments    = 1;    // the strip's level-0 segments; each level halves them down to one
        uint32_t Levels      = 0;    // the strip's levels = its record sets
        // where the generator's (F / d)^2 thinning starts, cm through the lens: F = min(StartCullDistance S, the
        // screen-width floor's D) (GrassFieldShare.glslh GrassThinShare); 0: none
        float    StartCull   = 0.0f;
        float    PixelSpread = 0.0f; // the main view's pixel size per cm of distance (0: orthographic, no thinning)
    };
} // namespace Desert::Graphic
