// FDynamicMeshUVEditor::SetTriangleUVsFromExpMap (ported from UE) on a CURVED patch. The bevel regions ComputeUVs
// sees are flat or nearly so, where every vertex frame is the same and PropagateUV's rotation is the identity; on a
// cylinder the vertex frames turn around the axis, so the rotation from a neighbour's frame into the seed's frame
// carries the whole map. A cylinder is developable: the exponential map must unroll it with edge lengths kept.
#include "Engine/Geometry/UECore/DynamicMesh/DynamicMeshAttributeSet.hpp"
#include "Engine/Geometry/UECore/DynamicMesh/Parameterization/DynamicMeshUVEditor.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <numbers>

using namespace Desert::Geometry;

namespace
{
    constexpr double Radius   = 100.0;
    constexpr double Height   = 60.0;
    constexpr int    Segments = 12; // around the axis, over a quarter turn
    constexpr int    Rows     = 4;  // along the axis

    // A quarter of an open cylinder around Z, triangles wound so their normals point away from the axis.
    FDynamicMesh3 CylinderStrip()
    {
        FDynamicMesh3 mesh;
        for ( int k = 0; k <= Rows; ++k )
        {
            for ( int i = 0; i <= Segments; ++i )
            {
                const double angle = 0.5 * std::numbers::pi * i / Segments;
                mesh.AppendVertex(
                     FVector3d( Radius * std::cos( angle ), Radius * std::sin( angle ), Height * k / Rows ) );
            }
        }
        const auto id = []( int i, int k ) { return k * ( Segments + 1 ) + i; };
        for ( int k = 0; k < Rows; ++k )
        {
            for ( int i = 0; i < Segments; ++i )
            {
                mesh.AppendTriangle( id( i, k ), id( i + 1, k ), id( i + 1, k + 1 ) );
                mesh.AppendTriangle( id( i, k ), id( i + 1, k + 1 ), id( i, k + 1 ) );
            }
        }
        mesh.EnableAttributes();
        return mesh;
    }
} // namespace

TEST( DynamicMeshUVEditor, ExpMapUnrollsACylinderStripKeepingEdgeLengths )
{
    FDynamicMesh3 mesh = CylinderStrip();
    ASSERT_GE( mesh.Attributes()->NumUVLayers(), 1 );
    FDynamicMeshUVOverlay& uvs = *mesh.Attributes()->PrimaryUV();
    TArray<int32>          triangles;
    for ( const int t : mesh.TriangleIndicesItr() )
        triangles.Add( t );

    FDynamicMeshUVEditor editor( &mesh, &uvs );
    FUVEditResult        result;
    ASSERT_TRUE( editor.SetTriangleUVsFromExpMap( triangles, &result ) );
    EXPECT_EQ( result.NewUVElements.Num(), mesh.VertexCount() );

    double worst = 0.0;
    for ( const int t : triangles )
    {
        ASSERT_TRUE( uvs.IsSetTriangle( t ) ) << "triangle " << t;
        const FIndex3i vertices = mesh.GetTriangle( t );
        const FIndex3i elements = uvs.GetTriangle( t );
        for ( int j = 0; j < 3; ++j )
        {
            const double    length3d = Distance( mesh.GetVertex( vertices[j] ), mesh.GetVertex( vertices[( j + 1 ) % 3] ) );
            const FVector2f d        = uvs.GetElement( elements[( j + 1 ) % 3] ) - uvs.GetElement( elements[j] );
            const double    lengthUV = std::sqrt( static_cast<double>( d.X ) * d.X + static_cast<double>( d.Y ) * d.Y );
            worst                    = std::max( worst, std::abs( lengthUV / length3d - 1.0 ) );
        }
    }
    std::printf( "worst relative edge length error %.6f\n", worst );
    EXPECT_LT( worst, 0.05 );
}
