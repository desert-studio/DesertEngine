// Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Public/DynamicMesh/Operations/SplitAttributeWelder.h:1-104
// and Private/DynamicMesh/Operations/SplitAttributeWelder.cpp:1-197, adapted: namespace Desert::Geometry, UE Core
// via UECore.hpp, the overlays are this port's (normal layers are normal, tangent, bitangent as in UE).
#pragma once

#include "Engine/Geometry/UECore/UECore.hpp"

#include "Engine/Geometry/UECore/DynamicMesh/DynamicMeshAttributeSet.hpp"

namespace Desert::Geometry
{
    /** Welds split attribute elements (split normals, UV seams...) that share one vertex of the parent mesh. */
    class SplitAttributeWelder
    {
    public:
        /** Applied as DistSquared(UVA, UVB) <= UVDistSqrdThreshold. */
        float m_UVDistSqrdThreshold = 0.f;
        /** Applied as (ColorA - ColorB).SizeSquared() <= ColorDistSqrdThreshold. */
        float m_ColorDistSqrdThreshold = 0.f;
        /** Applied as Abs(1 - VecA.dot.VecB) <= NormalVecDotThreshold. */
        float m_NormalVecDotThreshold = 0.f;
        /** Applied as Abs(1 - VecA.dot.VecB) <= TangentVecDotThreshold, to tangents and bitangents. */
        float m_TangentVecDotThreshold = 0.f;

        /** Weld the split elements at ParentVID in each overlay that are within the matching threshold. */
        void WeldSplitElements( DynamicMesh3& ParentMesh, const int32_t ParentVID );
        /** Weld split elements across the entire mesh. */
        void WeldSplitElements( DynamicMesh3& ParentMesh );

        static void WeldSplitUVs( const int32_t ParentVID, DynamicMeshUVOverlay& UVOverlay,
                                  float UVDistSqrdThreshold );
        /** Compares orientation only, not length; vectors too short to normalize weld together when
         *  bMergeZeroVectors. */
        static void WeldSplitUnitVectors( const int32_t ParentVID, DynamicMeshNormalOverlay& NormalOverlay,
                                          float DotThreshold, bool bMergeZeroVectors = true );
        static void WeldSplitColors( const int32_t ParentVID, DynamicMeshColorOverlay& ColorOverlay,
                                     float ColorDistSqrdThreshold );
    };
} // namespace Desert::Geometry
