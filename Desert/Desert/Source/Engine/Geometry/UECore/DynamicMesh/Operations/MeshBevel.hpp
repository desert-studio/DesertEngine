// Ported from UE 5.8
// Engine/Plugins/Runtime/GeometryProcessing/Source/DynamicMesh/Public/Operations/MeshBevel.h:26-378 and
// Private/Operations/MeshBevel.cpp:47-131,576-668,669-2082,3740-3774,3814-3969 (setup, topology build, unlink,
// displacement and meshing of the chamfer bevel, Apply, normals and material IDs),
// adapted: UE Core via UECore.hpp, namespace Desert::Geometry. FGeometryResult / FProgressCancel are replaced by a
// named FailureReason, and a vertex UE would leave as EBevelVertexType::Unknown (silently not beveled) is REFUSED
// with the vertex and the cause; bowtie vertices on the bevel graph are refused up front instead of FixBowties'
// SplitBowties (B:415-574). The fields UE marks deprecated in 5.5 (GroupEdgeID, GroupIDs, CornerID,
// IncomingBevelTopoEdges) and the multi-segment data (StripQuadPatch, NormalsA/B, InteriorVertices,
// InteriorBorderLoop) have no reader in the chamfer path and are not ported.
#pragma once

#include "Engine/Geometry/UECore/UECore.hpp"

#include "Engine/Geometry/UECore/DynamicMesh/DynamicMesh3.hpp"
#include "Engine/Geometry/UECore/DynamicMesh/GroupTopology.hpp"
#include "Engine/Geometry/UECore/MeshRegionBoundaryLoops.hpp"

#include <string>

namespace Desert::Geometry
{
    /**
     * Bevel of mesh edges, applied in place by unstitching the edges and inserting new triangles (UE FMeshBevel).
     * An isolated closed loop of edges becomes a quad strip; open edge spans meeting at a vertex of valence >= 3
     * replace that vertex by a polygon, a span ending at a vertex of valence 1 is terminated by expanding the
     * vertex into an edge.
     */
    class FMeshBevel
    {
    public:
        /** Why initialization refused the input; empty on success. */
        std::string FailureReason;

        /** Distance (cm) each beveled edge is inset into its two adjacent faces. */
        double InsetDistance = 5.0;

        /** Options for MaterialID assignment on the new triangles generated for the bevel */
        enum class EMaterialIDMode
        {
            ConstantMaterialID,
            InferMaterialID,
            InferMaterialID_ConstantIfAmbiguous
        };
        /** Which MaterialID assignment mode to use */
        EMaterialIDMode MaterialIDMode = EMaterialIDMode::ConstantMaterialID;
        /** Constant MaterialID used for various MaterialIDMode settings */
        int32 SetConstantMaterialID = 0;

        /** Triangles created by Apply: the vertex polygons, then the edge and loop strips. */
        TArray<int32> NewTriangles;

        /** Initialize the bevel with all edges of the given GroupTopology. */
        bool InitializeFromGroupTopology( const FDynamicMesh3& Mesh, const FGroupTopology& Topology );
        /** Initialize the bevel with the specified edges of a GroupTopology. */
        bool InitializeFromGroupTopologyEdges( const FDynamicMesh3& Mesh, const FGroupTopology& Topology,
                                               const TArray<int32>& GroupEdges );

        /**
         * Bevel the initialized edges in place (UE Apply, B:576): unlink, displace, mesh, then the primary normals
         * normals, primary UVs (one ExpMap island per region, as UE: other UV layers stay unset) and material
         * IDs of NewTriangles. Returns false with FailureReason (the mesh is then partially edited) as soon as a
         * phase refuses. Only the one-segment chamfer is ported:
         * UE's NumSubdivisions / round profile (CreateBevelMeshing_Multi) is task P13e.
         */
        bool Apply( FDynamicMesh3& Mesh );

        struct FBevelLoop
        {
            TArray<int32>     MeshVertices;     // sequential list of mesh vertex IDs along edge loop
            TArray<int32>     MeshEdges;        // sequential list of mesh edge IDs along edge loop
            TArray<FIndex2i>  MeshEdgeTris;     // the one or two triangles of each MeshEdges element in the input
            TArray<FVector3d> InitialPositions; // initial vertex positions
            TArray<int32>
                 NewMeshVertices; // vertices on the "other" side of the unlinked edge, 1-1 w/ MeshVertices
            TArray<int32>     NewMeshEdges;  // edges on the "other" side of the unlinked edge, 1-1 with MeshEdges
            TArray<FVector3d> NewPositions0; // new positions for MeshVertices
            TArray<FVector3d> NewPositions1; // new positions for NewMeshVertices
            TArray<int32>     NewGroupIDs;
            TArray<FIndex2i>  StripQuads; // triangle-ID pairs of each new quad along the edge, 1-1 with MeshEdges
        };

        struct FBevelEdge
        {
            int32            EdgeIndex = -1; // index of this BevelEdge in Edges
            TArray<int32>    MeshVertices;   // sequential list of mesh vertex IDs along edge
            TArray<int32>    MeshEdges;      // sequential list of mesh edge IDs along edge
            TArray<FIndex2i> MeshEdgeTris;   // the one or two triangles of each MeshEdges element in the input
            bool             bEndpointBoundaryFlag[2] = { false, false }; // start/end vertex was a boundary vertex
            TArray<FVector3d> InitialPositions;                           // initial vertex positions
            FIndex2i          BevelVertices; // indices of the Bevel Vertices at either end of the Bevel Edge
            TArray<int32>
                 NewMeshVertices; // vertices on the "other" side of the unlinked edge, 1-1 w/ MeshVertices
            TArray<int32>     NewMeshEdges;  // edges on the "other" side of the unlinked edge, 1-1 with MeshEdges
            TArray<FVector3d> NewPositions0; // new positions for MeshVertices
            TArray<FVector3d> NewPositions1; // new positions for NewMeshVertices
            int32             NewGroupID = -1;
            TArray<FIndex2i>  StripQuads; // triangle-ID pairs of each new quad along the edge, 1-1 with MeshEdges
        };

        struct FOneRingWedge
        {
            TArray<int32> Triangles;                // sequential triangles in this wedge
            FIndex2i      BorderEdges;              // first and last edges of Triangles (connected to the vertex)
            FIndex2i      BorderEdgeTriEdgeIndices; // index 0/1/2 of BorderEdges[j] in the start/end Triangles
            int32         WedgeVertex = -1; // central vertex of this wedge (updated by the unlink functions)
            FVector3d     NewPosition;      // new calculated position for the vertex of this wedge
            bool          bHaveNewPosition = false; // NewPosition is valid
        };

        enum class EBevelVertexType
        {
            JunctionVertex,
            TerminatorVertex,
            BoundaryVertex,
            Unknown
        };

        struct FBevelVertex
        {
            int32            VertexID   = -1; // initial mesh vertex ID of the Bevel Vertex
            EBevelVertexType VertexType = EBevelVertexType::Unknown;
            TArray<int32>    IncomingBevelMeshEdges; // (unsorted) mesh edges to be beveled, coming into the vertex
            TArray<int32>    IncomingBevelEdgeIndices; // (unsorted) indices of FBevelEdge coming into the vertex
            TArray<int32>    SortedTriangles;          // ordered triangle one-ring around VertexID
            TArray<FOneRingWedge>
                          Wedges; // ordered one-ring decomposition into wedges between incoming bevel edges
            int32         NewGroupID = -1;   // polygroup of the polygon generated by this vertex (NumEdges > 2)
            TArray<int32> NewTriangles;      // triangles of the polygon generated by this vertex (NumEdges > 2)
            FIndex2i      TerminatorInfo;    // TerminatorVertex: [EdgeID, FarVertexID] in the one-ring
            int32 ConnectedBevelVertex = -1; // another FBevelVertex index TerminatorInfo's edge directly reaches
        };

    protected:
        TMap<int32, int32>   VertexIDToIndexMap; // mesh vertex ID -> index into Vertices
        TArray<FBevelVertex> Vertices;           // mesh vertices that need beveling
        TArray<FBevelEdge>   Edges;              // mesh edge spans that need beveling
        TArray<FBevelLoop>   Loops;              // mesh edge loops that need beveling
        TMap<int32, int32>   MeshEdgePairs;      // input edges split into edge pairs, later stitched with quads

        FBevelVertex* GetBevelVertexFromVertexID( int32 VertexID, int32* IndexOut = nullptr );

        void AddBevelGroupEdge( const FDynamicMesh3& Mesh, const FGroupTopology& Topology, int32 GroupEdgeID );
        void AddBevelEdgeLoop( const FDynamicMesh3& Mesh, const FEdgeLoop& Loop );
        void BuildVertexSets( const FDynamicMesh3& Mesh );
        void BuildJunctionVertex( FBevelVertex& Vertex, const FDynamicMesh3& Mesh );
        void BuildTerminatorVertex( FBevelVertex& Vertex, const FDynamicMesh3& Mesh );

        // Unlink: split the bevel edges open; afterwards each MeshEdges[i] and NewMeshEdges[i] is a boundary edge
        // pair recorded in MeshEdgePairs. UE's FDynamicMeshChangeTracker parameter is dropped (not ported).
        void UnlinkEdges( FDynamicMesh3& Mesh );
        void UnlinkBevelEdgeInterior( FDynamicMesh3& Mesh, FBevelEdge& BevelEdge );
        void UnlinkLoops( FDynamicMesh3& Mesh );
        void UnlinkBevelLoop( FDynamicMesh3& Mesh, FBevelLoop& BevelLoop );
        void UnlinkVertices( FDynamicMesh3& Mesh );
        void UnlinkJunctionVertex( FDynamicMesh3& Mesh, FBevelVertex& Vertex );
        void UnlinkTerminatorVertex( FDynamicMesh3& Mesh, FBevelVertex& BevelVertex );
        void FixUpUnlinkedBevelEdges( const FDynamicMesh3& Mesh );

        /** Move the unlinked vertices InsetDistance into the adjacent faces (UE's Distance argument only fed its
         *  unused MeanValueCentroid fallback, so it is gone). */
        void DisplaceVertices( FDynamicMesh3& Mesh );

        /** Fill the unlinked holes: junction polygons, edge and loop quad strips, then the terminator triangles
         *  (last, so the strip's end edge orients them). */
        void CreateBevelMeshing( FDynamicMesh3& Mesh );
        void AppendJunctionVertexPolygon( FDynamicMesh3& Mesh, FBevelVertex& Vertex );
        void AppendTerminatorVertexTriangle( FDynamicMesh3& Mesh, FBevelVertex& Vertex );
        void AppendTerminatorVertexPairQuad( FDynamicMesh3& Mesh, FBevelVertex& Vertex0, FBevelVertex& Vertex1 );
        void AppendEdgeQuads( FDynamicMesh3& Mesh, FBevelEdge& Edge );
        void AppendLoopQuads( FDynamicMesh3& Mesh, FBevelLoop& Loop );

        /** Per-vertex normals within each new vertex polygon and each strip; no-op without attributes. */
        void ComputeNormals( FDynamicMesh3& Mesh );
        /** ExpMap primary UVs per strip, then per vertex polygon; no-op without a UV layer. */
        void ComputeUVs( FDynamicMesh3& Mesh );
        /** MaterialID of NewTriangles per MaterialIDMode; no-op without a MaterialID attribute. */
        void ComputeMaterialIDs( FDynamicMesh3& Mesh );

    private:
        void InitVertexSet( const FDynamicMesh3& Mesh, FBevelVertex& Vertex );
        void FinalizeTerminatorVertex( const FDynamicMesh3& Mesh, FBevelVertex& Vertex );
        /** Records the first refusal; later ones are consequences of the same input. */
        void Refuse( const std::string& Reason );
        bool RefuseBowties( const FDynamicMesh3& Mesh, const TArray<int32>& MeshVertices );
        /** SplitVertex, refusing (with Where and the result) when it fails; NewVertexOut = VertexID then. */
        bool SplitOrKeep( FDynamicMesh3& Mesh, int32 VertexID, const TArray<int32>& Triangles,
                          const std::string& Where, int32& NewVertexOut );
        void PairSplitWedgeBorderEdges( const FDynamicMesh3& Mesh, FBevelVertex& Vertex );
        /** AppendTriangle, refusing (with Where and the result) when it fails; UE drops such a triangle and
         *  leaves a hole. Returns the new triangle ID or the failed result. */
        int32 AppendOrRefuse( FDynamicMesh3& Mesh, int32 A, int32 B, int32 C, int32 GroupID,
                              const std::string& Where );
    };
} // namespace Desert::Geometry
