// Ported from UE 5.8 Engine/Plugins/Runtime/GeometryProcessing/Source/DynamicMesh/Public/Operations/
// SimpleHoleFiller.h:1-119 and Private/Operations/SimpleHoleFiller.cpp:9-80 (Fill, Fill_Fan), adapted: namespace
// Desert::Geometry, FEdgeLoop carries no mesh pointer, failures carry a reason. Only the TriangleFan fill is
// ported: PolygonEarClipping needs PolygonTriangulation::TriangulateSimplePolygon, which is not ported, so the
// fill type is not offered. UpdateAttributes (per-vertex UV maps) has no caller: UE's HoleFillOp sets normals and
// UVs through FDynamicMeshEditor instead. FillColors (HoleFillUtil::FillColorOverlay) is not ported: a mesh with a
// primary colour layer is refused by name, never left with unset colour triangles.
#pragma once

#include "Engine/Geometry/UECore/DynamicMesh/Operations/HoleFiller.hpp"
#include "Engine/Geometry/UECore/MeshRegionBoundaryLoops.hpp"

#include <string>
#include <utility>

namespace Desert::Geometry
{
    /** Fills an FEdgeLoop hole with a triangle fan around a new centroid vertex. */
    class FSimpleHoleFiller : public IHoleFiller
    {
    public:
        FDynamicMesh3* Mesh = nullptr;
        FEdgeLoop      Loop;

        int NewVertex = IndexConstants::InvalidID;
        /** Why Fill returned false. Empty on success. */
        std::string FailureReason;

        FSimpleHoleFiller( FDynamicMesh3* MeshIn, FEdgeLoop LoopIn ) : Mesh( MeshIn ), Loop( std::move( LoopIn ) )
        {
        }

        bool Fill( int GroupID = -1 ) override;

    protected:
        bool Fill_Fan( int NewGroupID );
    };
} // namespace Desert::Geometry
