#pragma once

#include "LODChainSettings.hpp"
#include "MeshSimplifier.hpp"
#include "MeshTypes.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace Desert::Geometry
{
    // THE CHAIN A SUBMESH NO COOK MADE IS GENERATED AT, AT LOAD: a mesh no import states a chain for (a
    // primitive, a procedural mesh; Submesh::LodChainCooked false). An imported mesh's chain is its import's
    // (MeshImportSettings::LodChain), baked into Submesh::BakedLODs at cook - none for LodPolicy None - so the
    // build and the pool reservation count exactly those levels.
    [[nodiscard]] inline const LODChainSettings& UnbakedLODChain()
    {
        static const LODChainSettings chain{};
        return chain;
    }

    // The indices BuildLODIndexBuffer appends for a submesh of @p baseIndexCount indices with no cooked chain,
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

    // THE PARTS A LEVEL THINS INSTEAD OF SIMPLIFYING (UE Nanite "Preserve Area"): a connected part (over
    // position-welded topology) whose box half-diagonal is at most this share of the submesh's extent is an
    // ELEMENT - a leaf card, a grass blade card - when the submesh has at least kLODMinElements of them. An
    // element is too small for a collapse to keep it, so a level keeps a subset of them, never losing more of
    // their area than the level's error budget allows: a crown thins card by card instead of losing every card at
    // once. A level adds no vertex (the kept cards keep their own corners). Anything larger (trunk, branch, rail,
    // post) is reduced by error.
    inline constexpr float       kLODElementExtentShare = 0.05f;
    inline constexpr std::size_t kLODMinElements        = 16;

    // THE ERROR A GENERATED LEVEL MAY SHOW, in reference pixels (kAutoLODReferenceScreenRows) at the screen size
    // the automatic progression starts it at (UE: a reduction's MaxDeviation is turned into the LOD's screen size
    // at 1 pixel of error). A part thinner than that there may go, one wider stays: a fence keeps its rails.
    inline constexpr float kLODMaxPixelError = 1.0f;

    // Simplifies ONE submesh into the LOD levels @p chain states (LODChainTargets), returning the simplified
    // TRIANGLE sets (submesh-local indices, coarsest last; an empty level = "this target did not improve, reuse
    // the previous"). Used at COOK time to BAKE LODs into the asset, and by BuildLODIndexBuffer when a submesh
    // has no cooked chain. `positions` = the submesh's tightly-packed xyz (vertexCount*3); `localTris` = its base
    // triangles (indices relative to the submesh's VertexOffset); `attributes` = null or kSimplifyAttributeCount
    // floats per vertex (normal xyz, uv) so the reduction is attribute-aware.
    // Each level aims at its target share of the BASE triangles: the solid parts are reduced by meshopt with the
    // level's error budget (kLODMaxPixelError - a level stops short of its target rather than exceed it), the
    // elements (kLODElementExtentShare) are thinned to the same share, never below the area the budget may lose.
    // Every index names one of the submesh's own vertexCount vertices (a level adds none), so the cook and the
    // load-time chain over a fixed vertex buffer build the same levels. Logs target / result / error / elements.
    std::vector<std::vector<Index>> SimplifyLODLevels( const float* positions, uint32_t vertexCount,
                                                       const std::vector<Index>& localTris,
                                                       const LODChainSettings&   chain,
                                                       const float*              attributes = nullptr );

    // Packs one vertex's simplifier attributes (normal xyz, uv) onto `out` - the one layout both the
    // load-time chain and the import-time bake hand to SimplifyLODLevels.
    void AppendSimplifyAttributes( const glm::vec3& normal, const glm::vec2& uv, std::vector<float>& out );

    // Builds the GPU index buffer for a mesh WITH a LOD chain. For each submesh: if Submesh::LodChainCooked, its
    // BakedLODs (cooked at import, possibly none) are appended as-is; otherwise they are GENERATED via
    // SimplifyLODLevels at @p chain (UnbakedLODChain at load; AutoLODIndexTarget sizes the pool by the same
    // chain). Records the per-submesh ranges in Submesh::LODs (LODs[0] = the original range). The CPU-side base
    // indices are NOT modified.
    std::vector<Index> BuildLODIndexBuffer( const std::vector<Vertex>& vertices,
                                            const std::vector<Index>&  baseIndices,
                                            std::vector<Submesh>&      submeshes,
                                            const LODChainSettings&    chain );
} // namespace Desert::Geometry
