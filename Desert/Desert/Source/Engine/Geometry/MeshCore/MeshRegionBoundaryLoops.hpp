// Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Public/MeshRegionBoundaryLoops.h:1-162,
// Engine/Source/Runtime/GeometryCore/Public/EdgeLoop.h and EdgeSpan.h (the Vertices/Edges/BowtieVertices payload,
// Initialize, InitializeFromVertices/InitializeFromEdges, IsBoundaryLoop; EdgeLoop.cpp:5-60,134-146), adapted: UE
// Core as std/glm, namespace Desert::Geometry, FIndexFlagSet is a TArray<bool> over the ID range. Bowtie
// vertices (a region vertex touching more than two region boundary edges) are refused with a named error instead
// of being split by UE's FindLeftTurnEdge/TryExtractSubloops: no Modeling mesh the editor produces has one, and a
// refused region is reported, never mis-walked. The loop overlay map (ElementIDAndValue, VidOverlayMap,
// GetLoopOverlayMap, UpdateLoopOverlayMapValidity; header :76-106) is UE's, instantiated for UV layers as UE does.
#pragma once


#include "Engine/Geometry/MeshCore/DynamicMesh/DynamicMesh3.hpp"
#include "Engine/Geometry/MeshCore/DynamicMesh/DynamicMeshOverlay.hpp"

#include <string>
#include <unordered_map>

namespace Desert::Geometry
{
    /** A closed loop of mesh vertices; Edges[i] joins Vertices[i] and Vertices[(i+1) % Num]. */
    struct EdgeLoop
    {
        std::vector<int> Vertices;
        std::vector<int> Edges;
        std::vector<int> BowtieVertices;

        void Initialize( const std::vector<int>& VerticesIn, const std::vector<int>& EdgesIn,
                         const std::vector<int>* BowtieVerticesIn = nullptr );
        /** UE InitializeFromVertices with bAutoOrient = false (MeshBoundaryLoops never orients). */
        bool InitializeFromVertices( const DynamicMesh3& Mesh, const std::vector<int>& VerticesIn );
        /** UE EdgeLoop.cpp:21-38: Vertices[i] is the vertex Edges[i-1] and Edges[i] share. */
        void               InitializeFromEdges( const DynamicMesh3& Mesh, const std::vector<int>& EdgesIn );
        [[nodiscard]] bool IsBoundaryLoop( const DynamicMesh3& Mesh ) const;
        [[nodiscard]] int  GetVertexCount() const
        {
            return static_cast<int32_t>( Vertices.size() );
        }
        [[nodiscard]] int GetEdgeCount() const
        {
            return static_cast<int32_t>( Edges.size() );
        }
    };

    /** An open span of mesh vertices; Edges[i] joins Vertices[i] and Vertices[i+1]. */
    struct EdgeSpan
    {
        std::vector<int> Vertices;
        std::vector<int> Edges;
        std::vector<int> BowtieVertices;

        void InitializeFromVertices( const DynamicMesh3& Mesh, const std::vector<int>& VerticesIn );
        /** For a closed loop of edges, Vertices ends with its first vertex repeated (as UE). */
        void InitializeFromEdges( const DynamicMesh3& Mesh, const std::vector<int>& EdgesIn );
    };

    /** Extracts the boundary loops of a triangle region, oriented with the region on the left. */
    class MeshRegionBoundaryLoops
    {
    public:
        const DynamicMesh3*   m_Mesh = nullptr;
        std::vector<EdgeLoop> m_Loops;
        bool                  m_bFailed = false;
        /** Why Compute failed, with the vertex/edge that stopped it. Empty on success. */
        std::string m_FailureReason;

        MeshRegionBoundaryLoops( const DynamicMesh3* MeshIn, const std::vector<int>& RegionTris,
                                 bool bAutoCompute = true );

        bool Compute();

        template <typename ElementType>
        using ElementIDAndValue = std::pair<int32_t, ElementType>;
        template <typename ElementType>
        using VidOverlayMap = std::unordered_map<int32_t, ElementIDAndValue<ElementType>>;

        /**
         * Maps each loop vertex to the overlay element (ID and value) it has in the region triangle whose edge
         * leaves that vertex in loop direction, so the triangles inside the loop can be deleted and replaced while
         * the border keeps its UVs. Adds to LoopVidsToOverlayElementsOut without clearing it, so several loops can
         * share one map.
         * @return false if the loop edge is not on this region's boundary or the overlay has no element there.
         */
        template <typename StorageType, int ElementSize, typename ElementType>
        bool GetLoopOverlayMap( const EdgeLoop&                                     LoopIn,
                                const DynamicMeshOverlay<StorageType, ElementSize>& Overlay,
                                VidOverlayMap<ElementType>& LoopVidsToOverlayElementsOut ) const;

        /**
         * After the loop's inner triangles are deleted, marks InvalidID every mapped element the overlay no longer
         * holds (the vertex sat on a seam, so only the deleted triangles referenced that element).
         */
        template <typename StorageType, int ElementSize, typename ElementType>
        static void UpdateLoopOverlayMapValidity( VidOverlayMap<ElementType>& LoopVidsToOverlayElements,
                                                  const DynamicMeshOverlay<StorageType, ElementSize>& Overlay );

    private:
        std::vector<bool> m_Triangles; // membership over [0, MaxTriangleID)
        std::vector<bool> m_Edges;     // region-boundary membership over [0, MaxEdgeID)
        std::vector<int>  m_EdgesRoi;

        [[nodiscard]] bool IsEdgeOnBoundary( int Eid ) const
        {
            return m_Edges[Eid];
        }
        bool     IsEdgeOnBoundary( int Eid, int& TidIn, int& TidOut ) const;
        [[nodiscard]] Index2i GetOrientedEdgeVerts( int Eid, int TidIn ) const;
        int      GetVertexBoundaryEdges( int Vid, int& E0, int& E1 ) const;
    };
} // namespace Desert::Geometry
