// Ported from UE 5.8
// Engine/Plugins/Runtime/GeometryProcessing/Source/DynamicMesh/Public/Operations/OffsetMeshRegion.h and
// Private/Operations/OffsetMeshRegion.cpp:28-63 (Apply), 64-342 (EdgesAreParallel, FindLoopShiftFromGroupIDs,
// LeftShiftArray, StitchRegionBorderLoopPairs_Version1), 455-512 (angle-weighted normals), 527-761
// (ApplyOffset_Version1), 1082-1108 (EdgesSeparateSameGroupsAndAreColinearAtBorder);
// PolyEditingEdgeUtil.cpp:108-170 (ComputeNewGroupIDsAlongEdgeLoop); QuadGridPatchUtil.cpp:14-110 (normals and UV
// island of a quad patch). Adapted: Version1 only (Legacy dropped) and NumSubdivisions = 0, so the FQuadGridPatch
// is one row of quads and is walked directly; CreaseAngleThresholdDeg is not ported (UE's default 180 never
// splits); bowties are refused by FMeshRegionBoundaryLoops instead of SplitBowtiesAtTriangles;
// FMeshConnectedComponents -> a flood fill over the region; ComputeMaterialIDsForVertexPath -> the material of the
// region triangle on each loop edge; UE Core via UECore.hpp; a failure names its reason in FailureReason.
#pragma once

#include "Engine/Geometry/UECore/UECore.hpp"

#include "Engine/Geometry/UECore/DynamicMesh/DynamicMesh3.hpp"
#include "Engine/Geometry/UECore/MeshRegionBoundaryLoops.hpp"

#include <string>

namespace Desert::Geometry
{
    class FOffsetMeshRegion
    {
    public:
        enum class EVertexExtrusionVectorType
        {
            Zero,
            VertexNormal,
            SelectionTriNormalsAngleWeightedAverage,
            SelectionTriNormalsAngleWeightedAdjusted,
        };

        FDynamicMesh3* Mesh;
        TArray<int32_t> Triangles;

        std::function<FVector3d( const FVector3d& Position, const FVector3d& VertexVector, int Vid )>
             OffsetPositionFunc = [this]( const FVector3d& Position, const FVector3d& VertexVector, int )
        { return Position + VertexVector * this->DefaultOffsetDistance; };
        double                                    DefaultOffsetDistance        = 1.0;
        EVertexExtrusionVectorType                ExtrusionVectorType          = EVertexExtrusionVectorType::Zero;
        std::function<bool( int32_t Eid1, int32_t Eid2 )> LoopEdgesShouldHaveSameGroup =
             [this]( int32_t Eid1, int32_t Eid2 )
        { return EdgesSeparateSameGroupsAndAreColinearAtBorder( Mesh, Eid1, Eid2, true ); };
        float  UVScaleFactor                        = 1.0f;
        bool   bOffsetFullComponentsAsSolids        = true;
        bool   bIsPositiveOffset                    = true;
        double MaxScaleForAdjustingTriNormalsOffset = 4.0;
        bool   bSingleGroupPerArea                  = true;
        bool   bUVIslandPerGroup                    = true;
        bool   bInferMaterialID                     = true;
        int    SetMaterialID                        = 0;

        struct FOffsetInfo
        {
            TArray<int32_t>         OffsetTids;
            TArray<int32_t>         OffsetGroups;
            bool                  bIsSolid = false;
            TArray<FEdgeLoop>     BaseLoops;
            TArray<FEdgeLoop>     OffsetLoops;
            TArray<TArray<int32_t>> StitchTriangles;
            TArray<TArray<int32_t>> StitchPolygonIDs;
        };
        TArray<FOffsetInfo> OffsetRegions;
        TArray<int32_t>     AllModifiedAndNewTriangles;
        // Why Apply returned false, naming the region and the step. Empty on success.
        std::string FailureReason;

        explicit FOffsetMeshRegion( FDynamicMesh3* MeshIn ) : Mesh( MeshIn )
        {
        }

        bool Apply();

        static bool EdgesSeparateSameGroupsAndAreColinearAtBorder( FDynamicMesh3* Mesh, int32_t Eid1, int32_t Eid2,
                                                                   bool bCheckColinearityAtBorder );

    protected:
        bool ApplyOffset( FOffsetInfo& Region );
    };

    // The connected components of @p Triangles (edge adjacency), each in ascending order.
    void FindConnectedTriangleComponents( const FDynamicMesh3& Mesh, const TArray<int32_t>& Triangles,
                                          TArray<TArray<int32_t>>& ComponentsOut );
    // UE ComputeNewGroupIDsAlongEdgeLoop (PolyEditingEdgeUtil.cpp:108).
    void
    ComputeNewGroupIDsAlongEdgeLoop( FDynamicMesh3& Mesh, const TArray<int32_t>& LoopEdgeIDs,
                                     TArray<int32_t>& NewLoopEdgeGroupIDs, TArray<int32_t>& NewGroupIDsOut,
                                     const std::function<bool( int32_t, int32_t )>& EdgesShouldHaveSameGroupFunc );
} // namespace Desert::Geometry
