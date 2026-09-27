// Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Public/Operations/MeshMeshCut.h:84-160 (FMeshMeshCut),
// adapted: std/glm, namespace Desert::Geometry. Left out: FMeshSelfCut (no caller: nothing here finds a mesh's
// self-intersections), bCutCoplanar (UE's own "TODO: not implemented!" guarded by check(!bCutCoplanar)),
// Validate (always Ok), and SegmentToChain (MeshBoolean, the only caller, reads VertexChains alone).
// VertexChains are always recorded: UE's bTrackInsertedVertices is set by MeshBoolean from
// bCollapseDegenerateEdgesOnCut, which is on by default and has no other setter here.
#pragma once

#include "Engine/Geometry/MeshCore/DynamicMesh/DynamicMesh3.hpp"
#include "Engine/Geometry/MeshCore/MathUtil.hpp"
#include "Engine/Geometry/MeshCore/Spatial/MeshAABBTree3.hpp"

#include <array>
#include <vector>

namespace Desert::Geometry
{
    /**
     * Cut a mesh where it crosses a second mesh -- resolving all intersections, but not deleting geometry.
     * Optionally resolve intersections mutually for both meshes.
     */
    class MeshMeshCut
    {
    public:
        /** Meshes to cut -- Cut is destructive, so these are also outputs */
        std::array<DynamicMesh3*, 2> Mesh;

        /** Tolerance distance for considering a point to be on a vertex, edge or plane */
        double SnapTolerance = ZeroTolerance<float> * 100.0;

        /** If true, modify both meshes to split at crossing points; otherwise only modify Mesh[0] */
        bool bMutuallyCut = true;

        /**
         * Output: packed chains of vertex IDs, representing the vertices for each segment in the mesh, packed as
         * the number of vertices in that chain followed by that many vertex IDs, per segment. NOT 1:1 with
         * segments; some segments may have failed to insert.
         */
        std::array<std::vector<int>, 2> VertexChains;

        MeshMeshCut( DynamicMesh3* MeshA, DynamicMesh3* MeshB ) : Mesh{ MeshA, MeshB }
        {
        }

        /**
         * Split mesh(es) along the provided intersections.
         * @param Intersections mesh-mesh intersections, as returned by DynamicMeshAABBTree3::FindAllIntersections
         * @return false if any segment could not be embedded (the meshes are still valid, just not fully cut)
         */
        bool Cut( const MeshIntersection::IntersectionsQueryResult& Intersections );

        void ResetOutputs()
        {
            VertexChains[0].clear();
            VertexChains[1].clear();
        }
    };
} // namespace Desert::Geometry
