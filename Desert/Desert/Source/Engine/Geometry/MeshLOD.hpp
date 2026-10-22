#pragma once

#include "MeshSimplifier.hpp"
#include "MeshTypes.hpp"

#include <cstdint>
#include <vector>

namespace Desert::Geometry
{
    // Simplifies ONE submesh into its LOD levels (meshopt), returning the simplified TRIANGLE sets
    // (submesh-local indices, coarsest last; an empty level = "this ratio didn't improve, reuse the previous").
    // Used at COOK time to BAKE LODs into the asset, and by BuildLODIndexBuffer when a submesh has no baked
    // LODs. `positions` = the submesh's tightly-packed xyz (vertexCount*3); `localTris` = its base triangles
    // (indices relative to the submesh's VertexOffset); `attributes` = null or kSimplifyAttributeCount floats
    // per vertex (normal xyz, uv; see SimplifyLODAttributes) so the reduction is attribute-aware. Each level
    // is reduced to its ratio of the BASE triangles with no error cap (UE percent-triangles reduction) and
    // logs ratio / target / result / error.
    std::vector<std::vector<Index>> SimplifyLODLevels( const float* positions, uint32_t vertexCount,
                                                       const std::vector<Index>& localTris,
                                                       const float*              attributes = nullptr );

    // Packs one vertex's simplifier attributes (normal xyz, uv) onto `out` - the one layout both the
    // load-time chain and the import-time bake hand to SimplifyLODLevels.
    void AppendSimplifyAttributes( const glm::vec3& normal, const glm::vec2& uv, std::vector<float>& out );

    // Builds the GPU index buffer for a mesh WITH a LOD chain. For each submesh: if Submesh::BakedLODs is
    // populated (cooked at import), those levels are appended as-is; otherwise they are GENERATED via
    // SimplifyLODLevels (the pre-baking behaviour). Records the per-submesh ranges in Submesh::LODs
    // (LODs[0] = the original range). The CPU-side base indices are NOT modified.
    std::vector<Index> BuildLODIndexBuffer( const std::vector<Vertex>& vertices,
                                            const std::vector<Index>&  baseIndices,
                                            std::vector<Submesh>&      submeshes );
} // namespace Desert::Geometry
