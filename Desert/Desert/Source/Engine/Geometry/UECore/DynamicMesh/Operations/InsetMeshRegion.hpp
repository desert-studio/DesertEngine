// Ported from UE 5.8
// Engine/Plugins/Runtime/GeometryProcessing/Source/DynamicMesh/Public/Operations/InsetMeshRegion.h and
// Private/Operations/InsetMeshRegion.cpp:26-378 (the inset lines and their solve live in PolyEditingEdgeUtil.hpp).
// Adapted: the interior solve (ConstrainedMeshDeformer + AABB reprojection, run when the region has interior
// vertices or Softness > 0) is not ported - such a region is REFUSED with a named reason before the mesh is
// touched, and Softness / AreaCorrection / bReproject / bSolveRegionInteriors are therefore absent; bowties are
// refused by MeshRegionBoundaryLoops instead of SplitBowtiesAtTriangles; DistLine3Line3d is the closed-form
// closest points of two lines; Frame3d::ConstrainedAlignAxis is written out as its two axes; UE Core via
// UECore.hpp.
#pragma once

#include "Engine/Geometry/UECore/UECore.hpp"

#include "Engine/Geometry/UECore/DynamicMesh/DynamicMesh3.hpp"
#include "Engine/Geometry/UECore/MeshRegionBoundaryLoops.hpp"

#include <string>

namespace Desert::Geometry
{
    class InsetMeshRegion
    {
    public:
        DynamicMesh3*        Mesh;
        std::vector<int32_t> Triangles;
        double         InsetDistance = 1.0;
        float          UVScaleFactor = 1.0f;

        struct InsetInfo
        {
            std::vector<int32_t>          InitialTriangles;
            std::vector<EdgeLoop>         BaseLoops;
            std::vector<EdgeLoop>         InsetLoops;
            std::vector<std::vector<int>> StitchTriangles;
            std::vector<std::vector<int>> StitchPolygonIDs;
        };
        std::vector<InsetInfo>  InsetRegions;
        std::vector<int32_t>    AllModifiedTriangles;
        std::string        FailureReason;

        explicit InsetMeshRegion( DynamicMesh3* MeshIn ) : Mesh( MeshIn )
        {
        }

        bool Apply();

    protected:
        bool ApplyInset( InsetInfo& Region );
    };
} // namespace Desert::Geometry
