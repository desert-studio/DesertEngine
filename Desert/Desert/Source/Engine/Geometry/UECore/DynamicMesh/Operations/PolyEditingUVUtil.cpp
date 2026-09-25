// Ported from UE 5.8 Engine/Plugins/Runtime/GeometryProcessing/Source/DynamicMesh/Private/Operations/
// PolyEditingUVUtil.cpp:11-50, adapted: see the header; the 2D triangle area is written out.
#include "Engine/Geometry/UECore/DynamicMesh/Operations/PolyEditingUVUtil.hpp"

#include "Engine/Geometry/UECore/DynamicMesh/Parameterization/DynamicMeshUVEditor.hpp"
#include "Engine/Geometry/UECore/VectorUtil.hpp"

#include <cmath>

namespace Desert::Geometry
{
    bool ComputeArbitraryTrianglePatchUVs( FDynamicMesh3& Mesh, FDynamicMeshUVOverlay& UVOverlay,
                                           const TArray<int32>& TriangleSet )
    {
        TArray<int32> NbrTriSet;
        double        NbrUVAreaSum = 0.0;
        double        Nbr3DAreaSum = 0.0;
        for ( const int32 tid : TriangleSet )
        {
            const FIndex3i NbrTris = Mesh.GetTriNeighbourTris( tid );
            for ( int32 j = 0; j < 3; ++j )
            {
                if ( NbrTris[j] == FDynamicMesh3::InvalidID || NbrTriSet.Contains( NbrTris[j] ) )
                    continue;
                NbrTriSet.Add( NbrTris[j] );
                if ( !UVOverlay.IsSetTriangle( NbrTris[j] ) )
                    continue;
                FVector3d A, B, C;
                Mesh.GetTriVertices( NbrTris[j], A, B, C );
                Nbr3DAreaSum += VectorUtil::Area( A, B, C );
                FVector2f U, V, W;
                UVOverlay.GetTriElements( NbrTris[j], U, V, W );
                NbrUVAreaSum += 0.5 * std::abs( (double)( V.X - U.X ) * ( W.Y - U.Y ) -
                                                (double)( V.Y - U.Y ) * ( W.X - U.X ) );
            }
        }
        const double UseUVScale = TMathUtil<double>::Max( std::sqrt( NbrUVAreaSum ), 0.0001 ) /
                                  TMathUtil<double>::Max( std::sqrt( Nbr3DAreaSum ), 0.0001 );

        FDynamicMeshUVEditor UVEditor( &Mesh, &UVOverlay );
        FUVEditResult        UVEditResult;
        const bool           bOK = UVEditor.SetTriangleUVsFromExpMap( TriangleSet, &UVEditResult );
        UVEditor.TransformUVElements( UVEditResult.NewUVElements,
                                      [UseUVScale]( const FVector2f& UV ) { return UV * (float)UseUVScale; } );
        return bOK;
    }
} // namespace Desert::Geometry
