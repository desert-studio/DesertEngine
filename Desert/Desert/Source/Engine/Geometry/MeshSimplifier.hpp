#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Desert::Geometry
{
    // Thin wrapper over meshoptimizer for LOD generation. Simplifies an index buffer to ~targetRatio of
    // its triangles while preserving the mesh silhouette (meshopt_simplify). The LOD reuses the SAME
    // vertex buffer — only extra indices are produced — so a full LOD chain costs little memory.
    struct SimplifyResult
    {
        std::vector<uint32_t> Indices;
        float                 Error = 0.0f; // resulting geometric error, relative to the mesh extent
        // Why the reduction stopped above its target (empty when it reached it): the caller logs it - a
        // short LOD is reported, never silently patched with a lower-quality reducer.
        std::string StopReason;
    };

    // Per-vertex attributes the reduction weighs next to position (UE's QEM attribute-aware metric):
    // normal xyz + UV0 uv, tightly packed. Seams (same position, different wedge) are NOT barriers: the
    // reduction runs over POSITION-WELDED topology (UE's QEM collapses positions; wedges ride along), then
    // every surviving corner is given back the original wedge of its triangle's own chart, so a hard
    // normal or UV split stays split and the welded geometry has no cracks.
    constexpr std::size_t kSimplifyAttributeCount = 5;

    // positions: tightly-packed xyz per vertex (vertexCount * 3 floats). attributes: null, or
    // kSimplifyAttributeCount floats per vertex (normal xyz, uv). targetRatio in (0,1]; targetError is the
    // max allowed relative error (the simplifier stops early if it would exceed it); a ratio LOD passes
    // kNoErrorLimit, as UE reduces to a percent of triangles with no deviation cap. Returns the original
    // indices unchanged for degenerate input.
    constexpr float kNoErrorLimit = 3.0e38f;
    SimplifyResult  SimplifyMesh( const float* positions, std::size_t vertexCount,
                                  const std::vector<uint32_t>& indices, float targetRatio,
                                  float targetError = 0.05f, const float* attributes = nullptr );
} // namespace Desert::Geometry
