#pragma once

#include <cstdint>
#include <string>

namespace Desert::Graphic
{
    // THE MESH DRAW SPLIT's label of one GPU-driven bin (Common::Profiling::GpuDrawSplitScope): the bin's
    // submesh and LOD, the pipeline it draws with, and the triangles of one of its instances - the culled
    // instance count is the GPU's, never read back. Built with the bin (GpuDriven::DrawCommandCache, on a
    // scene change), read only while Profiler::MeshDrawSplit is on.
    struct DrawSplitLabel
    {
        std::string Mesh;
        std::string Material;
        uint32_t    TrianglesPerInstance = 0;
    };
} // namespace Desert::Graphic
