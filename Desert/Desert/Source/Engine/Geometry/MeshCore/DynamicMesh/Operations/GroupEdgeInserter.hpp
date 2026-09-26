// Ported from UE 5.8 Engine/Plugins/Runtime/GeometryProcessing/Source/DynamicMesh/Public/Operations/
// GroupEdgeInserter.h:23-160, adapted: namespace Desert::Geometry, UE Core as std/glm, no FProgressCancel
// (nothing here runs on a cancellable worker). Only the PlaneCut insertion mode is ported, so EInsertionMode and
// the Mode fields are not offered: Retriangulate needs ear clipping (PolygonTriangulation), EdgeLoop::Reverse and
// SimpleHoleFiller::UpdateAttributes, and it drops the UVs (UE's own comment), which ToRenderMesh would refuse.
// bSimplifyAlongPath is not offered either: it needs FLocalPlanarSimplify (582 lines), which is not ported.
#pragma once


#include "Engine/Geometry/MeshCore/DynamicMesh/DynamicMesh3.hpp"
#include "Engine/Geometry/MeshCore/DynamicMesh/GroupTopology.hpp"

#include <unordered_set>

namespace Desert::Geometry
{
    /** Optional outputs of GroupEdgeInserter::InsertEdgeLoops() and InsertGroupEdge(). */
    struct GroupEdgeInserterOptionalOutputParams
    {
        /** Edge IDs of the edges composing the newly inserted group edges. */
        std::unordered_set<int32_t>* NewEidsOut = nullptr;

        /**
         * Any triangle IDs whose triangles were deleted or changed by the operation (but not newly
         * created tids). Useful for setting up undo.
         */
        std::unordered_set<int32_t>* ChangedTidsOut = nullptr;

        /**
         * In loop insertion, the group edge IDs in the original topology that surround non-quad-like
         * groups that stopped the loop.
         */
        std::unordered_set<int32_t>* ProblemGroupEdgeIDsOut = nullptr;
    };

    /**
     * Used to insert group edges and group edge loops, by cutting the triangles of a group with a plane.
     */
    class GroupEdgeInserter
    {
    public:
        /** Parameters for an InsertEdgeLoops() call. */
        struct EdgeLoopInsertionParams
        {
            /** Both of these get updated in the operation */
            DynamicMesh3*  Mesh     = nullptr;
            GroupTopology* Topology = nullptr;

            /** Edge loops will be inserted perpendicular to this group edge */
            int32_t GroupEdgeID = IndexConstants::InvalidID;

            /**
             * Inputs can be proportions in the range (0,1), or absolute lengths.
             * As the name suggests, they must already be sorted.
             */
            const std::vector<double>* SortedInputLengths    = nullptr;
            bool                  bInputsAreProportions = true;

            /**
             * One of the endpoints of the group edge, from which the arc lengths
             * or proportions should be measured
             */
            int32_t StartCornerID = IndexConstants::InvalidID;

            /**
             * When inserting edges, this is the distance that a desired new point can be to
             * use a nearby vertex rather than splitting an edge to create a new one.
             */
            double VertexTolerance = 1e-4 * 10; // UE: KINDA_SMALL_NUMBER * 10
        };

        // Defined at namespace scope: a nested struct with default member initializers cannot be the type of a
        // default argument inside its enclosing class (it is not complete there), which UE works around with an
        // empty user-provided constructor.
        using OptionalOutputParams = GroupEdgeInserterOptionalOutputParams;

        static bool InsertEdgeLoops( const EdgeLoopInsertionParams& Params,
                                     OptionalOutputParams           OptionalOut = OptionalOutputParams() );

        /** Point along a group edge that is used as a start/endpoint for an inserted group edge. */
        struct GroupEdgeSplitPoint
        {
            /** Either vertex ID or edge ID of the point. */
            int32_t ElementID = IndexConstants::InvalidID;

            /** Whether the point is an edge or vertex. */
            bool bIsVertex = true;

            /**
             * Tangent that is used to help position the cutting plane. For edges, it is just an edge vector, but
             * for vertices, it is the average of the adjacent edge vectors of the group boundary.
             */
            glm::dvec3 Tangent = glm::dvec3( 0 );

            /**
             * Only relevant for edges. The range (0,1) parameter that determines where the edge
             * should be split (the order of endpoints is given by the mesh structure).
             */
            double EdgeTValue = 0;
        };

        /** Parameters for an InsertGroupEdge() call */
        struct GroupEdgeInsertionParams
        {
            /** These are both modified in the operation */
            DynamicMesh3*  Mesh     = nullptr;
            GroupTopology* Topology = nullptr;

            /** Group across which the cut is inserted. */
            int32_t GroupID = IndexConstants::InvalidID;

            GroupEdgeSplitPoint StartPoint;
            GroupEdgeSplitPoint EndPoint;

            /**
             * When inserting edges, this is the distance that a desired new point can be to
             * use a nearby vertex rather than splitting an edge to create a new one.
             */
            double VertexTolerance = 1e-4 * 10; // UE: KINDA_SMALL_NUMBER * 10
        };

        static bool InsertGroupEdge( GroupEdgeInsertionParams& Params,
                                     OptionalOutputParams      OptionalOut = OptionalOutputParams() );
    };
} // namespace Desert::Geometry
