#pragma once

// MESH DISPLACEMENT: THE CPU HALF (contract CONTRACT-DISPLACE §1b, decisions §9.1/§9.4/§9.6).
//
// Displacement is geometry, not a raster stage: a displaced primitive's vertex and index buffers are written once
// per change of its tessellation levels and read by every raster view and by its BLAS. Everything that decides HOW
// MANY vertices and triangles there are lives here, on the CPU, so the counts are exact by construction - the GPU
// only evaluates positions, attributes and heights for the records this file produces (DisplacementEmit.shader).
//
// Pieces:
//   * TessellationTable - the 816 patterns of UE Nanite's tessellation table (every edge-factor triple 1..16),
//     ported from the generator in UE (TessellationTable.cpp), built once per process.
//   * DisplacementTopology - per mesh, built from LOD0 at cook: positions welded by value, welded edges and their
//     incident triangles, the displacement direction per welded vertex, and CLUSTERS of at most
//     kDisplacementClusterTriangles triangles (Nanite's cluster size) - a cluster is the unit that carries one
//     tessellation level.
//   * TessellateCluster - one cluster at a level vector, with UE's recursive patch split (NaniteSplit.usf) ported
//     to the CPU: a patch whose edge factor exceeds 16 is split by the table and each child re-measured.
//
// Watertightness (§9.6): a point that lies on a welded edge or vertex is described CANONICALLY - by welded ids in
// ascending order and a fixed-point parameter measured from the lower id - so every triangle, every cluster and
// every submesh that touches it produces the identical record, the GPU evaluates it with one instruction sequence,
// and its height is the average over the samples of EVERY triangle corner that touches it (a UV seam or a
// material boundary therefore moves both sides by the same amount).

#include <Common/Core/ResultStr.hpp>
#include <Engine/Geometry/MeshTypes.hpp>

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace Desert::Geometry
{
    // UE NANITE_TESSELLATION_TABLE_SIZE: the largest edge factor one pattern covers; larger edges split first.
    inline constexpr uint32_t kMaxEdgeFactor = 16;
    // UE NANITE_TESSELLATION_TABLE_PO2_SIZE: the stride of the pattern index.
    inline constexpr uint32_t kTessellationTablePo2Size = 16;
    // UE BarycentricMax (NaniteTessellation.ush): barycentrics are fixed point in this unit. Must be a power of
    // two.
    inline constexpr uint32_t kBarycentricMax = 1u << 15;
    // Level L+1 halves the target edge length of level L; level 0 targets the mesh's median edge.
    inline constexpr uint32_t kDisplacementLevelCount = 8;
    // UE NANITE_MAX_CLUSTER_TRIANGLES.
    inline constexpr uint32_t kDisplacementClusterTriangles = 128;
    // UE's split cap: a patch that needs splitting splits each edge into at most this many children per round.
    inline constexpr uint32_t kMaxSplitFactor = 8;
    // The fewest fixed-point barycentric units one diced segment may span (UE's disabled "avoid infinite recursion"
    // guard used 2; 64 keeps the edge snapping of nested patches exact).
    inline constexpr uint32_t kMinBarycentricStep = 64;

    // ---------------------------------------------------------------------------------------------------------
    // The pattern table (UE FTessellationTable, the regular table; the immediate-mode table serves UE's hardware
    // rasteriser path, which has no counterpart here).

    struct TessellationPattern
    {
        uint32_t VertexOffset   = 0; // into TessellationTable::Verts
        uint32_t Vertices       = 0;
        uint32_t TriangleOffset = 0; // into TessellationTable::Triangles
        uint32_t Triangles      = 0;
    };

    struct TessellationTable
    {
        // Indexed by PatternIndex(); only descending triples (X >= Y >= Z) are filled.
        std::vector<TessellationPattern> Patterns;
        // Per pattern vertex: barycentrics x | y << 16 (z = kBarycentricMax - x - y), UE's encoding.
        std::vector<uint32_t> Verts;
        // Per pattern triangle: three pattern-local vertex indices.
        std::vector<std::array<uint16_t, 3>> Triangles;

        // UE GetPattern: factors each 1..kMaxEdgeFactor, sorted descending -> index.
        [[nodiscard]] static uint32_t PatternIndex( uint32_t x, uint32_t y, uint32_t z );
    };

    // Built on first call (thread-safe), then shared for the life of the process.
    [[nodiscard]] const TessellationTable& GetTessellationTable();

    // ---------------------------------------------------------------------------------------------------------
    // Per-mesh topology (cooked).

    struct DisplacementCluster
    {
        uint32_t              Submesh = 0;
        std::vector<uint32_t> Triangles;  // mesh-global triangle ids (index buffer triangle / 3)
        glm::vec3             BoundsMin{ 0.0f }; // object space, undisplaced
        glm::vec3             BoundsMax{ 0.0f };
        std::vector<uint32_t> Neighbours; // clusters sharing a welded edge with this one (sorted, unique)
        // The highest level whose every edge still leaves kMinBarycentricStep fixed-point units per diced segment:
        // a resolution limit of the barycentric encoding, not a budget. Requests above it are clamped and counted.
        uint8_t MaxLevel = 0;
        bool                  operator==( const DisplacementCluster& ) const = default;
    };

    struct DisplacementTopology
    {
        // Per LOD0 triangle (mesh-global id t, corners 3t..3t+2 in index-buffer order).
        std::vector<uint32_t> CornerVertex; // the mesh vertex each corner reads (base vertex applied)
        // Submesh positions are welded only within submeshes whose Transform is equal (positions are submesh-local).
        std::vector<uint32_t> CornerWeld;   // the welded vertex of each corner
        std::vector<uint32_t> CornerEdge;   // the welded edge from corner c to corner (c+1)%3
        std::vector<uint32_t> TriangleSubmesh;
        std::vector<uint32_t> TriangleCluster;

        // Per welded vertex.
        std::vector<glm::vec3> WeldPosition;
        std::vector<glm::vec3> WeldDirection;    // normalised sum of the copies' normals
        std::vector<uint32_t>  WeldCornerOffset; // CSR into WeldCorners, size = welds + 1
        std::vector<uint32_t>  WeldCorners;      // corners (3t + c) of every copy, ascending

        // Per welded edge (A < B).
        std::vector<std::array<uint32_t, 2>> EdgeWelds;
        std::vector<uint32_t>                EdgeTriangleOffset; // CSR into EdgeTriangles, size = edges + 1
        std::vector<uint32_t>                EdgeTriangles;      // incident triangles, ascending

        std::vector<DisplacementCluster> Clusters;

        // Level 0's target edge length (cm): the median welded edge length. Level L targets BaseEdgeCm / 2^L.
        float BaseEdgeCm = 0.0f;

        [[nodiscard]] uint64_t GetBytes() const;
        bool                   operator==( const DisplacementTopology& ) const = default;
    };

    [[nodiscard]] Common::ResultStr<DisplacementTopology>
    BuildDisplacementTopology( std::span<const Vertex> vertices, std::span<const Index> indices,
                               std::span<const Submesh> submeshes );

    // Cooked form (its own DDC bucket next to the mesh's render data).
    [[nodiscard]] std::vector<uint8_t> EncodeDisplacementTopology( const DisplacementTopology& );
    [[nodiscard]] Common::ResultStr<DisplacementTopology> DecodeDisplacementTopology( std::span<const uint8_t> );

    // ---------------------------------------------------------------------------------------------------------
    // One cluster tessellated. Every record is plain data the emit pass reads (std430 layouts in
    // Shaders/Common/DisplacementRecords.glslh mirror these four structs).

    // A displaced point, described canonically: P = (B0*W[0] + B1*W[1] + (Max-B0-B1)*W[2]) / Max over welded
    // vertices W ascending (unused slots repeat the last id with weight 0). Its height is the average of its
    // samples.
    struct DisplacementPoint
    {
        std::array<uint32_t, 3> Welds{};
        uint32_t                Weights                                      = 0; // B0 | B1 << 16
        uint32_t                SampleOffset                                 = 0;
        uint32_t                SampleCount                                  = 0;
        // The level its height is sampled at (the texture mip matches the diced edge): the cluster's level inside
        // a triangle, the welded edge's (or welded vertex's) highest touching level on a border - so the record,
        // and therefore the displaced position, is identical in every cluster that emits it.
        uint32_t Level = 0;
        bool     operator==( const DisplacementPoint& ) const = default;
    };
    static_assert( sizeof( DisplacementPoint ) == 28 );

    // A height sample: the material of triangle Triangle's submesh, at barycentrics b0 | b1 << 16 in that
    // triangle's corner order (UV, channel and transform from the triangle's own vertices).
    struct DisplacementSample
    {
        uint32_t Triangle                                      = 0;
        uint32_t Barycentrics                                  = 0;
        bool     operator==( const DisplacementSample& ) const = default;
    };
    static_assert( sizeof( DisplacementSample ) == 8 );

    // An output vertex: attributes interpolated in Triangle at Barycentrics, position = the displaced Point.
    struct DisplacementVertex
    {
        uint32_t Triangle                                      = 0;
        uint32_t Barycentrics                                  = 0;
        uint32_t Point                                         = 0;
        bool     operator==( const DisplacementVertex& ) const = default;
    };
    static_assert( sizeof( DisplacementVertex ) == 12 );

    struct TessellatedCluster
    {
        std::vector<DisplacementPoint>  Points;
        std::vector<DisplacementSample> Samples;
        std::vector<DisplacementVertex> Vertices;
        std::vector<uint32_t>           Indices; // cluster-local vertex indices, 3 per triangle
        bool                            operator==( const TessellatedCluster& ) const = default;
    };

    // Tessellates cluster @p cluster with @p clusterLevels (one level 0..kDisplacementLevelCount-1 per cluster of
    // the mesh). An edge interior to the cluster uses the cluster's level; a welded edge on its border uses the
    // HIGHEST level among the clusters touching it, so both sides cut it identically (no T-junction). The result
    // therefore depends on the cluster's level and its Neighbours' levels, and on nothing else.
    [[nodiscard]] Common::ResultStr<TessellatedCluster>
    TessellateCluster( const DisplacementTopology& topology, uint32_t cluster,
                       std::span<const uint8_t> clusterLevels );

    // The level a cluster asks for when one centimetre of its surface covers @p pixelsPerCm pixels and an edge
    // should cover @p dicingRatePixels after dicing (UE r.Nanite.DicingRate).
    // Clamped to the cluster's MaxLevel; @p clamped (optional) reports that the clamp applied.
    [[nodiscard]] uint8_t DisplacementLevelForDensity( const DisplacementTopology& topology, uint32_t cluster,
                                                       float pixelsPerCm, float dicingRatePixels,
                                                       bool* clamped = nullptr );
} // namespace Desert::Geometry
