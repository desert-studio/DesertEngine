// Ported from UE 5.8
// Engine/Plugins/Runtime/GeometryProcessing/Source/DynamicMesh/Public/Operations/MeshPlaneCut.h:30-228 and
// Private/Operations/MeshPlaneCut.cpp:20-186 (ComputeVertexSignedDistances, SplitCrossingEdges), 398-428 (Cut),
// 573-599 (ExtractBoundaryLoops), 650-653 with GeometryCore/Private/Operations/LocalPlanarSimplify.cpp:517-580
// (CollapseDegenerateEdges), 719-771 (HoleFill), adapted: namespace Desert::Geometry, UE Core as std/glm, TSet ->
// std::unordered_set. HoleFill runs the PlanarHoleFiller of this port (ear clip per loop; see its header) instead
// of taking a triangulation function, and a failed fill names its reason. Not ported: CutWithoutDelete (Keep Both
// Halves: the editor puts the other half on its own entity, so it cuts a copy with the plane flipped instead of
// labelling one mesh), SplitEdgesOnly, SimpleHoleFill / MinimalHoleFill,
// TransferTriangleLabelsToHoleFillTriangles, EdgeFilterFunc and bSimplifyAlongNewEdges (FLocalPlanarSimplify) - no
// caller here sets them.
#pragma once

#include "Engine/Geometry/MeshCore/DynamicMesh/DynamicMesh3.hpp"
#include "Engine/Geometry/MeshCore/MathUtil.hpp"
#include "Engine/Geometry/MeshCore/MeshRegionBoundaryLoops.hpp"

#include <string>
#include <unordered_set>
#include <vector>

namespace Desert::Geometry
{
    /**
     * Cut the Mesh with the Plane. The *positive* side, ie (p-o).n > 0, is removed.
     * If possible, returns boundary loop(s) along the cut (this fails if the cut meets holes in the mesh).
     * Algorithm: find all edge crossings; discard triangles with all vertex distances < PlaneTolerance; split the
     * crossing edges; delete every vertex on the positive side; collapse degenerate cut edges; find the loops
     * through the cut's edges (the ones the splits made and the ones that already lay in the plane).
     */
    class MeshPlaneCut
    {
    public:
        DynamicMesh3* m_Mesh = nullptr;
        glm::dvec3    m_PlaneOrigin{ 0.0 };
        glm::dvec3    m_PlaneNormal{ 0.0, 0.0, 1.0 };

        bool m_bCollapseDegenerateEdgesOnCut = true;
        /** UVs on any hole fill surfaces are scaled by this amount */
        float  m_UVScaleFactor     = 1.0f;
        double m_DegenerateEdgeTol = ZeroTolerance<double>;
        /** Tolerance distance for considering a vertex to be 'on plane' */
        double m_PlaneTolerance = static_cast<double>( ZeroTolerance<float> ) * 10.0;

        struct OpenBoundary
        {
            float                 NormalSign = 1.0f;
            std::vector<EdgeLoop> CutLoops;
            std::vector<EdgeSpan> CutSpans;
            bool                  CutLoopsFailed = false; // the cut loops/spans could not be computed
            bool                  FoundOpenSpans = false; // the cut has open spans
        };
        std::vector<OpenBoundary> m_OpenBoundaries;
        /** Triangle IDs of hole fill triangles, 1:1 with m_OpenBoundaries */
        std::vector<std::vector<int>> m_HoleFillTriangles;
        /** Why HoleFill returned false. Empty on success. */
        std::string m_FailureReason;

        MeshPlaneCut( DynamicMesh3* MeshIn, const glm::dvec3& Origin, const glm::dvec3& Normal )
             : m_Mesh( MeshIn ), m_PlaneOrigin( Origin ), m_PlaneNormal( Normal )
        {
        }

        /** Split the edges crossing the plane, then delete the triangles on its positive side.
         *  @return false if the cut loops could not be extracted */
        bool Cut();

        /** Fill the cut loops (and, with bFillSpans, the open spans) with flat caps: one new group per boundary
         *  (or ConstantGroupID), the plane normal, planar-projected UVs on every UV layer, MaterialID if >= 0. */
        bool HoleFill( bool bFillSpans, int ConstantGroupID = -1, int MaterialID = -1 );

    private:
        void ComputeVertexSignedDistances( std::vector<double>& Signs, double InvalidDist ) const;
        void SplitCrossingEdges( bool bDeleteTrisOnPlane, std::vector<double>& Signs,
                                 std::unordered_set<int>& AlreadyOnPlaneEdges,
                                 std::unordered_set<int>& CutPlaneEdges );
        bool ExtractBoundaryLoops( const std::unordered_set<int>& OnCutEdges,
                                   const std::unordered_set<int>& ZeroEdges, OpenBoundary& Boundary ) const;
        void CollapseDegenerateEdges( std::unordered_set<int>& Edges );
    };
} // namespace Desert::Geometry
