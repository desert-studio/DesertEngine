// Ported from UE 5.8
// Engine/Plugins/Runtime/GeometryProcessing/Source/DynamicMesh/Public/Operations/MeshBevel.h:26-378 and
// Private/Operations/MeshBevel.cpp:47-131,576-668,669-2082,2084-3740,3814-3969 (setup, topology build,
// unlink, displacement, meshing of the chamfer and of the multi-segment bevel, the round profile, Apply, normals,
// material IDs), adapted: UE Core as std/glm, namespace Desert::Geometry. FGeometryResult / FProgressCancel
// are replaced by a named FailureReason, and a vertex UE would leave as BevelVertexType::Unknown (silently not
// beveled) is REFUSED with the vertex and the cause; bowtie vertices on the bevel graph are refused up front
// instead of FixBowties' SplitBowties (B:415-574). The fields UE marks deprecated in 5.5 (GroupEdgeID, GroupIDs,
// CornerID, IncomingBevelTopoEdges) are not ported. The round profile (RoundWeight, MakeArcSplineCurve,
// ApplyProfileShape_Round B:3241-3739) is ported for loops, edges and every junction patch: 4-sided (blended
// border curves), 3-sided (PN triangle; its barycentrics come from the tessellation lattice instead of UE's vertex
// colours) and 5+-sided (mean-value coordinates over the spectral-conformal flattening, with DeformNormals; UE
// also sums DeformNormals along loops, which no junction reads, so that is not ported). FInterpCurveVector becomes
// the two-point cubic Hermite ArcSplineCurve evaluated in double (UE evaluates at a float parameter); a strip
// column takes its NormalsA/B by its end vertices rather than by UE's column index, and a missing border curve is
// a refusal, not UE's ensure.
#pragma once


#include "Engine/Geometry/MeshCore/DynamicMesh/DynamicMesh3.hpp"
#include "Engine/Geometry/MeshCore/DynamicMesh/GroupTopology.hpp"
#include "Engine/Geometry/MeshCore/MeshRegionBoundaryLoops.hpp"
#include "Engine/Geometry/MeshCore/Selections/QuadGridPatch.hpp"

#include <string>
#include <unordered_map>

namespace Desert::Geometry
{
    /**
     * Bevel of mesh edges, applied in place by unstitching the edges and inserting new triangles (UE MeshBevel).
     * An isolated closed loop of edges becomes a quad strip; open edge spans meeting at a vertex of valence >= 3
     * replace that vertex by a polygon, a span ending at a vertex of valence 1 is terminated by expanding the
     * vertex into an edge.
     */
    class MeshBevel
    {
    public:
        /** Why initialization refused the input; empty on success. */
        std::string m_FailureReason;

        /** Distance (cm) each beveled edge is inset into its two adjacent faces. */
        double m_InsetDistance = 5.0;

        /** Number of subdivisions inserted in each bevel strip; 0 is the one-segment chamfer. */
        int32_t m_NumSubdivisions = 0;

        /**
         * "Roundness" of the bevel profile, ignored when NumSubdivisions = 0. 1 approximates a circular arc
         * tangent to both faces, larger values pull towards a sharper crease, negative values give an inverted
         * arc. DEFAULT 0 (UE's default is 1.0): the flat profile, every subdivision in the chamfer plane, so
         * callers written before the round profile existed keep their output bit for bit.
         */
        double m_RoundWeight = 0.0;

        /** Options for MaterialID assignment on the new triangles generated for the bevel */
        enum class MaterialIDMode
        {
            ConstantMaterialID,
            InferMaterialID,
            InferMaterialID_ConstantIfAmbiguous
        };
        /** Which MaterialID assignment mode to use */
        MaterialIDMode m_MaterialIDMode = MaterialIDMode::ConstantMaterialID;
        /** Constant MaterialID used for various MaterialIDMode settings */
        int32_t m_SetConstantMaterialID = 0;

        /** Triangles created by Apply: the vertex polygons, then the edge and loop strips. */
        std::vector<int32_t> m_NewTriangles;

        /** Initialize the bevel with all edges of the given GroupTopology. */
        bool InitializeFromGroupTopology( const DynamicMesh3& Mesh, const GroupTopology& Topology );
        /** Initialize the bevel with the specified edges of a GroupTopology. */
        bool InitializeFromGroupTopologyEdges( const DynamicMesh3& Mesh, const GroupTopology& Topology,
                                               const std::vector<int32_t>& GroupEdges );

        /**
         * Bevel the initialized edges in place (UE Apply, B:576): unlink, displace, mesh, then the primary normals
         * normals, primary UVs (one ExpMap island per region, as UE: other UV layers stay unset) and material
         * IDs of NewTriangles. Returns false with FailureReason (the mesh is then partially edited) as soon as a
         * phase refuses. NumSubdivisions > 0 meshes through CreateBevelMeshing_Multi, which then applies the
         * round profile when RoundWeight != 0.
         */
        bool Apply( DynamicMesh3& Mesh );

        struct BevelLoop
        {
            std::vector<int32_t> MeshVertices; // sequential list of mesh vertex IDs along edge loop
            std::vector<int32_t> MeshEdges;    // sequential list of mesh edge IDs along edge loop
            std::vector<Index2i> MeshEdgeTris; // the one or two triangles of each MeshEdges element in the input
            std::vector<glm::dvec3> InitialPositions; // initial vertex positions
            std::vector<int32_t>
                 NewMeshVertices; // vertices on the "other" side of the unlinked edge, 1-1 w/ MeshVertices
            std::vector<int32_t>
                 NewMeshEdges; // edges on the "other" side of the unlinked edge, 1-1 with MeshEdges
            std::vector<glm::dvec3> NewPositions0; // new positions for MeshVertices
            std::vector<glm::dvec3> NewPositions1; // new positions for NewMeshVertices
            std::vector<int32_t>    NewGroupIDs;
            std::vector<Index2i>
                          StripQuads;     // triangle-ID pairs of the new quads (1-1 with MeshEdges in the chamfer)
            QuadGridPatch StripQuadPatch; // only initialized in multi-segment bevel
            // normals at NewPositions0 / NewPositions1 before the strips are added (the arc's tangent-plane
            // boundary condition); only filled for a round profile
            std::vector<glm::dvec3> NormalsA, NormalsB;
        };

        struct BevelEdge
        {
            int32_t           EdgeIndex = -1; // index of this BevelEdge in Edges
            std::vector<int32_t> MeshVertices;   // sequential list of mesh vertex IDs along edge
            std::vector<int32_t> MeshEdges;      // sequential list of mesh edge IDs along edge
            std::vector<Index2i> MeshEdgeTris;   // the one or two triangles of each MeshEdges element in the input
            bool             bEndpointBoundaryFlag[2] = { false, false }; // start/end vertex was a boundary vertex
            std::vector<glm::dvec3> InitialPositions;                         // initial vertex positions
            Index2i                 BevelVertices; // indices of the Bevel Vertices at either end of the Bevel Edge
            std::vector<int32_t>
                 NewMeshVertices; // vertices on the "other" side of the unlinked edge, 1-1 w/ MeshVertices
            std::vector<int32_t>
                 NewMeshEdges; // edges on the "other" side of the unlinked edge, 1-1 with MeshEdges
            std::vector<glm::dvec3> NewPositions0; // new positions for MeshVertices
            std::vector<glm::dvec3> NewPositions1; // new positions for NewMeshVertices
            int32_t           NewGroupID = -1;
            std::vector<Index2i>
                          StripQuads;     // triangle-ID pairs of the new quads (1-1 with MeshEdges in the chamfer)
            QuadGridPatch StripQuadPatch; // only initialized in multi-segment bevel
            // normals at NewPositions0 / NewPositions1 before the strips are added (the arc's tangent-plane
            // boundary condition); only filled for a round profile
            std::vector<glm::dvec3> NormalsA, NormalsB;
        };

        struct OneRingWedge
        {
            std::vector<int32_t> Triangles;   // sequential triangles in this wedge
            Index2i              BorderEdges; // first and last edges of Triangles (connected to the vertex)
            Index2i         BorderEdgeTriEdgeIndices; // index 0/1/2 of BorderEdges[j] in the start/end Triangles
            int32_t         WedgeVertex = -1; // central vertex of this wedge (updated by the unlink functions)
            glm::dvec3      NewPosition{};    // new calculated position for the vertex of this wedge
            bool            bHaveNewPosition = false; // NewPosition is valid
        };

        enum class BevelVertexType
        {
            JunctionVertex,
            TerminatorVertex,
            BoundaryVertex,
            Unknown
        };

        /** A vertex added inside a junction polygon, with its coordinates in the polygon's frame. */
        struct BevelVertex_InteriorVertex
        {
            int32_t VertexID = -1;
            // 4-sided patch: one (tx, ty, 0), the vertex's grid coordinates in the planar quad; 3-sided: one
            // (w, u, v), its barycentrics of InteriorBorderLoop[0..2]; 5+-sided: one per InteriorBorderLoop
            // vertex, (DeltaX, DeltaY, MVC weight): its flattened offset in that border vertex's frame and its
            // normalized mean-value coordinate
            std::vector<glm::dvec3> BorderFrameWeight;
        };

        struct BevelVertex
        {
            int32_t          VertexID   = -1; // initial mesh vertex ID of the Bevel Vertex
            BevelVertexType  VertexType = BevelVertexType::Unknown;
            std::vector<int32_t>
                 IncomingBevelMeshEdges; // (unsorted) mesh edges to be beveled, coming into the vertex
            std::vector<int32_t>
                 IncomingBevelEdgeIndices;        // (unsorted) indices of BevelEdge coming into the vertex
            std::vector<int32_t> SortedTriangles; // ordered triangle one-ring around VertexID
            std::vector<OneRingWedge>
                                 Wedges; // ordered one-ring decomposition into wedges between incoming bevel edges
            int32_t         NewGroupID = -1;   // polygroup of the polygon generated by this vertex (NumEdges > 2)
            std::vector<int32_t> NewTriangles; // triangles of the polygon generated by this vertex (NumEdges > 2)
            Index2i              TerminatorInfo; // TerminatorVertex: [EdgeID, FarVertexID] in the one-ring
            int32_t ConnectedBevelVertex = -1; // another BevelVertex index TerminatorInfo's edge directly reaches
            // multi-segment junction polygon: its added interior vertices, and its corners in the order c00, c10,
            // c01, c11 (quad) or in wedge order (triangle), or for 5+ corners (round profile only) its whole
            // border loop; read by the round profile
            std::vector<BevelVertex_InteriorVertex> InteriorVertices;
            std::vector<int32_t>                    InteriorBorderLoop;
        };

    protected:
        std::unordered_map<int32_t, int32_t> m_VertexIDToIndexMap; // mesh vertex ID -> index into Vertices
        std::vector<BevelVertex>             m_Vertices;           // mesh vertices that need beveling
        std::vector<BevelEdge>               m_Edges;              // mesh edge spans that need beveling
        std::vector<BevelLoop>               m_Loops;              // mesh edge loops that need beveling
        std::unordered_map<int32_t, int32_t>
             m_MeshEdgePairs; // input edges split into edge pairs, later stitched with quads

        BevelVertex* GetBevelVertexFromVertexID( int32_t VertexID, int32_t* IndexOut = nullptr );

        void AddBevelGroupEdge( const DynamicMesh3& Mesh, const GroupTopology& Topology, int32_t GroupEdgeID );
        void AddBevelEdgeLoop( const DynamicMesh3& Mesh, const EdgeLoop& Loop );
        void BuildVertexSets( const DynamicMesh3& Mesh );
        void BuildJunctionVertex( BevelVertex& Vertex, const DynamicMesh3& Mesh );
        void BuildTerminatorVertex( BevelVertex& Vertex, const DynamicMesh3& Mesh );

        // Unlink: split the bevel edges open; afterwards each MeshEdges[i] and NewMeshEdges[i] is a boundary edge
        // pair recorded in MeshEdgePairs. UE's FDynamicMeshChangeTracker parameter is dropped (not ported).
        void UnlinkEdges( DynamicMesh3& Mesh );
        void UnlinkBevelEdgeInterior( DynamicMesh3& Mesh, BevelEdge& BevelEdge );
        void UnlinkLoops( DynamicMesh3& Mesh );
        void UnlinkBevelLoop( DynamicMesh3& Mesh, BevelLoop& BevelLoop );
        void UnlinkVertices( DynamicMesh3& Mesh );
        void UnlinkJunctionVertex( DynamicMesh3& Mesh, BevelVertex& Vertex );
        void UnlinkTerminatorVertex( DynamicMesh3& Mesh, BevelVertex& BevelVertex );
        void FixUpUnlinkedBevelEdges( const DynamicMesh3& Mesh );

        /** Move the unlinked vertices InsetDistance into the adjacent faces (UE's Distance argument only fed its
         *  unused MeanValueCentroid fallback, so it is gone). */
        void DisplaceVertices( DynamicMesh3& Mesh );

        /** Fill the unlinked holes: junction polygons, edge and loop quad strips, then the terminator triangles
         *  (last, so the strip's end edge orients them). */
        void CreateBevelMeshing( DynamicMesh3& Mesh );
        void AppendJunctionVertexPolygon( DynamicMesh3& Mesh, BevelVertex& Vertex );
        void AppendTerminatorVertexTriangle( DynamicMesh3& Mesh, BevelVertex& Vertex );
        void AppendTerminatorVertexPairQuad( DynamicMesh3& Mesh, BevelVertex& Vertex0, BevelVertex& Vertex1 );
        void AppendEdgeQuads( DynamicMesh3& Mesh, BevelEdge& Edge );
        void AppendLoopQuads( DynamicMesh3& Mesh, BevelLoop& Loop );

        /** Multi-segment meshing: the edge and loop strips first (NumSubdivisions + 1 quad rows each, kept as a
         *  StripQuadPatch), then the junction polygons tessellated to match their columns, terminators last. */
        void CreateBevelMeshing_Multi( DynamicMesh3& Mesh );
        void AppendEdgeQuads_Multi( DynamicMesh3& Mesh, BevelEdge& Edge );
        void AppendLoopQuads_Multi( DynamicMesh3& Mesh, BevelLoop& Loop );
        void AppendJunctionVertexPolygon_Multi( DynamicMesh3& Mesh, BevelVertex& Vertex );
        void AppendTerminatorVertexTriangles_Multi( DynamicMesh3& Mesh, BevelVertex& Vertex );
        void AppendTerminatorVertexPairQuad_Multi( DynamicMesh3& Mesh, BevelVertex& Vertex0,
                                                   BevelVertex& Vertex1 );

        /** UE's FInterpCurveVector with two CIM_CurveUser points at parameters 0 and 1: a cubic Hermite. */
        struct ArcSplineCurve
        {
            glm::dvec3               Pos0{}, Pos1{};         // points at T = 0 and T = 1
            glm::dvec3               Tangent0{}, Tangent1{}; // their (arrive == leave) tangents
            [[nodiscard]] glm::dvec3 Eval( double T ) const;
        };
        /** Hermite approximation of the arc from PosA to PosB tangent to the planes (PosA, NormalA) and (PosB,
         *  NormalB); tangents scaled by |RoundWeight| * sqrt(2), and swapped for a negative RoundWeight. */
        [[nodiscard]] ArcSplineCurve MakeArcSplineCurve( const glm::dvec3& PosA, const glm::dvec3& NormalA,
                                                         const glm::dvec3& PosB, const glm::dvec3& NormalB ) const;
        /** Deform the finished multi-segment topology into the round profile: each strip column onto its arc,
         *  then each 4-sided junction patch by blending its four border arcs, each 3-sided one as the PN
         *  triangle of its three and each larger one by mean-value blending of its border frames. */
        void ApplyProfileShape_Round( DynamicMesh3& Mesh );
        /** RoundWeight is non-zero beyond the tolerance (the profile is flat otherwise). */
        [[nodiscard]] bool HasRoundProfile() const;

        /** Per-vertex normals within each new vertex polygon and each strip; no-op without attributes. */
        void ComputeNormals( DynamicMesh3& Mesh );
        /** ExpMap primary UVs per strip, then per vertex polygon; no-op without a UV layer. */
        void ComputeUVs( DynamicMesh3& Mesh );
        /** MaterialID of NewTriangles per MaterialIDMode; no-op without a MaterialID attribute. */
        void ComputeMaterialIDs( DynamicMesh3& Mesh );

    private:
        void InitVertexSet( const DynamicMesh3& Mesh, BevelVertex& Vertex );
        void FinalizeTerminatorVertex( const DynamicMesh3& Mesh, BevelVertex& Vertex );
        /** Records the first refusal; later ones are consequences of the same input. */
        void Refuse( const std::string& Reason );
        bool RefuseBowties( const DynamicMesh3& Mesh, const std::vector<int32_t>& MeshVertices );
        /** SplitVertex, refusing (with Where and the result) when it fails; NewVertexOut = VertexID then. */
        bool SplitOrKeep( DynamicMesh3& Mesh, int32_t VertexID, const std::vector<int32_t>& Triangles,
                          const std::string& Where, int32_t& NewVertexOut );
        void PairSplitWedgeBorderEdges( const DynamicMesh3& Mesh, BevelVertex& Vertex );
        /** AppendTriangle, refusing (with Where and the result) when it fails; UE drops such a triangle and
         *  leaves a hole. Returns the new triangle ID or the failed result. */
        int32_t AppendOrRefuse( DynamicMesh3& Mesh, int32_t A, int32_t B, int32_t C, int32_t GroupID,
                                const std::string& Where );
    };
} // namespace Desert::Geometry
