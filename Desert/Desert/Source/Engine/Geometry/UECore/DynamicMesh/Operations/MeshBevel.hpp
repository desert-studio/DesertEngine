// Ported from UE 5.8
// Engine/Plugins/Runtime/GeometryProcessing/Source/DynamicMesh/Public/Operations/MeshBevel.h:26-378 and
// Private/Operations/MeshBevel.cpp:47-131,576-668,669-2082,2084-3240,3740-3774,3814-3969 (setup, topology build,
// unlink, displacement, meshing of the chamfer and of the multi-segment flat bevel, Apply, normals, material IDs),
// adapted: UE Core via UECore.hpp, namespace Desert::Geometry. FGeometryResult / FProgressCancel are replaced by a
// named FailureReason, and a vertex UE would leave as EBevelVertexType::Unknown (silently not beveled) is REFUSED
// with the vertex and the cause; bowtie vertices on the bevel graph are refused up front instead of FixBowties'
// SplitBowties (B:415-574). The fields UE marks deprecated in 5.5 (GroupEdgeID, GroupIDs, CornerID,
// IncomingBevelTopoEdges) are not ported, nor is the round profile (RoundWeight, MakeArcSplineCurve,
// ApplyProfileShape_Round B:3241-3739) with the data only it reads (NormalsA/B, InteriorVertices,
// InteriorBorderLoop): a multi-segment bevel here has UE's RoundWeight = 0 flat profile.
#pragma once

#include "Engine/Geometry/UECore/UECore.hpp"

#include "Engine/Geometry/UECore/DynamicMesh/DynamicMesh3.hpp"
#include "Engine/Geometry/UECore/DynamicMesh/GroupTopology.hpp"
#include "Engine/Geometry/UECore/MeshRegionBoundaryLoops.hpp"
#include "Engine/Geometry/UECore/Selections/QuadGridPatch.hpp"

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

        /** Number of subdivisions inserted in each bevel strip; 0 is the one-segment chamfer. The profile across
         * the strip is flat (UE RoundWeight = 0), so every subdivision lies in the chamfer plane. */
        int32_t NumSubdivisions = 0;

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
        int32_t SetConstantMaterialID = 0;

        /** Triangles created by Apply: the vertex polygons, then the edge and loop strips. */
        TArray<int32_t> NewTriangles;

        /** Initialize the bevel with all edges of the given GroupTopology. */
        bool InitializeFromGroupTopology( const FDynamicMesh3& Mesh, const FGroupTopology& Topology );
        /** Initialize the bevel with the specified edges of a GroupTopology. */
        bool InitializeFromGroupTopologyEdges( const FDynamicMesh3& Mesh, const FGroupTopology& Topology,
                                               const TArray<int32_t>& GroupEdges );

        /**
         * Bevel the initialized edges in place (UE Apply, B:576): unlink, displace, mesh, then the primary normals
         * normals, primary UVs (one ExpMap island per region, as UE: other UV layers stay unset) and material
         * IDs of NewTriangles. Returns false with FailureReason (the mesh is then partially edited) as soon as a
         * phase refuses. NumSubdivisions > 0 meshes through CreateBevelMeshing_Multi (flat profile).
         */
        bool Apply( FDynamicMesh3& Mesh );

        struct FBevelLoop
        {
            TArray<int32_t>   MeshVertices;     // sequential list of mesh vertex IDs along edge loop
            TArray<int32_t>   MeshEdges;        // sequential list of mesh edge IDs along edge loop
            TArray<FIndex2i>  MeshEdgeTris;     // the one or two triangles of each MeshEdges element in the input
            TArray<FVector3d> InitialPositions; // initial vertex positions
            TArray<int32_t>
                 NewMeshVertices; // vertices on the "other" side of the unlinked edge, 1-1 w/ MeshVertices
            TArray<int32_t>   NewMeshEdges;  // edges on the "other" side of the unlinked edge, 1-1 with MeshEdges
            TArray<FVector3d> NewPositions0; // new positions for MeshVertices
            TArray<FVector3d> NewPositions1; // new positions for NewMeshVertices
            TArray<int32_t>   NewGroupIDs;
            TArray<FIndex2i>  StripQuads; // triangle-ID pairs of the new quads (1-1 with MeshEdges in the chamfer)
            FQuadGridPatch    StripQuadPatch; // only initialized in multi-segment bevel
        };

        struct FBevelEdge
        {
            int32_t           EdgeIndex = -1; // index of this BevelEdge in Edges
            TArray<int32_t>   MeshVertices;   // sequential list of mesh vertex IDs along edge
            TArray<int32_t>   MeshEdges;      // sequential list of mesh edge IDs along edge
            TArray<FIndex2i> MeshEdgeTris;   // the one or two triangles of each MeshEdges element in the input
            bool             bEndpointBoundaryFlag[2] = { false, false }; // start/end vertex was a boundary vertex
            TArray<FVector3d> InitialPositions;                           // initial vertex positions
            FIndex2i          BevelVertices; // indices of the Bevel Vertices at either end of the Bevel Edge
            TArray<int32_t>
                 NewMeshVertices; // vertices on the "other" side of the unlinked edge, 1-1 w/ MeshVertices
            TArray<int32_t>   NewMeshEdges;  // edges on the "other" side of the unlinked edge, 1-1 with MeshEdges
            TArray<FVector3d> NewPositions0; // new positions for MeshVertices
            TArray<FVector3d> NewPositions1; // new positions for NewMeshVertices
            int32_t           NewGroupID = -1;
            TArray<FIndex2i>  StripQuads; // triangle-ID pairs of the new quads (1-1 with MeshEdges in the chamfer)
            FQuadGridPatch    StripQuadPatch; // only initialized in multi-segment bevel
        };

        struct FOneRingWedge
        {
            TArray<int32_t> Triangles;                // sequential triangles in this wedge
            FIndex2i      BorderEdges;              // first and last edges of Triangles (connected to the vertex)
            FIndex2i      BorderEdgeTriEdgeIndices; // index 0/1/2 of BorderEdges[j] in the start/end Triangles
            int32_t         WedgeVertex = -1; // central vertex of this wedge (updated by the unlink functions)
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
            int32_t          VertexID   = -1; // initial mesh vertex ID of the Bevel Vertex
            EBevelVertexType VertexType = EBevelVertexType::Unknown;
            TArray<int32_t>  IncomingBevelMeshEdges; // (unsorted) mesh edges to be beveled, coming into the vertex
            TArray<int32_t>  IncomingBevelEdgeIndices; // (unsorted) indices of FBevelEdge coming into the vertex
            TArray<int32_t>  SortedTriangles;          // ordered triangle one-ring around VertexID
            TArray<FOneRingWedge>
                          Wedges; // ordered one-ring decomposition into wedges between incoming bevel edges
            int32_t         NewGroupID = -1;   // polygroup of the polygon generated by this vertex (NumEdges > 2)
            TArray<int32_t> NewTriangles;      // triangles of the polygon generated by this vertex (NumEdges > 2)
            FIndex2i      TerminatorInfo;    // TerminatorVertex: [EdgeID, FarVertexID] in the one-ring
            int32_t ConnectedBevelVertex = -1; // another FBevelVertex index TerminatorInfo's edge directly reaches
        };

    protected:
        TMap<int32_t, int32_t> VertexIDToIndexMap; // mesh vertex ID -> index into Vertices
        TArray<FBevelVertex> Vertices;           // mesh vertices that need beveling
        TArray<FBevelEdge>   Edges;              // mesh edge spans that need beveling
        TArray<FBevelLoop>   Loops;              // mesh edge loops that need beveling
        TMap<int32_t, int32_t> MeshEdgePairs;      // input edges split into edge pairs, later stitched with quads

        FBevelVertex* GetBevelVertexFromVertexID( int32_t VertexID, int32_t* IndexOut = nullptr );

        void AddBevelGroupEdge( const FDynamicMesh3& Mesh, const FGroupTopology& Topology, int32_t GroupEdgeID );
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

        /** Multi-segment meshing: the edge and loop strips first (NumSubdivisions + 1 quad rows each, kept as a
         *  StripQuadPatch), then the junction polygons tessellated to match their columns, terminators last. */
        void CreateBevelMeshing_Multi( FDynamicMesh3& Mesh );
        void AppendEdgeQuads_Multi( FDynamicMesh3& Mesh, FBevelEdge& Edge );
        void AppendLoopQuads_Multi( FDynamicMesh3& Mesh, FBevelLoop& Loop );
        void AppendJunctionVertexPolygon_Multi( FDynamicMesh3& Mesh, FBevelVertex& Vertex );
        void AppendTerminatorVertexTriangles_Multi( FDynamicMesh3& Mesh, FBevelVertex& Vertex );
        void AppendTerminatorVertexPairQuad_Multi( FDynamicMesh3& Mesh, FBevelVertex& Vertex0,
                                                   FBevelVertex& Vertex1 );

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
        bool RefuseBowties( const FDynamicMesh3& Mesh, const TArray<int32_t>& MeshVertices );
        /** SplitVertex, refusing (with Where and the result) when it fails; NewVertexOut = VertexID then. */
        bool SplitOrKeep( FDynamicMesh3& Mesh, int32_t VertexID, const TArray<int32_t>& Triangles,
                          const std::string& Where, int32_t& NewVertexOut );
        void PairSplitWedgeBorderEdges( const FDynamicMesh3& Mesh, FBevelVertex& Vertex );
        /** AppendTriangle, refusing (with Where and the result) when it fails; UE drops such a triangle and
         *  leaves a hole. Returns the new triangle ID or the failed result. */
        int32_t AppendOrRefuse( FDynamicMesh3& Mesh, int32_t A, int32_t B, int32_t C, int32_t GroupID,
                                const std::string& Where );
    };
} // namespace Desert::Geometry
