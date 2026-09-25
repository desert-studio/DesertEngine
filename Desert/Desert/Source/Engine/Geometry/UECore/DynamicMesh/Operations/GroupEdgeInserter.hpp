// Ported from UE 5.8 Engine/Plugins/Runtime/GeometryProcessing/Source/DynamicMesh/Public/Operations/
// GroupEdgeInserter.h:23-160, adapted: namespace Desert::Geometry, UE Core via UECore.hpp, no FProgressCancel
// (nothing here runs on a cancellable worker). Only the PlaneCut insertion mode is ported, so EInsertionMode and
// the Mode fields are not offered: Retriangulate needs ear clipping (PolygonTriangulation), FEdgeLoop::Reverse and
// SimpleHoleFiller::UpdateAttributes, and it drops the UVs (UE's own comment), which ToRenderMesh would refuse.
// bSimplifyAlongPath is not offered either: it needs FLocalPlanarSimplify (582 lines), which is not ported.
#pragma once

#include "Engine/Geometry/UECore/UECore.hpp"

#include "Engine/Geometry/UECore/DynamicMesh/DynamicMesh3.hpp"
#include "Engine/Geometry/UECore/DynamicMesh/GroupTopology.hpp"

namespace Desert::Geometry
{
    /** Optional outputs of FGroupEdgeInserter::InsertEdgeLoops() and InsertGroupEdge(). */
    struct FGroupEdgeInserterOptionalOutputParams
    {
        /** Edge IDs of the edges composing the newly inserted group edges. */
        TSet<int32>* NewEidsOut = nullptr;

        /**
         * Any triangle IDs whose triangles were deleted or changed by the operation (but not newly
         * created tids). Useful for setting up undo.
         */
        TSet<int32>* ChangedTidsOut = nullptr;

        /**
         * In loop insertion, the group edge IDs in the original topology that surround non-quad-like
         * groups that stopped the loop.
         */
        TSet<int32>* ProblemGroupEdgeIDsOut = nullptr;
    };

    /**
     * Used to insert group edges and group edge loops, by cutting the triangles of a group with a plane.
     */
    class FGroupEdgeInserter
    {
    public:
        /** Parameters for an InsertEdgeLoops() call. */
        struct FEdgeLoopInsertionParams
        {
            /** Both of these get updated in the operation */
            FDynamicMesh3*  Mesh     = nullptr;
            FGroupTopology* Topology = nullptr;

            /** Edge loops will be inserted perpendicular to this group edge */
            int32 GroupEdgeID = IndexConstants::InvalidID;

            /**
             * Inputs can be proportions in the range (0,1), or absolute lengths.
             * As the name suggests, they must already be sorted.
             */
            const TArray<double>* SortedInputLengths    = nullptr;
            bool                  bInputsAreProportions = true;

            /**
             * One of the endpoints of the group edge, from which the arc lengths
             * or proportions should be measured
             */
            int32 StartCornerID = IndexConstants::InvalidID;

            /**
             * When inserting edges, this is the distance that a desired new point can be to
             * use a nearby vertex rather than splitting an edge to create a new one.
             */
            double VertexTolerance = 1e-4 * 10; // UE: KINDA_SMALL_NUMBER * 10
        };

        // Defined at namespace scope: a nested struct with default member initializers cannot be the type of a
        // default argument inside its enclosing class (it is not complete there), which UE works around with an
        // empty user-provided constructor.
        using FOptionalOutputParams = FGroupEdgeInserterOptionalOutputParams;

        static bool InsertEdgeLoops( const FEdgeLoopInsertionParams& Params,
                                     FOptionalOutputParams           OptionalOut = FOptionalOutputParams() );

        /** Point along a group edge that is used as a start/endpoint for an inserted group edge. */
        struct FGroupEdgeSplitPoint
        {
            /** Either vertex ID or edge ID of the point. */
            int32 ElementID = IndexConstants::InvalidID;

            /** Whether the point is an edge or vertex. */
            bool bIsVertex = true;

            /**
             * Tangent that is used to help position the cutting plane. For edges, it is just an edge vector, but
             * for vertices, it is the average of the adjacent edge vectors of the group boundary.
             */
            FVector3d Tangent = FVector3d::Zero();

            /**
             * Only relevant for edges. The range (0,1) parameter that determines where the edge
             * should be split (the order of endpoints is given by the mesh structure).
             */
            double EdgeTValue = 0;
        };

        /** Parameters for an InsertGroupEdge() call */
        struct FGroupEdgeInsertionParams
        {
            /** These are both modified in the operation */
            FDynamicMesh3*  Mesh     = nullptr;
            FGroupTopology* Topology = nullptr;

            /** Group across which the cut is inserted. */
            int32 GroupID = IndexConstants::InvalidID;

            FGroupEdgeSplitPoint StartPoint;
            FGroupEdgeSplitPoint EndPoint;

            /**
             * When inserting edges, this is the distance that a desired new point can be to
             * use a nearby vertex rather than splitting an edge to create a new one.
             */
            double VertexTolerance = 1e-4 * 10; // UE: KINDA_SMALL_NUMBER * 10
        };

        static bool InsertGroupEdge( FGroupEdgeInsertionParams& Params,
                                     FOptionalOutputParams      OptionalOut = FOptionalOutputParams() );
    };
} // namespace Desert::Geometry
