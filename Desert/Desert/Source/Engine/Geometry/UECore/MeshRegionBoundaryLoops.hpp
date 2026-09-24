// Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Public/MeshRegionBoundaryLoops.h:1-162,
// Engine/Source/Runtime/GeometryCore/Public/EdgeLoop.h and EdgeSpan.h (the Vertices/Edges/BowtieVertices payload,
// Initialize, InitializeFromVertices/InitializeFromEdges, IsBoundaryLoop; EdgeLoop.cpp:5-60,134-146), adapted: UE
// Core via UECore.hpp, namespace Desert::Geometry, FIndexFlagSet is a TArray<bool> over the ID range. Bowtie
// vertices (a region vertex touching more than two region boundary edges) are refused with a named error instead
// of being split by UE's FindLeftTurnEdge/TryExtractSubloops: no Modeling mesh the editor produces has one, and a
// refused region is reported, never mis-walked.
#pragma once

#include "Engine/Geometry/UECore/UECore.hpp"

#include "Engine/Geometry/UECore/DynamicMesh/DynamicMesh3.hpp"

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
