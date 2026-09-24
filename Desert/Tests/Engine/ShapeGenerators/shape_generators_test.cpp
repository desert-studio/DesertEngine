// SHAPE GENERATORS - the one source of primitive geometry (Engine/Geometry/ShapeGenerators.hpp).
//
// Every shape the Create tool offers and every `Primitive` the scene draws comes from these generators, so
// the suite holds each shape, in each polygroup mode, to the invariants a modeling tool relies on: it welds
// into ONE closed two-manifold shell (V - E + F = 2), it is wound OUTWARD (positive signed volume, close to
// the analytic one), its vertex normals agree with that winding, its polygroups are exactly the formula's
// count, and its pivot is where it was asked to be. And the box the world partitioner reads for a
// primitive (PrimitiveBounds) is the box of the vertices the renderer draws.

#include <gtest/gtest.h>

#include <Engine/Geometry/EditMesh.hpp>
#include <Engine/Geometry/PrimitiveType.hpp>
#include <Engine/Geometry/ShapeGenerators.hpp>

#include <glm/gtc/constants.hpp>

#include <functional>
#include <set>
#include <string>
#include <vector>

namespace
{
    using namespace Desert::Geometry;
    using Desert::Index;
    using Desert::Vertex;

    struct ShapeCase
    {
        std::string                                     Name;
        std::function<ShapeMesh( const ShapeOptions& )> Make;
        int                                             PerFaceGroups   = 0;
        int                                             PerQuadGroups   = 0;
        bool                                            Closed          = true;
        float                                           Volume          = 0.0f; // analytic, of the smooth shape
        float                                           VolumeTolerance = 0.0f; // relative
    };

    std::vector<ShapeCase> Cases()
    {
        const float pi = glm::pi<float>();
        return {
             { "Box", []( const ShapeOptions& o ) { return MakeBox( { 200.0f, 100.0f, 50.0f }, { 2, 3, 1 }, o ); },
               6, 2 * ( 2 * 3 + 3 * 1 + 1 * 2 ), true, 200.0f * 100.0f * 50.0f, 1e-4f },
             { "Plane", []( const ShapeOptions& o ) { return MakePlane( { 100.0f, 60.0f }, { 3, 2 }, o ); }, 1, 6,
               false, 0.0f, 0.0f },
             { "Sphere", []( const ShapeOptions& o ) { return MakeSphere( 120.0f, 16, 12, o ); }, 1, 16 * 12, true,
               4.0f / 3.0f * pi * 60.0f * 60.0f * 60.0f, 0.1f },
             { "Cylinder", []( const ShapeOptions& o ) { return MakeCylinder( 80.0f, 150.0f, 12, o ); }, 3, 3 * 12,
               true, pi * 40.0f * 40.0f * 150.0f, 0.06f },
             { "Cone", []( const ShapeOptions& o ) { return MakeCone( 80.0f, 150.0f, 12, o ); }, 2, 2 * 12, true,
               pi * 40.0f * 40.0f * 150.0f / 3.0f, 0.06f },
             { "Capsule", []( const ShapeOptions& o ) { return MakeCapsule( 60.0f, 200.0f, 16, 4, o ); }, 3,
               16 * ( 2 * 4 + 1 ), true, pi * 30.0f * 30.0f * 140.0f + 4.0f / 3.0f * pi * 30.0f * 30.0f * 30.0f,
               0.06f },
             { "Pyramid", []( const ShapeOptions& o ) { return MakePyramid( { 100.0f, 150.0f, 80.0f }, o ); }, 5,
               5, true, 100.0f * 80.0f * 150.0f / 3.0f, 1e-4f },
             // 6 steps: two sides, back, bottom, 6 risers, 6 treads; per quad the sides are 6*7/2 cells each.
             { "Stairs", []( const ShapeOptions& o ) { return MakeStairs( 200.0f, 30.0f, 20.0f, 6, o ); },
               4 + 2 * 6, 6 * 7 + 4 * 6, true, 200.0f * 30.0f * 20.0f * 21.0f, 1e-4f },
        };
    }

    constexpr ShapePolygroupMode kModes[] = { ShapePolygroupMode::PerFace, ShapePolygroupMode::PerQuad,
                                              ShapePolygroupMode::Single };

    int ExpectedGroups( const ShapeCase& c, ShapePolygroupMode mode )
    {
        switch ( mode )
        {
            case ShapePolygroupMode::PerFace:
                return c.PerFaceGroups;
            case ShapePolygroupMode::PerQuad:
                return c.PerQuadGroups;
            case ShapePolygroupMode::Single:
                return 1;
        }
        return -1;
    }

    // Sum of the signed tetrahedra from the origin: positive exactly when the shell is wound outward.
    double SignedVolume( const ShapeMesh& m )
    {
        double volume = 0.0;
        for ( const Index& t : m.Indices )
        {
            const glm::dvec3 a( m.Vertices[t.V1].Position );
            const glm::dvec3 b( m.Vertices[t.V2].Position );
            const glm::dvec3 c( m.Vertices[t.V3].Position );
            volume += glm::dot( a, glm::cross( b, c ) ) / 6.0;
        }
        return volume;
    }

    std::string Label( const ShapeCase& c, ShapePolygroupMode mode )
    {
        return c.Name + " / " + ToString( mode );
    }
} // namespace

// Every shape, every mode: one welded shell, closed (the Plane excepted), wound outward, normals agreeing.
TEST( ShapeGenerators, EveryShapeInEveryModeIsOneOutwardShell )
{
    for ( const ShapeCase& c : Cases() )
    {
        for ( const ShapePolygroupMode mode : kModes )
        {
            SCOPED_TRACE( Label( c, mode ) );
            const ShapeMesh m = c.Make( { mode, ShapePivot::Base } );
            ASSERT_FALSE( m.Indices.empty() );
            ASSERT_EQ( m.Groups.size(), m.Indices.size() );

            auto converted = ShapeToEditMesh( m );
            ASSERT_TRUE( converted.IsSuccess() ) << converted.GetError();
            const EditMesh& mesh = converted.GetValue();
            EXPECT_TRUE( mesh.IsManifold() );
            EXPECT_EQ( mesh.TriangleCount(), static_cast<int>( m.Indices.size() ) );

            int boundary = 0;
            for ( int e = 0; e < mesh.MaxEdgeId(); ++e )
                if ( mesh.IsEdge( e ) && mesh.IsBoundaryEdge( e ) )
                    ++boundary;
            if ( c.Closed )
            {
                EXPECT_EQ( boundary, 0 ) << "a closed shape has an open edge";
                EXPECT_EQ( mesh.VertexCount() - mesh.EdgeCount() + mesh.TriangleCount(), 2 )
                     << "not one sphere-like shell";
                const double volume = SignedVolume( m );
                EXPECT_GT( volume, 0.0 ) << "wound inward";
                EXPECT_NEAR( volume / c.Volume, 1.0, c.VolumeTolerance );
            }
            else
            {
                EXPECT_EQ( boundary, 2 * ( 3 + 2 ) ) << "the plane's rim, one edge per grid step";
            }

            // Each triangle's vertex normals lean the way its winding faces.
            for ( const Index& t : m.Indices )
            {
                const glm::vec3 a  = m.Vertices[t.V1].Position;
                const glm::vec3 b  = m.Vertices[t.V2].Position;
                const glm::vec3 cc = m.Vertices[t.V3].Position;
                const glm::vec3 n  = glm::cross( b - a, cc - a );
                ASSERT_GT( glm::length( n ), 1e-6f ) << "degenerate triangle";
                for ( const uint32_t v : { t.V1, t.V2, t.V3 } )
                {
                    EXPECT_NEAR( glm::length( m.Vertices[v].Normal ), 1.0f, 1e-3f );
                    EXPECT_GT( glm::dot( m.Vertices[v].Normal, n ), 0.0f ) << "normal against the winding";
                }
            }
            for ( const Vertex& v : m.Vertices )
            {
                EXPECT_GE( v.TexCoord.x, -1e-5f );
                EXPECT_LE( v.TexCoord.x, 1.0f + 1e-5f );
                EXPECT_GE( v.TexCoord.y, -1e-5f );
                EXPECT_LE( v.TexCoord.y, 1.0f + 1e-5f );
            }
        }
    }
}

// The group count is the formula's, the ids are dense, and the EditMesh carries them triangle for triangle.
TEST( ShapeGenerators, PolygroupsFollowTheModeFormula )
{
    for ( const ShapeCase& c : Cases() )
    {
        for ( const ShapePolygroupMode mode : kModes )
        {
            SCOPED_TRACE( Label( c, mode ) );
            const ShapeMesh m = c.Make( { mode, ShapePivot::Base } );
            EXPECT_EQ( m.GroupCount(), ExpectedGroups( c, mode ) );
            const std::set<int> used( m.Groups.begin(), m.Groups.end() );
            EXPECT_EQ( static_cast<int>( used.size() ), m.GroupCount() ) << "group ids are not dense";

            auto converted = ShapeToEditMesh( m );
            ASSERT_TRUE( converted.IsSuccess() ) << converted.GetError();
            for ( size_t k = 0; k < m.Groups.size(); ++k )
                ASSERT_EQ( converted.GetValue().Attributes().GetPolyGroup( static_cast<int>( k ) ), m.Groups[k] );
        }
    }
}

// Base puts the bottom on Y = 0, Centre the middle, Top the top; X and Z stay centred.
TEST( ShapeGenerators, PivotIsWhereItWasAsked )
{
    for ( const ShapeCase& c : Cases() )
    {
        SCOPED_TRACE( c.Name );
        const auto base   = c.Make( { ShapePolygroupMode::PerFace, ShapePivot::Base } ).Bounds();
        const auto centre = c.Make( { ShapePolygroupMode::PerFace, ShapePivot::Centre } ).Bounds();
        const auto top    = c.Make( { ShapePolygroupMode::PerFace, ShapePivot::Top } ).Bounds();
        EXPECT_NEAR( base.Min.y, 0.0f, 1e-3f );
        EXPECT_NEAR( centre.Min.y + centre.Max.y, 0.0f, 1e-3f );
        EXPECT_NEAR( top.Max.y, 0.0f, 1e-3f );
        EXPECT_NEAR( base.Max.y - base.Min.y, top.Max.y - top.Min.y, 1e-3f ) << "the pivot moved the shape only";
        EXPECT_NEAR( base.Min.x + base.Max.x, 0.0f, 1e-2f );
        if ( c.Closed )
            EXPECT_NEAR( base.Min.z + base.Max.z, 0.0f, 1e-2f );
    }
}

TEST( ShapeGenerators, SizesAreFullExtents )
{
    const auto box = MakeBox( { 200.0f, 100.0f, 50.0f } ).Bounds();
    EXPECT_NEAR( box.Max.x - box.Min.x, 200.0f, 1e-3f );
    EXPECT_NEAR( box.Max.y - box.Min.y, 100.0f, 1e-3f );
    EXPECT_NEAR( box.Max.z - box.Min.z, 50.0f, 1e-3f );

    const auto stairs = MakeStairs( 200.0f, 30.0f, 20.0f, 6 ).Bounds();
    EXPECT_NEAR( stairs.Max.z - stairs.Min.z, 6 * 30.0f, 1e-3f ) << "footprint is steps x depth";
    EXPECT_NEAR( stairs.Max.y, 6 * 20.0f, 1e-3f ) << "climbs steps x height";

    const auto capsule = MakeCapsule( 60.0f, 200.0f, 16, 4 ).Bounds();
    EXPECT_NEAR( capsule.Max.y - capsule.Min.y, 200.0f, 1e-2f ) << "height is end to end";
    EXPECT_NEAR( capsule.Max.x, 30.0f, 1e-2f );

    // The sphere's vertices lie on it and are smooth: the normal is the direction from the centre.
    const ShapeMesh sphere = MakeSphere( 120.0f, 16, 12 );
    for ( const Vertex& v : sphere.Vertices )
    {
        const glm::vec3 fromCentre = v.Position - glm::vec3( 0.0f, 60.0f, 0.0f );
        EXPECT_NEAR( glm::length( fromCentre ), 60.0f, 1e-2f );
        EXPECT_NEAR( glm::dot( v.Normal, glm::normalize( fromCentre ) ), 1.0f, 1e-3f );
    }
}

// A zero or negative size and silly segment counts still give a closed, outward shell.
TEST( ShapeGenerators, DegenerateInputsAreClampedIntoAShell )
{
    const std::vector<ShapeMesh> shapes = { MakeBox( { 0.0f, -5.0f, 0.0f }, { 0, -1, 0 } ),
                                            MakeSphere( 0.0f, 1, 1 ),
                                            MakeCylinder( -1.0f, 0.0f, 0 ),
                                            MakeCone( 0.0f, -1.0f, 2 ),
                                            MakeCapsule( 0.0f, 0.0f, 0, 0 ),
                                            MakePyramid( glm::vec3( 0.0f ) ),
                                            MakeStairs( 0.0f, 0.0f, 0.0f, 0 ) };
    for ( const ShapeMesh& m : shapes )
    {
        auto converted = ShapeToEditMesh( m );
        ASSERT_TRUE( converted.IsSuccess() ) << converted.GetError();
        EXPECT_EQ( converted.GetValue().VertexCount() - converted.GetValue().EdgeCount() +
                        converted.GetValue().TriangleCount(),
                   2 );
        EXPECT_GT( SignedVolume( m ), 0.0 );
    }
}

// THE BOX THE PARTITIONER READS IS THE BOX THE RENDERER DRAWS. PrimitiveBounds is a closed form beside the
// generators because the partitioner cannot afford to tessellate a sphere per record; this holds the two
// to each other for every PrimitiveType, including the ones that draw nothing.
TEST( ShapeGenerators, PrimitiveBoundsIsTheBoxOfTheGeneratedVertices )
{
    for ( int i = 0; i < static_cast<int>( PrimitiveType::Count ); ++i )
    {
        const auto type = static_cast<PrimitiveType>( i );
        SCOPED_TRACE( PrimitiveTypeName( type ) );
        const auto shape = MakePrimitive( type );
        const auto box   = PrimitiveBounds( type );
        ASSERT_EQ( shape.has_value(), box.has_value() );
        if ( !shape )
            continue;
        const auto drawn = shape->Bounds();
        for ( int axis = 0; axis < 3; ++axis )
        {
            EXPECT_NEAR( drawn.Min[axis], box->Min[axis], 1e-3f ) << "axis " << axis;
            EXPECT_NEAR( drawn.Max[axis], box->Max[axis], 1e-3f ) << "axis " << axis;
        }
    }
}

// Every authorable primitive draws something, and every one but the Plane card is a closed shell.
TEST( ShapeGenerators, EveryAuthorablePrimitiveIsDrawn )
{
    for ( const PrimitiveType type : kAuthorablePrimitives )
    {
        SCOPED_TRACE( PrimitiveTypeName( type ) );
        const auto shape = MakePrimitive( type );
        ASSERT_TRUE( shape.has_value() );
        auto converted = ShapeToEditMesh( *shape );
        ASSERT_TRUE( converted.IsSuccess() ) << converted.GetError();
        if ( type != PrimitiveType::Plane )
            EXPECT_GT( SignedVolume( *shape ), 0.0 );
    }
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
