// Ported from UE 5.8 Engine/Plugins/Runtime/GeometryProcessing/Source/DynamicMesh/Public/Operations/
// PolyEditingUVUtil.h:26, adapted: returns SetTriangleUVsFromExpMap's result (UE's is void and drops it).
#pragma once

#include "Engine/Geometry/UECore/DynamicMesh/DynamicMeshAttributeSet.hpp"

namespace Desert::Geometry
{
    /**
     * ExpMap UVs for an arbitrary connected TriangleSet, scaled so their UV/3D density matches the already-set
     * neighbour triangles. False when the ExpMap reports a failure.
     */
    bool ComputeArbitraryTrianglePatchUVs( DynamicMesh3& Mesh, FDynamicMeshUVOverlay& UVOverlay,
                                           const std::vector<int32_t>& TriangleSet );
} // namespace Desert::Geometry
