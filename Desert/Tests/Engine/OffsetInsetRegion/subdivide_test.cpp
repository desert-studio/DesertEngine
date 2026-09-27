// Subdivide (MeshRegionOperation.hpp SubdivideMesh over the SubdividePoly port and OpenSubdiv) on the input the
// editor gives it: a closed cube imported from render data WITH a tangent space, one polygroup per face. The
// expected positions are Catmull-Clark's / Loop's own rules worked by hand, not read back from the refiner.
#include "Engine/Geometry/DynamicMeshRenderConversion.hpp"
#include "Engine/Geometry/MeshRegionOperation.hpp"
#include "Engine/Geometry/MeshCore/DynamicMesh/DynamicMeshAttributeSet.hpp"

#include <gtest/gtest.h>

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <string>

using namespace Desert;
using namespace Desert::Geometry;

namespace
{
    constexpr double kHalf = 50.0;

    Vertex MakeVertex( const glm::vec3& p, const glm::vec3& n, const glm::vec3& t, const glm::vec2& uv )
    {
        Vertex v{};
        v.Position  = p;
        v.Normal    = n;
        v.Tangent   = t;
        v.Bitangent = glm::cross( n, t );
        v.TexCoord  = uv;
        return v;
    }

    // region_operation_test.cpp's HardCube: one quad per face with its own normal, tangent and UVs.
    RenderMeshData HardCube( float half )
    {
        const glm::vec3 X( 1, 0, 0 );
        const glm::vec3 Y( 0, 1, 0 );
        const glm::vec3 Z( 0, 0, 1 );
        struct Face
        {
            glm::vec3 N{}, U{}, V{};
        };
        const Face faces[6] = { { X, Y, Z }, { -X, Z, Y }, { Y, Z, X }, { -Y, X, Z }, { Z, X, Y }, { -Z, Y, X } };
        std::vector<Vertex> vertices;
        std::vector<Index>  indices;
        for ( const Face& face : faces )
        {
            const glm::vec3 c    = face.N * half;
            const auto      base = static_cast<uint32_t>( vertices.size() );
            vertices.push_back( MakeVertex( c - face.U * half - face.V * half, face.N, face.U, { 0, 0 } ) );
            vertices.push_back( MakeVertex( c + face.U * half - face.V * half, face.N, face.U, { 1, 0 } ) );
            vertices.push_back( MakeVertex( c + face.U * half + face.V * half, face.N, face.U, { 1, 1 } ) );
            vertices.push_back( MakeVertex( c - face.U * half + face.V * half, face.N, face.U, { 0, 1 } ) );
            indices.push_back( { base, base + 1, base + 2 } );
            indices.push_back( { base, base + 2, base + 3 } );
        }
        RenderMeshData render;
        Submesh        s{};
        s.Name          = "MaterialID 0";
        s.VertexCount   = static_cast<uint32_t>( vertices.size() );
        s.IndexCount    = static_cast<uint32_t>( indices.size() * 3 );
        s.Transform     = glm::mat4( 1.0f );
        render.Vertices = vertices;
        render.Indices  = indices;
        render.Submeshes.push_back( s );
        render.SubmeshMaterialIds.push_back( 0 );
        return render;
    }

    // A closed (welded) cube; face f is polygroup f + 1.
    DynamicMesh3 TangentCube()
    {
        auto imported = DynamicMeshFromRenderMesh( HardCube( static_cast<float>( kHalf ) ) );
        EXPECT_TRUE( imported.IsSuccess() ) << ( imported.IsSuccess() ? "" : imported.GetError() );
        DynamicMesh3 mesh = std::move( imported.ExtractValue().Mesh );
        mesh.EnableTriangleGroups();
        for ( int const t : mesh.TriangleIndicesItr() )
            mesh.SetTriangleGroup( t, 1 + t / 2 );
        return mesh;
    }

    // A copy: the outcome holding the mesh is usually a temporary.
    DynamicMesh3 Subdivided( const Common::ResultStr<RegionOutcome>& result )
    {
        EXPECT_TRUE( result.IsSuccess() ) << ( result.IsSuccess() ? "" : result.GetError() );
        return result.IsSuccess() ? *result.GetValue().Mesh : DynamicMesh3{};
    }

    void ExpectRefused( const Common::ResultStr<RegionOutcome>& result, const std::string& fragment )
    {
        ASSERT_FALSE( result.IsSuccess() );
        EXPECT_NE( result.GetError().find( fragment ), std::string::npos ) << result.GetError();
    }

    SubdivideSettings Settings( SubdivisionScheme scheme, int level )
    {
        SubdivideSettings settings;
        settings.Scheme = scheme;
        settings.Level  = level;
        return settings;
    }

    int BoundaryEdgeCount( const DynamicMesh3& mesh )
    {
        int count = 0;
        for ( int const e : mesh.EdgeIndicesItr() )
            count += mesh.IsBoundaryEdge( e ) ? 1 : 0;
        return count;
    }

    // Every original cube corner's image: the vertex nearest (h, h, h) * sign in each octant.
    std::vector<glm::dvec3> CornerImages( const DynamicMesh3& mesh )
    {
        std::vector<glm::dvec3> images;
        for ( int sx = -1; sx <= 1; sx += 2 )
            for ( int sy = -1; sy <= 1; sy += 2 )
                for ( int sz = -1; sz <= 1; sz += 2 )
                {
                    const glm::dvec3 corner( sx * kHalf, sy * kHalf, sz * kHalf );
                    glm::dvec3       best( 0.0 );
                    for ( int const v : mesh.VertexIndicesItr() )
                        if ( glm::length( mesh.GetVertex( v ) - corner ) < glm::length( best - corner ) )
                            best = mesh.GetVertex( v );
                    images.push_back( best );
                }
        return images;
    }
} // namespace

TEST( Subdivide, TheCubeIsClosedAndHasSixFaceGroups )
{
    const DynamicMesh3 cube = TangentCube();
    EXPECT_EQ( cube.VertexCount(), 8 );
    EXPECT_EQ( BoundaryEdgeCount( cube ), 0 );
    EXPECT_TRUE( cube.Attributes()->HasTangentSpace() );
}

// Catmull-Clark at a valence-3 cube corner: (Q + 2R) / 3 with Q the mean face point h/3 (1,1,1) and R the mean
// edge midpoint 2h/3 (1,1,1), so every corner lands on 5h/9 (1,1,1).
TEST( Subdivide, CatmullClarkMovesEveryCubeCornerToFiveNinthsOfTheHalfSize )
{
    const DynamicMesh3  cube = TangentCube();
    const DynamicMesh3& fine = Subdivided(
         SubdivideMesh( cube, Settings( SubdivisionScheme::CatmullClark, 1 ), ElementMode::Triangle ) );
    EXPECT_EQ( fine.VertexCount(), 8 + 12 + 6 );
    EXPECT_EQ( fine.TriangleCount(), 6 * 4 * 2 );
    EXPECT_EQ( BoundaryEdgeCount( fine ), 0 );
    for ( const glm::dvec3& p : CornerImages( fine ) )
        for ( int axis = 0; axis < 3; ++axis )
            EXPECT_NEAR( std::abs( p[axis] ), 5.0 * kHalf / 9.0, 1e-3 ) << p.x << " " << p.y << " " << p.z;
}

TEST( Subdivide, BilinearSplitsTheFacesAndMovesNothingOffThem )
{
    const DynamicMesh3  cube = TangentCube();
    const DynamicMesh3& fine =
         Subdivided( SubdivideMesh( cube, Settings( SubdivisionScheme::Bilinear, 2 ), ElementMode::Triangle ) );
    EXPECT_EQ( fine.TriangleCount(), 6 * 16 * 2 );
    for ( int const v : fine.VertexIndicesItr() )
    {
        const glm::dvec3 p = fine.GetVertex( v );
        EXPECT_NEAR( std::max( { std::abs( p.x ), std::abs( p.y ), std::abs( p.z ) } ), kHalf, 1e-4 );
    }
}

// Loop on the cube's twelve triangles: 8 + 18 vertices after one level; every vertex moves strictly inside.
TEST( Subdivide, LoopRunsOnTheTrianglesAndShrinksTheCube )
{
    const DynamicMesh3  cube = TangentCube();
    const DynamicMesh3& fine =
         Subdivided( SubdivideMesh( cube, Settings( SubdivisionScheme::Loop, 1 ), ElementMode::Triangle ) );
    EXPECT_EQ( fine.VertexCount(), 8 + 18 );
    EXPECT_EQ( fine.TriangleCount(), 12 * 4 );
    for ( const glm::dvec3& p : CornerImages( fine ) )
        EXPECT_LT( std::max( { std::abs( p.x ), std::abs( p.y ), std::abs( p.z ) } ), kHalf - 1.0 );
}

TEST( Subdivide, EachFaceKeepsItsPolygroupUnlessNewGroupsAreAsked )
{
    const DynamicMesh3  cube = TangentCube();
    const DynamicMesh3& kept = Subdivided(
         SubdivideMesh( cube, Settings( SubdivisionScheme::CatmullClark, 1 ), ElementMode::Triangle ) );
    std::map<int, int> perGroup;
    for ( int const t : kept.TriangleIndicesItr() )
        ++perGroup[kept.GetTriangleGroup( t )];
    ASSERT_EQ( perGroup.size(), 6u );
    for ( const auto& [group, count] : perGroup )
    {
        EXPECT_GE( group, 1 );
        EXPECT_LE( group, 6 );
        EXPECT_EQ( count, 8 ) << "group " << group;
    }

    SubdivideSettings fresh    = Settings( SubdivisionScheme::CatmullClark, 1 );
    fresh.NewPolyGroups        = true;
    const auto          result = SubdivideMesh( cube, fresh, ElementMode::Triangle );
    const DynamicMesh3& split  = Subdivided( result );
    std::set<int>       groups;
    for ( int const t : split.TriangleIndicesItr() )
        groups.insert( split.GetTriangleGroup( t ) );
    EXPECT_EQ( groups.size(), 24u );
}

// The relation the editor depends on: every scheme's result, with interpolated and generated normals, converts
// back to render data - UVs, normals and the rebuilt tangent space set on every triangle.
TEST( Subdivide, EveryResultRendersBackWithItsTangentSpace )
{
    const DynamicMesh3 cube = TangentCube();
    for ( const SubdivisionScheme scheme :
          { SubdivisionScheme::Bilinear, SubdivisionScheme::CatmullClark, SubdivisionScheme::Loop } )
        for ( const SubdivisionOutputNormals normals :
              { SubdivisionOutputNormals::Generated, SubdivisionOutputNormals::Interpolated } )
        {
            SubdivideSettings settings = Settings( scheme, 2 );
            settings.Normals           = normals;
            const auto result          = SubdivideMesh( cube, settings, ElementMode::PolyGroup );
            ASSERT_TRUE( result.IsSuccess() ) << ToString( scheme ) << ": " << result.GetError();
            const DynamicMesh3& fine = *result.GetValue().Mesh;
            EXPECT_TRUE( fine.Attributes()->HasTangentSpace() ) << ToString( scheme );
            EXPECT_EQ( result.GetValue().Selection.Mode(), ElementMode::PolyGroup );
            EXPECT_TRUE( result.GetValue().Selection.Ids().empty() );
            const auto render = ToRenderMesh( fine );
            EXPECT_TRUE( render.IsSuccess() ) << ToString( scheme ) << " / " << ToString( normals ) << ": "
                                              << ( render.IsSuccess() ? "" : render.GetError() );
        }
}

// Interpolated normals keep the cube's hard edges (the seams run along the group boundaries): a triangle of
// the +Z face keeps (0, 0, 1) on every corner under Bilinear.
TEST( Subdivide, InterpolatedNormalsKeepTheHardEdgesOfTheFaces )
{
    const DynamicMesh3 cube                 = TangentCube();
    SubdivideSettings  settings             = Settings( SubdivisionScheme::Bilinear, 1 );
    settings.Normals                        = SubdivisionOutputNormals::Interpolated;
    const DynamicMesh3&             fine    = Subdivided( SubdivideMesh( cube, settings, ElementMode::Triangle ) );
    const DynamicMeshNormalOverlay& normals = *fine.Attributes()->PrimaryNormals();
    int                             onTop   = 0;
    for ( int const t : fine.TriangleIndicesItr() )
    {
        const Index3i tri = fine.GetTriangle( t );
        if ( fine.GetVertex( tri.A ).z < kHalf - 1e-3 || fine.GetVertex( tri.B ).z < kHalf - 1e-3 ||
             fine.GetVertex( tri.C ).z < kHalf - 1e-3 )
            continue;
        ++onTop;
        const Index3i elements = normals.GetTriangle( t );
        for ( int i = 0; i < 3; ++i )
            EXPECT_NEAR( normals.GetElement( elements[i] ).z, 1.0f, 1e-5f ) << "triangle " << t;
    }
    EXPECT_EQ( onTop, 8 );
}

// Loop's boundary rule on an open two-triangle quad: a corner with ONE adjoining face stays put under Sharp
// Corners and is pulled towards its boundary neighbours under Smooth Corners.
TEST( Subdivide, SharpCornersPinTheSingleFaceCornersAndSmoothCornersMoveThem )
{
    DynamicMesh3 quad;
    quad.EnableAttributes();
    quad.EnableTriangleGroups();
    const int v0 = quad.AppendVertex( glm::dvec3( 0, 0, 0 ) );
    const int v1 = quad.AppendVertex( glm::dvec3( 100, 0, 0 ) );
    const int v2 = quad.AppendVertex( glm::dvec3( 100, 100, 0 ) );
    const int v3 = quad.AppendVertex( glm::dvec3( 0, 100, 0 ) );
    quad.AppendTriangle( v0, v1, v2, 1 );
    quad.AppendTriangle( v0, v2, v3, 1 );
    // v1 and v3 have one adjoining face each.
    const auto nearest = []( const DynamicMesh3& mesh, const glm::dvec3& p )
    {
        double best = 1e30;
        for ( int const v : mesh.VertexIndicesItr() )
            best = std::min( best, glm::length( mesh.GetVertex( v ) - p ) );
        return best;
    };
    SubdivideSettings sharp = Settings( SubdivisionScheme::Loop, 1 );
    sharp.Boundary          = SubdivisionBoundaryScheme::SharpCorners;
    sharp.Normals           = SubdivisionOutputNormals::Generated;
    const auto sharpResult  = SubdivideMesh( quad, sharp, ElementMode::Triangle );
    EXPECT_NEAR( nearest( Subdivided( sharpResult ), glm::dvec3( 100, 0, 0 ) ), 0.0, 1e-6 );
    EXPECT_NEAR( nearest( Subdivided( sharpResult ), glm::dvec3( 0, 100, 0 ) ), 0.0, 1e-6 );

    SubdivideSettings smooth = sharp;
    smooth.Boundary          = SubdivisionBoundaryScheme::SmoothCorners;
    const auto smoothResult  = SubdivideMesh( quad, smooth, ElementMode::Triangle );
    EXPECT_GT( nearest( Subdivided( smoothResult ), glm::dvec3( 100, 0, 0 ) ), 1.0 );
}

TEST( Subdivide, RefusesByNameAndNumbers )
{
    const DynamicMesh3 cube = TangentCube();
    ExpectRefused( SubdivideMesh( cube, Settings( SubdivisionScheme::Loop, 0 ), ElementMode::Triangle ),
                   "at least 1, not 0" );
    ExpectRefused( SubdivideMesh( DynamicMesh3{}, Settings( SubdivisionScheme::Loop, 1 ), ElementMode::Triangle ),
                   "no triangles" );
    // Six cage faces: floor(log2(3000000 / 7) / 2) = 9.
    EXPECT_EQ( MaxSubdivisionLevel( 6 ), 9 );
    EXPECT_EQ( MaxSubdivisionLevel( 1 ), kMaxSubdivisionLevel );
    ExpectRefused( SubdivideMesh( cube, Settings( SubdivisionScheme::CatmullClark, 10 ), ElementMode::Triangle ),
                   "level 10 exceeds the maximum 9 for 6 Catmull-Clark faces" );

    // One polygroup over the closed cube has no boundary: the polygon schemes cannot use it, Loop can.
    DynamicMesh3 oneGroup = cube;
    for ( int const t : oneGroup.TriangleIndicesItr() )
        oneGroup.SetTriangleGroup( t, 1 );
    ExpectRefused(
         SubdivideMesh( oneGroup, Settings( SubdivisionScheme::CatmullClark, 1 ), ElementMode::Triangle ),
         "Catmull-Clark runs on the polygroups as polygons and a polygroup has no boundary" );
    EXPECT_TRUE(
         SubdivideMesh( oneGroup, Settings( SubdivisionScheme::Loop, 1 ), ElementMode::Triangle ).IsSuccess() );
}
