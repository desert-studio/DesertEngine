// FDynamicMeshUVEditor::SetTriangleUVsFromFreeBoundarySpectralConformal (ported from UE with its solver). A planar
// region has a zero-energy conformal map, a similarity of itself: every UV angle must equal its 3D angle, and the
// square's corners must stay right angles (a boundary pinned to a circle would open them to 180 degrees). A
// spherical cap is not developable: angles move, but no triangle may flip or collapse.
#include "Engine/Geometry/UECore/DynamicMesh/DynamicMeshAttributeSet.hpp"
#include "Engine/Geometry/UECore/DynamicMesh/Parameterization/DynamicMeshUVEditor.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <numbers>

using namespace Desert::Geometry;

namespace
{
    constexpr int Cells = 8;

    // (Cells+1)^2 grid over a 100 cm square, interior vertices jittered so the triangles are irregular; Bend lifts
    // it onto a sphere of that radius (0 = flat).
    FDynamicMesh3 Grid( double Bend )
    {
        FDynamicMesh3 mesh;
        for ( int k = 0; k <= Cells; ++k )
        {
            for ( int i = 0; i <= Cells; ++i )
            {
                double x = 100.0 * i / Cells - 50.0;
                double y = 100.0 * k / Cells - 50.0;
                if ( i > 0 && i < Cells && k > 0 && k < Cells )
                {
                    x += 3.0 * std::sin( 1.7 * i + 2.3 * k );
                    y += 3.0 * std::cos( 2.9 * i - 1.1 * k );
                }
                const double z = Bend > 0.0 ? std::sqrt( Bend * Bend - x * x - y * y ) - Bend : 0.0;
                mesh.AppendVertex( glm::dvec3( x, y, z ) );
            }
        }
        const auto id = []( int i, int k ) { return k * ( Cells + 1 ) + i; };
        for ( int k = 0; k < Cells; ++k )
        {
            for ( int i = 0; i < Cells; ++i )
            {
                mesh.AppendTriangle( id( i, k ), id( i + 1, k ), id( i + 1, k + 1 ) );
                mesh.AppendTriangle( id( i, k ), id( i + 1, k + 1 ), id( i, k + 1 ) );
            }
        }
        mesh.EnableAttributes();
        return mesh;
    }

    double Angle3( const glm::dvec3& a, const glm::dvec3& b, const glm::dvec3& c )
    {
        return std::acos( std::clamp( glm::dot( Normalized( b - a ), Normalized( c - a ) ), -1.0, 1.0 ) );
    }

    struct FStats
    {
        double MeanAngleError = 0.0;
        double MaxAngleError  = 0.0;
        int    Positive       = 0; // UV triangles by orientation
        int    Negative       = 0;
        double MinAreaRatio   = 1e30; // UV area / 3D area, normalized by the mean ratio
    };

    FStats Measure( const FDynamicMesh3& mesh, const FDynamicMeshUVOverlay& uvs )
    {
        FStats              s;
        int                 count    = 0;
        double              sumRatio = 0.0;
        std::vector<double> ratios;
        for ( const int t : mesh.TriangleIndicesItr() )
        {
            const FIndex3i v = mesh.GetTriangle( t );
            const FIndex3i e = uvs.GetTriangle( t );
            glm::dvec3     p[3]{};
            glm::dvec3     q[3]{};
            for ( int j = 0; j < 3; ++j )
            {
                p[j]               = mesh.GetVertex( v[j] );
                const glm::vec2 uv = uvs.GetElement( e[j] );
                q[j]               = glm::dvec3( uv.x, uv.y, 0.0 );
            }
            for ( int j = 0; j < 3; ++j )
            {
                const double err = std::abs( Angle3( p[j], p[( j + 1 ) % 3], p[( j + 2 ) % 3] ) -
                                             Angle3( q[j], q[( j + 1 ) % 3], q[( j + 2 ) % 3] ) );
                s.MeanAngleError += err;
                s.MaxAngleError = std::max( s.MaxAngleError, err );
                ++count;
            }
            const double signedUV = glm::cross( ( q[1] - q[0] ), q[2] - q[0] ).z;
            ( signedUV > 0.0 ? s.Positive : s.Negative )++;
            const double ratio = std::abs( signedUV ) / glm::length( glm::cross( ( p[1] - p[0] ), p[2] - p[0] ) );
            ratios.push_back( ratio );
            sumRatio += ratio;
        }
        s.MeanAngleError /= count;
        for ( const double r : ratios )
            s.MinAreaRatio = std::min( s.MinAreaRatio, r * static_cast<double>( ratios.size() ) / sumRatio );
        return s;
    }

    TArray<int32_t> AllTriangles( const FDynamicMesh3& mesh )
    {
        TArray<int32_t> triangles;
        for ( const int t : mesh.TriangleIndicesItr() )
            triangles.Add( t );
        return triangles;
    }
} // namespace

TEST( SpectralConformalUV, FlatIrregularSquareMapsToASimilarityWithFreeCorners )
{
    for ( const bool preserveIrregularity : { false, true } )
    {
        FDynamicMesh3          mesh = Grid( 0.0 );
        FDynamicMeshUVOverlay& uvs  = *mesh.Attributes()->PrimaryUV();
        FDynamicMeshUVEditor   editor( &mesh, &uvs );
        FUVEditResult          result;
        ASSERT_TRUE( editor.SetTriangleUVsFromFreeBoundarySpectralConformal( AllTriangles( mesh ), false,
                                                                             preserveIrregularity, &result ) );
        EXPECT_EQ( result.NewUVElements.Num(), mesh.VertexCount() );
        const FStats s = Measure( mesh, uvs );
        // a similarity keeps every angle, the square's 90-degree corners included (the boundary is free)
        EXPECT_LT( s.MaxAngleError, 1e-4 ) << "irregularity " << preserveIrregularity;
        // UE's area term reverses every edge for its winding: a counter-clockwise (+Z) triangle maps clockwise
        EXPECT_EQ( s.Negative, 2 * Cells * Cells ) << s.Positive << " / " << s.Negative;
        EXPECT_GT( s.MinAreaRatio, 0.99 );
    }
}

TEST( SpectralConformalUV, SphericalCapInPlaceKeepsOrientationAndAngles )
{
    // the bevel's call: existing UV topology, irregularity preserved
    FDynamicMesh3          mesh = Grid( 120.0 );
    FDynamicMeshUVOverlay& uvs  = *mesh.Attributes()->PrimaryUV();
    FDynamicMeshUVEditor   editor( &mesh, &uvs );
    ASSERT_TRUE( editor.SetTriangleUVsFromExpMap( AllTriangles( mesh ) ) );
    const int     elementsBefore = uvs.ElementCount();
    FUVEditResult result;
    ASSERT_TRUE(
         editor.SetTriangleUVsFromFreeBoundarySpectralConformal( AllTriangles( mesh ), true, true, &result ) );
    EXPECT_EQ( uvs.ElementCount(), elementsBefore );
    EXPECT_EQ( result.NewUVElements.Num(), elementsBefore );
    const FStats s = Measure( mesh, uvs );
    EXPECT_EQ( s.Negative, 2 * Cells * Cells ) << s.Positive << " / " << s.Negative;
    // measured: mean 0.0058 rad, max 0.016, smallest normalized area ratio 0.946
    EXPECT_GT( s.MinAreaRatio, 0.9 );
    EXPECT_LT( s.MeanAngleError, 0.0075 );
    EXPECT_LT( s.MaxAngleError, 0.02 );
}
