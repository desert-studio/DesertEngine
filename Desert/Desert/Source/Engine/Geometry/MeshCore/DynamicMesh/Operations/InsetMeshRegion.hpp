// Ported from UE 5.8
// Engine/Plugins/Runtime/GeometryProcessing/Source/DynamicMesh/Public/Operations/InsetMeshRegion.h and
// Private/Operations/InsetMeshRegion.cpp:26-378 (the inset lines and their solve live in PolyEditingEdgeUtil.hpp).
// Adapted: the interior solve (ConstrainedMeshDeformer + AABB reprojection, run when the region has interior
// vertices or Softness > 0) is not ported - such a region is REFUSED with a named reason before the mesh is
// touched, and Softness / AreaCorrection / bReproject / bSolveRegionInteriors are therefore absent; bowties are
// refused by MeshRegionBoundaryLoops instead of SplitBowtiesAtTriangles; DistLine3Line3d is the closed-form
// closest points of two lines; Frame3d::ConstrainedAlignAxis is written out as its two axes; UE Core as
// std/glm.
#pragma once


#include "Engine/Geometry/MeshCore/DynamicMesh/DynamicMesh3.hpp"
#include "Engine/Geometry/MeshCore/MeshRegionBoundaryLoops.hpp"

#include <string>

namespace Desert::Geometry
{
    class InsetMeshRegion
    {
    public:
        DynamicMesh3*        m_Mesh;
        std::vector<int32_t> m_Triangles;
        double               m_InsetDistance = 1.0;
        float                m_UVScaleFactor = 1.0f;

        struct InsetInfo
        {
            std::vector<int32_t>          InitialTriangles;
            std::vector<EdgeLoop>         BaseLoops;
            std::vector<EdgeLoop>         InsetLoops;
            std::vector<std::vector<int>> StitchTriangles;
            std::vector<std::vector<int>> StitchPolygonIDs;
        };
        std::vector<InsetInfo> m_InsetRegions;
        std::vector<int32_t>   m_AllModifiedTriangles;
        std::string            m_FailureReason;

        explicit InsetMeshRegion( DynamicMesh3* MeshIn ) : m_Mesh( MeshIn )
        {
        }

        bool Apply();

    protected:
        bool ApplyInset( InsetInfo& Region );
    };
} // namespace Desert::Geometry
