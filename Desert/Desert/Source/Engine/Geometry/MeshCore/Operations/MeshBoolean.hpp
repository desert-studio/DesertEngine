// Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Public/Operations/MeshBoolean.h:28-240 (FMeshBoolean),
// adapted: std/glm, namespace Desert::Geometry. Both meshes are given in ONE space (UE's identity-transform
// constructor): a caller with a transformed cutter bakes the transform into its copy first. Kept settings are the
// ones something here reads: Operation, SnapTolerance, DegenerateEdgeTolFactor, WindingThreshold. Left out, each a
// setting or output nothing sets or reads, so none of them would be more than a dead switch:
//  - bSimplifyAlongNewEdges and everything only it reads (SimplificationAngleTolerance,
//    TryToImproveTriQualityThreshold, bPreserve*/UV/Normal tolerances, PreserveUVsOnlyForMesh) -- off by default
//    in UE, and its LocalPlanarSimplify is not ported;
//  - bCollapseDegenerateEdgesOnCut (default true: the collapse always runs), bWeldSharedEdges (default true: the
//    seams are always welded), bPutResultInInputSpace (default true: the result is always put back, so there is
//    no ResultTransform output);
//  - bTrackAllNewEdges/AllNewEdges, bPopulateSecondMeshGroupMap/SecondMeshGroupMap, TrackPerTriangleSourceMesh;
//  - Progress/Cancelled (no cancellable caller) and Validate (always Ok).
#pragma once

#include "Engine/Geometry/MeshCore/DynamicMesh/DynamicMesh3.hpp"
#include "Engine/Geometry/MeshCore/MathUtil.hpp"
#include "Engine/Geometry/MeshCore/Spatial/FastWinding.hpp"
#include "Engine/Geometry/MeshCore/Spatial/MeshAABBTree3.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <vector>

namespace Desert::Geometry
{
    class MeshBoolean
    {
    public:
        enum class BooleanOp : uint8_t
        {
            Union,
            Difference,
            Intersect,
            TrimInside,
            TrimOutside,
            NewGroupInside,
            NewGroupOutside
        };

        std::array<const DynamicMesh3*, 2> Meshes;
        BooleanOp                          Operation;

        /** Tolerance distance for considering a point to be on a vertex or edge, in the unit-cube space the
         * boolean runs in (both meshes are centred and scaled so that their combined bounds fit a unit cube) */
        double SnapTolerance = ZeroTolerance<float> * 1.0;

        /** Edges on the cut shorter than DegenerateEdgeTolFactor * SnapTolerance are collapsed */
        double DegenerateEdgeTolFactor = 1.5;

        /** Threshold to determine whether a triangle in one mesh is inside or outside of the other */
        double WindingThreshold = .5;

        /** Output: the resulting mesh (may be Meshes[0], for an in-place boolean) */
        DynamicMesh3* Result;

        /** Output: boundary edges created by cutting the meshes and that could not be welded */
        std::vector<int> CreatedBoundaryEdges;

        MeshBoolean( const DynamicMesh3* MeshA, const DynamicMesh3* MeshB, DynamicMesh3* OutputMesh,
                     BooleanOp OperationIn )
             : Meshes{ MeshA, MeshB }, Operation( OperationIn ), Result( OutputMesh )
        {
        }

        /** @return true if the operation succeeded; false leaves the mesh valid but possibly with cracks */
        bool Compute();

        static bool OperatesOnSingleMesh( BooleanOp Op )
        {
            return Op == BooleanOp::TrimInside || Op == BooleanOp::TrimOutside ||
                   Op == BooleanOp::NewGroupInside || Op == BooleanOp::NewGroupOutside;
        }

        struct CustomInsideTestContext
        {
            DynamicMeshAABBTree3* Spatial;
            FastWindingTree*      OptionalWindingTree;
        };

        using TestInsideFunctionType = std::function<bool( const glm::dvec3&, const CustomInsideTestContext& )>;

        struct CustomInsideMeshTest
        {
            TestInsideFunctionType IsPointInsideFn;
            bool                   bRequiresWinding = true;
        };

        /** Compute with custom inside tests; InsideMeshA decides what is inside mesh A (tested against mesh B's
         * triangles) and InsideMeshB what is inside mesh B. Compute() passes the winding-number test for both. */
        bool ComputeWithCustomInside( const CustomInsideMeshTest& InsideMeshA,
                                      const CustomInsideMeshTest& InsideMeshB );

    private:
        [[nodiscard]] int FindNearestEdge( const DynamicMesh3& OnMesh, const std::vector<int>& EIDs,
                                           const glm::dvec3& Pos ) const;
        bool MergeEdges( const DynamicMesh3::AppendInfo& AppendOffsets, std::array<DynamicMesh3*, 2>& CutMesh,
                         const std::array<std::vector<int>, 2>& CutBoundaryEdges,
                         const std::map<int, int>&              AllVIDMatches );
    };
} // namespace Desert::Geometry
