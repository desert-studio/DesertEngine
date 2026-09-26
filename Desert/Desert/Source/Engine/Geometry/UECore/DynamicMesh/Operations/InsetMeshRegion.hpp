// Ported from UE 5.8
// Engine/Plugins/Runtime/GeometryProcessing/Source/DynamicMesh/Public/Operations/InsetMeshRegion.h and
// Private/Operations/InsetMeshRegion.cpp:26-378 (the inset lines and their solve live in PolyEditingEdgeUtil.hpp).
// Adapted: the interior solve (ConstrainedMeshDeformer + AABB reprojection, run when the region has interior
// vertices or Softness > 0) is not ported - such a region is REFUSED with a named reason before the mesh is
// touched, and Softness / AreaCorrection / bReproject / bSolveRegionInteriors are therefore absent; bowties are
// refused by FMeshRegionBoundaryLoops instead of SplitBowtiesAtTriangles; FDistLine3Line3d is the closed-form
// closest points of two lines; FFrame3d::ConstrainedAlignAxis is written out as its two axes; UE Core via
// UECore.hpp.
#pragma once

#include "Engine/Geometry/UECore/UECore.hpp"

#include "Engine/Geometry/UECore/DynamicMesh/DynamicMesh3.hpp"
#include "Engine/Geometry/UECore/MeshRegionBoundaryLoops.hpp"

#include <string>

namespace Desert::Geometry
{
    class FInsetMeshRegion
    {
    public:
        FDynamicMesh3* Mesh;
        std::vector<int32_t> Triangles;
        double         InsetDistance = 1.0;
        float          UVScaleFactor = 1.0f;

        struct FInsetInfo
        {
            std::vector<int32_t>          InitialTriangles;
            std::vector<FEdgeLoop>        BaseLoops;
            std::vector<FEdgeLoop>        InsetLoops;
            std::vector<std::vector<int>> StitchTriangles;
            std::vector<std::vector<int>> StitchPolygonIDs;
        };
        std::vector<FInsetInfo> InsetRegions;
        std::vector<int32_t>    AllModifiedTriangles;
        std::string        FailureReason;

        explicit FInsetMeshRegion( FDynamicMesh3* MeshIn ) : Mesh( MeshIn )
        {
        }

        bool Apply();

    protected:
        bool ApplyInset( FInsetInfo& Region );
    };
} // namespace Desert::Geometry
