// Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Public/MeshRegionBoundaryLoops.h:1-162,
// Engine/Source/Runtime/GeometryCore/Public/EdgeLoop.h and EdgeSpan.h (the Vertices/Edges/BowtieVertices payload,
// Initialize, InitializeFromVertices/InitializeFromEdges, IsBoundaryLoop; EdgeLoop.cpp:5-60,134-146), adapted: UE
// Core via UECore.hpp, namespace Desert::Geometry, FIndexFlagSet is a TArray<bool> over the ID range. Bowtie
// vertices (a region vertex touching more than two region boundary edges) are refused with a named error instead
// of being split by UE's FindLeftTurnEdge/TryExtractSubloops: no Modeling mesh the editor produces has one, and a
// refused region is reported, never mis-walked. The loop overlay map (ElementIDAndValue, VidOverlayMap,
// GetLoopOverlayMap, UpdateLoopOverlayMapValidity; header :76-106) is UE's, instantiated for UV layers as UE does.
#pragma once

#include "Engine/Geometry/UECore/UECore.hpp"

#include "Engine/Geometry/UECore/DynamicMesh/DynamicMesh3.hpp"
#include "Engine/Geometry/UECore/DynamicMesh/DynamicMeshOverlay.hpp"

#include <string>

namespace Desert::Geometry
{
    /** A closed loop of mesh vertices; Edges[i] joins Vertices[i] and Vertices[(i+1) % Num]. */
    struct FEdgeLoop
    {
        TArray<int> Vertices;
        TArray<int> Edges;
        TArray<int> BowtieVertices;

        void Initialize( const TArray<int>& VerticesIn, const TArray<int>& EdgesIn,
                         const TArray<int>* BowtieVerticesIn = nullptr );
        /** UE InitializeFromVertices with bAutoOrient = false (FMeshBoundaryLoops never orients). */
        bool InitializeFromVertices( const FDynamicMesh3& Mesh, const TArray<int>& VerticesIn );
        /** UE EdgeLoop.cpp:21-38: Vertices[i] is the vertex Edges[i-1] and Edges[i] share. */
        void InitializeFromEdges( const FDynamicMesh3& Mesh, const TArray<int>& EdgesIn );
        bool IsBoundaryLoop( const FDynamicMesh3& Mesh ) const;
        int  GetVertexCount() const
        {
            return Vertices.Num();
        }
        int GetEdgeCount() const
        {
            return Edges.Num();
        }
    };

    /** An open span of mesh vertices; Edges[i] joins Vertices[i] and Vertices[i+1]. */
    struct FEdgeSpan
    {
        TArray<int> Vertices;
        TArray<int> Edges;
        TArray<int> BowtieVertices;

        void InitializeFromVertices( const FDynamicMesh3& Mesh, const TArray<int>& VerticesIn );
        /** For a closed loop of edges, Vertices ends with its first vertex repeated (as UE). */
        void InitializeFromEdges( const FDynamicMesh3& Mesh, const TArray<int>& EdgesIn );
    };

    /** Extracts the boundary loops of a triangle region, oriented with the region on the left. */
    class FMeshRegionBoundaryLoops
    {
    public:
        const FDynamicMesh3* Mesh = nullptr;
        TArray<FEdgeLoop>    Loops;
        bool                 bFailed = false;
        /** Why Compute failed, with the vertex/edge that stopped it. Empty on success. */
        std::string FailureReason;

        FMeshRegionBoundaryLoops( const FDynamicMesh3* MeshIn, const TArray<int>& RegionTris,
                                  bool bAutoCompute = true );

        bool Compute();

        template <typename ElementType>
        using ElementIDAndValue = TPair<int32, ElementType>;
        template <typename ElementType>
        using VidOverlayMap = TMap<int32, ElementIDAndValue<ElementType>>;

        /**
         * Maps each loop vertex to the overlay element (ID and value) it has in the region triangle whose edge
         * leaves that vertex in loop direction, so the triangles inside the loop can be deleted and replaced while
         * the border keeps its UVs. Adds to LoopVidsToOverlayElementsOut without clearing it, so several loops can
         * share one map.
         * @return false if the loop edge is not on this region's boundary or the overlay has no element there.
         */
        template <typename StorageType, int ElementSize, typename ElementType>
        bool GetLoopOverlayMap( const FEdgeLoop&                                     LoopIn,
                                const TDynamicMeshOverlay<StorageType, ElementSize>& Overlay,
                                VidOverlayMap<ElementType>& LoopVidsToOverlayElementsOut ) const;

        /**
         * After the loop's inner triangles are deleted, marks InvalidID every mapped element the overlay no longer
         * holds (the vertex sat on a seam, so only the deleted triangles referenced that element).
         */
        template <typename StorageType, int ElementSize, typename ElementType>
        static void UpdateLoopOverlayMapValidity( VidOverlayMap<ElementType>& LoopVidsToOverlayElements,
                                                  const TDynamicMeshOverlay<StorageType, ElementSize>& Overlay );

    private:
        TArray<bool> Triangles; // membership over [0, MaxTriangleID)
        TArray<bool> Edges;     // region-boundary membership over [0, MaxEdgeID)
        TArray<int>  EdgesRoi;

        bool IsEdgeOnBoundary( int Eid ) const
        {
            return Edges[Eid];
        }
        bool     IsEdgeOnBoundary( int Eid, int& TidIn, int& TidOut ) const;
        FIndex2i GetOrientedEdgeVerts( int Eid, int TidIn ) const;
        int      GetVertexBoundaryEdges( int Vid, int& E0, int& E1 ) const;
    };
} // namespace Desert::Geometry
