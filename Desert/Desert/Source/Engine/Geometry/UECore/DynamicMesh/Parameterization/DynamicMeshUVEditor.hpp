// Ported from UE 5.8 Engine/Plugins/Runtime/GeometryProcessing/Source/DynamicMesh/Public/Parameterization/
// DynamicMeshUVEditor.h, adapted: only the ExpMap path the bevel needs (ResetUVs(Triangles), TransformUVElements,
// EstimateGeodesicCenterFrameVertex, SetTriangleUVsFromExpMap) and SetTriangleUVsFromFreeBoundarySpectralConformal
// (the spectral branch of SetTriangleUVsFromConformal; the natural-conformal branch has no caller here);
// FExpMapOptions is not ported (its defaults, zero normal-smoothing rounds, are what every UE caller here passes).
#pragma once

#include "Engine/Geometry/UECore/DynamicMesh/DynamicMeshAttributeSet.hpp"
#include "Engine/Geometry/UECore/FrameTypes.hpp"

#include <functional>

namespace Desert::Geometry
{
    struct FUVEditResult
    {
        TArray<int32> NewUVElements;
    };

    class FDynamicMeshUVEditor
    {
    public:
        FDynamicMeshUVEditor( FDynamicMesh3* MeshIn, FDynamicMeshUVOverlay* UVOverlayIn )
             : Mesh( MeshIn ), UVOverlay( UVOverlayIn )
        {
        }

        void ResetUVs( const TArray<int32>& Triangles );
        void TransformUVElements( const TArray<int32>&                                ElementIDs,
                                  const std::function<FVector2f( const FVector2f& )>& TransformFunc );

        /** Frame at the vertex farthest (Dijkstra) from the longest boundary loop; false without a boundary. */
        static bool EstimateGeodesicCenterFrameVertex( const FDynamicMesh3& Mesh, FFrame3d& FrameOut,
                                                       int32& VertexIDOut, bool bAlignToUnitAxes = true );

        /**
         * New UV island for the (connected) Triangles from one discrete exponential map centred at
         * EstimateGeodesicCenterFrameVertex. False if the frame was a fallback or any triangle stayed unset.
         */
        bool SetTriangleUVsFromExpMap( const TArray<int32>& Triangles, FUVEditResult* Result = nullptr );

        /**
         * Spectral conformal UVs with a free boundary (nothing pinned; the longest boundary loop is the free set).
         * bUseExistingUVTopology keeps the triangles' current UV elements and only moves them (triangles without
         * UVs are skipped); otherwise a new island is made. False without a boundary or if the solve failed.
         */
        bool SetTriangleUVsFromFreeBoundarySpectralConformal( const TArray<int32>& Triangles,
                                                              bool                 bUseExistingUVTopology,
                                                              bool                 bPreserveIrregularity,
                                                              FUVEditResult*       Result = nullptr );

    private:
        FDynamicMesh3*         Mesh;
        FDynamicMeshUVOverlay* UVOverlay;
    };
} // namespace Desert::Geometry
