// Ported from UE 5.8
// Engine/Source/Runtime/GeometryCore/Public/DynamicMesh/Operations/MergeCoincidentMeshEdges.h:1-91 and
// Private/DynamicMesh/Operations/MergeCoincidentMeshEdges.cpp:1-235, adapted: namespace Desert::Geometry, UE Core
// via UECore.hpp; TPointHashGrid3 (FindPointsInBall/InsertPointUnsafe) and IndexPriorityQueue (Insert/Dequeue)
// are ported as the subset Apply uses, private to the .cpp - the hash cell is a std::unordered_map bucket, so
// points of one cell are visited in insertion order where UE's TMultiMap visits them newest-first; the equivalence
// sets are owned by std::unique_ptr instead of new/delete.
#pragma once

#include "Engine/Geometry/UECore/UECore.hpp"

#include "Engine/Geometry/UECore/DynamicMesh/DynamicMesh3.hpp"
#include "Engine/Geometry/UECore/DynamicMesh/Operations/SplitAttributeWelder.hpp"

namespace Desert::Geometry
{
    /**
     * Finds pairs of boundary edges of the mesh that are identical (endpoint vertices at the same locations) and
     * merges each pair into a single edge. Similar to welding vertices but safer: it cannot form bowties. Two
     * edges with the same orientation (from their triangles) cannot be merged.
     */
    class MergeCoincidentMeshEdges
    {
    public:
        /** Default tolerance is float ZeroTolerance. */
        static const double DEFAULT_TOLERANCE;

        DynamicMesh3* m_Mesh;
        /** Edges are coincident if both pairs of endpoint vertices are closer than this distance. */
        double m_MergeVertexTolerance = DEFAULT_TOLERANCE;
        /** Only merge unambiguous pairs that have unique duplicate-edge matches. */
        bool m_OnlyUniquePairs = false;
        /** Edges are candidates if their midpoints are within this distance; zero means MergeVertexTolerance*2. */
        double  m_MergeSearchTolerance    = 0;
        int32_t m_InitialNumBoundaryEdges = 0;
        int32_t m_FinalNumBoundaryEdges   = 0;
        /** Weld split attributes at the vertices of each merged edge. */
        bool                 m_bWeldAttrsOnMergedEdges = false;
        SplitAttributeWelder m_SplitAttributeWelder;
        /** Edges to merge (a pair qualifies when EITHER edge is in it); null merges across the entire mesh. */
        std::unordered_set<int32_t>* m_EdgesToMerge = nullptr;

        explicit MergeCoincidentMeshEdges( DynamicMesh3* mesh ) : m_Mesh( mesh )
        {
        }
        virtual ~MergeCoincidentMeshEdges() = default;

        /** Run the merge and modify Mesh; true if the algorithm succeeds. */
        virtual bool Apply();

    protected:
        double m_MergeVtxDistSqr = 0; // cached

        // the endpoint order is unknown, so both combinations are tried
        bool IsSameEdge( const glm::dvec3& a, const glm::dvec3& b, const glm::dvec3& c, const glm::dvec3& d ) const
        {
            return ( DistSq( a, c ) < m_MergeVtxDistSqr && DistSq( b, d ) < m_MergeVtxDistSqr ) ||
                   ( DistSq( a, d ) < m_MergeVtxDistSqr && DistSq( b, c ) < m_MergeVtxDistSqr );
        }
        static double DistSq( const glm::dvec3& a, const glm::dvec3& b )
        {
            const double x = a.x - b.x, y = a.y - b.y, z = a.z - b.z;
            return x * x + y * y + z * z;
        }
    };
} // namespace Desert::Geometry
