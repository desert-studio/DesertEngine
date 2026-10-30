#pragma once

#include "LODChainSettings.hpp"
#include "MeshSimplifier.hpp"
#include "MeshTypes.hpp"

#include <cstdint>
#include <vector>

namespace Desert::Geometry
{
    // THE CHAIN A SUBMESH WITHOUT BAKED LODS IS GENERATED AT, AT LOAD: a mesh no import states a chain for (a
    // primitive, a procedural mesh). An imported mesh's chain is its import's (MeshImportSettings::LodChain),
    // baked into Submesh::BakedLODs at cook, so the build and the pool reservation count those levels as cooked.
    [[nodiscard]] inline const LODChainSettings& UnbakedLODChain()
    {
        static const LODChainSettings chain{};
        return chain;
    }

    // The indices BuildLODIndexBuffer appends for a submesh of @p baseIndexCount indices with no baked LODs,
    // at @p chain's targets (LODChainTargets; a level stopping short of its target adds more, a level that
    // cannot improve adds none). Sizes the geometry pool before the chain is generated: @p chain is the one
    // the build is handed (UnbakedLODChain at load).
    [[nodiscard]] inline uint64_t AutoLODIndexTarget( const uint64_t          baseIndexCount,
                                                      const LODChainSettings& chain )
    {
        if ( baseIndexCount / 3 < 8 )
            return 0; // BuildLODIndexBuffer generates no chain below eight triangles
        uint64_t total = 0;
        for ( const uint32_t target : LODChainTargets( static_cast<uint32_t>( baseIndexCount / 3 ), chain ) )
            total += static_cast<uint64_t>( target ) * 3;
        return total;
    }

    // Simplifies ONE submesh into the LOD levels @p chain states (meshopt, LODChainTargets), returning the
    // simplified TRIANGLE sets (submesh-local indices, coarsest last; an empty level = "this target did not
    // improve, reuse the previous").
    // Used at COOK time to BAKE LODs into the asset, and by BuildLODIndexBuffer when a submesh has no baked
    // LODs. `positions` = the submesh's tightly-packed xyz (vertexCount*3); `localTris` = its base triangles
    // (indices relative to the submesh's VertexOffset); `attributes` = null or kSimplifyAttributeCount floats
    // per vertex (normal xyz, uv; see SimplifyLODAttributes) so the reduction is attribute-aware. Each level
    // is reduced to its target share of the BASE triangles with no error cap (UE percent-triangles reduction)
    // and logs target / result / error.
    std::vector<std::vector<Index>> SimplifyLODLevels( const float* positions, uint32_t vertexCount,
                                                       const std::vector<Index>& localTris,
                                                       const LODChainSettings&   chain,
                                                       const float*              attributes = nullptr );

    // Packs one vertex's simplifier attributes (normal xyz, uv) onto `out` - the one layout both the
    // load-time chain and the import-time bake hand to SimplifyLODLevels.
    void AppendSimplifyAttributes( const glm::vec3& normal, const glm::vec2& uv, std::vector<float>& out );

    // Builds the GPU index buffer for a mesh WITH a LOD chain. For each submesh: if Submesh::BakedLODs is
    // populated (cooked at import), those levels are appended as-is; otherwise they are GENERATED via
    // SimplifyLODLevels at @p chain (UnbakedLODChain at load; AutoLODIndexTarget sizes the pool by the same
    // chain). Records the per-submesh ranges in Submesh::LODs (LODs[0] = the original range). The CPU-side base
    // indices are NOT modified.
    std::vector<Index> BuildLODIndexBuffer( const std::vector<Vertex>& vertices,
                                            const std::vector<Index>&  baseIndices,
                                            std::vector<Submesh>&      submeshes,
                                            const LODChainSettings&    chain );
} // namespace Desert::Geometry
