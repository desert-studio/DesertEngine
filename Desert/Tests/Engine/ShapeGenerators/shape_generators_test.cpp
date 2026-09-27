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

#include <Common/Core/Math/Ray.hpp>

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace
{
    using namespace Desert::Geometry;
    using Desert::Index;
    using Desert::Submesh;
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
        int                                             Euler           = 2;    // V - E + F of the welded mesh
        int                                             Boundary        = 0;    // open edges (0 when Closed)
    };

    // A curved flight's floor plan per step: the annular sector between the chords of the two radii.
    float CurvedStepArea( float inner, float width, float degrees, int steps )
    {
        const float outer = inner + width;
        return 0.5f * std::sin( glm::radians( std::abs( degrees ) ) / static_cast<float>( steps ) ) *
               ( outer * outer - inner * inner );
    }

    StairsShape Stairs( StairsType type, float curveAngle = 90.0f )
    {
        StairsShape s;
        s.Type        = type;
        s.Steps       = 6;
        s.StepWidth   = 200.0f;
        s.StepHeight  = 20.0f;
        s.StepDepth   = 30.0f;
        s.InnerRadius = 150.0f;
        s.CurveAngle  = curveAngle;
        return s;
    }

    std::vector<ShapeCase> Cases()
    {
        const auto pi = glm::pi<float>();
        return {
             { "Box", []( const ShapeOptions& o ) { return MakeBox( { 200.0f, 100.0f, 50.0f }, { 2, 3, 1 }, o ); },
               6, 2 * ( 2 * 3 + 3 * 1 + 1 * 2 ), true, 200.0f * 100.0f * 50.0f, 1e-4f },
             { "Plane", []( const ShapeOptions& o ) { return MakePlane( { 100.0f, 60.0f }, { 3, 2 }, o ); }, 1, 6,
               false, 0.0f, 0.0f, 1, 2 * ( 3 + 2 ) },
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
             { "Stairs Linear",
               []( const ShapeOptions& o ) { return MakeStairs( Stairs( StairsType::Linear ), o ); }, 4 + 2 * 6,
               6 * 7 + 4 * 6, true, 200.0f * 30.0f * 20.0f * 21.0f, 1e-4f },
             // Floating: each step is two rows tall except the first, 2*6-1 side cells per wall (UE's
             // NumQuadsPerSide) and 4*6 quads across (UE's NumConnectQuads).
             { "Stairs Floating",
               []( const ShapeOptions& o ) { return MakeStairs( Stairs( StairsType::Floating ), o ); }, 4 + 2 * 6,
               2 * 11 + 4 * 6, true, 200.0f * 30.0f * 20.0f * 11.0f, 1e-4f },
             { "Stairs Curved",
               []( const ShapeOptions& o ) { return MakeStairs( Stairs( StairsType::Curved ), o ); }, 4 + 2 * 6,
               6 * 7 + 4 * 6, true, CurvedStepArea( 150.0f, 200.0f, 90.0f, 6 ) * 20.0f * 21.0f, 1e-4f },
             // A negative angle turns the other way: the mirrored flight must still face out.
             { "Stairs Curved CCW", []( const ShapeOptions& o )
               { return MakeStairs( Stairs( StairsType::Curved, -120.0f ), o ); }, 4 + 2 * 6, 6 * 7 + 4 * 6, true,
               CurvedStepArea( 150.0f, 200.0f, 120.0f, 6 ) * 20.0f * 21.0f, 1e-4f },
             { "Stairs Spiral", []( const ShapeOptions& o )
               { return MakeStairs( Stairs( StairsType::Spiral, 400.0f ), o ); }, 4 + 2 * 6, 2 * 11 + 4 * 6, true,
               CurvedStepArea( 150.0f, 200.0f, 400.0f, 6 ) * 20.0f * 11.0f, 1e-4f },
             // Genus one: V - E + F = 0. Pappus on the faceted solid: a 12-gon of circumradius 15 at R = 45,
             // revolved as a 16-gon ring, is exactly 16 sin(2pi/16) x (12/2 sin(2pi/12) 15^2) x 45.
             { "Torus", []( const ShapeOptions& o ) { return MakeTorus( 120.0f, 30.0f, 16, 12, o ); }, 1, 16 * 12,
               true,
               16.0f * std::sin( 2.0f * pi / 16.0f ) * 6.0f * std::sin( 2.0f * pi / 12.0f ) * 15.0f * 15.0f *
                    45.0f,
               1e-4f, 0 },
             { "Arrow", []( const ShapeOptions& o ) { return MakeArrow( 40.0f, 200.0f, 120.0f, 120.0f, 12, o ); },
               4, 4 * 12, true, pi * 20.0f * 20.0f * 200.0f + pi * 60.0f * 60.0f * 120.0f / 3.0f, 0.06f },
             // Head as wide as the shaft: no underside ring, one face fewer.
             { "Arrow flush",
               []( const ShapeOptions& o ) { return MakeArrow( 40.0f, 200.0f, 40.0f, 60.0f, 12, o ); }, 3, 3 * 12,
               true, pi * 20.0f * 20.0f * 200.0f + pi * 20.0f * 20.0f * 60.0f / 3.0f, 0.06f },
             { "Disc", []( const ShapeOptions& o ) { return MakeDisc( 100.0f, 0.0f, 16, 3, o ); }, 1, 16 * 3,
               false, 0.0f, 0.0f, 1, 16 },
             { "Punctured Disc", []( const ShapeOptions& o ) { return MakeDisc( 100.0f, 40.0f, 16, 2, o ); }, 1,
               16 * 2, false, 0.0f, 0.0f, 0, 2 * 16 },
             { "Rectangle",
               []( const ShapeOptions& o ) { return MakeRectangle( { 100.0f, 60.0f }, { 3, 2 }, o ); }, 1, 6,
               false, 0.0f, 0.0f, 1, 2 * ( 3 + 2 ) },
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
            EXPECT_EQ( boundary, c.Boundary ) << "open edges: the rim of an open shape, none on a closed one";
            EXPECT_EQ( mesh.VertexCount() - mesh.EdgeCount() + mesh.TriangleCount(), c.Euler )
                 << "not one shell of the expected genus";
            if ( c.Closed )
            {
                const double volume = SignedVolume( m );
                EXPECT_GT( volume, 0.0 ) << "wound inward";
                EXPECT_NEAR( volume / c.Volume, 1.0, c.VolumeTolerance );
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

// What the entity draws is the same outward shell: the render mesh SetEditableMesh builds from the EditMesh
// (ToRenderMesh) has the shape's triangle count and its signed volume.
TEST( ShapeGenerators, TheEditMeshRendersTheSameShell )
{
    for ( const ShapeCase& c : Cases() )
    {
        SCOPED_TRACE( c.Name );
        const ShapeMesh m         = c.Make( {} );
        auto            converted = ShapeToEditMesh( m );
        ASSERT_TRUE( converted.IsSuccess() ) << converted.GetError();
        auto render = ToRenderMesh( converted.GetValue() );
        ASSERT_TRUE( render.IsSuccess() ) << render.GetError();
        ShapeMesh drawn;
        drawn.Vertices = render.GetValue().Vertices;
        for ( const Submesh& sub : render.GetValue().Submeshes )
            for ( uint32_t k = 0; k < sub.IndexCount / 3; ++k )
            {
                Index t = render.GetValue().Indices[sub.IndexOffset / 3 + k];
                t.V1 += sub.VertexOffset;
                t.V2 += sub.VertexOffset;
                t.V3 += sub.VertexOffset;
                drawn.Indices.push_back( t );
            }
        EXPECT_EQ( drawn.Indices.size(), m.Indices.size() );
        if ( c.Closed )
            EXPECT_NEAR( SignedVolume( drawn ) / SignedVolume( m ), 1.0, 1e-4 );
    }
}

// Base puts the bottom on Y = 0, Centre the middle, Top the top; X and Z stay centred.
// UE clamps Box Subdivisions at 500 (AddPrimitiveTool.h), and the panel lets a typed value reach it: the
// generator and the EditMesh the Create tool builds from it must both carry a box that dense, whole.
TEST( ShapeGenerators, ABoxAtUEsSubdivisionCeilingIsWhole )
{
    constexpr int   n     = 500;
    const ShapeMesh m     = MakeBox( { 100.0f, 100.0f, 100.0f }, glm::ivec3( n ) );
    const size_t    quads = 6u * static_cast<size_t>( n ) * static_cast<size_t>( n );
    ASSERT_EQ( m.Indices.size(), 2u * quads );
    ASSERT_EQ( m.Groups.size(), m.Indices.size() );
    for ( const Index& t : m.Indices )
        ASSERT_TRUE( t.V1 < m.Vertices.size() && t.V2 < m.Vertices.size() && t.V3 < m.Vertices.size() );
    EXPECT_NEAR( SignedVolume( m ), 100.0 * 100.0 * 100.0, 1.0 );
    auto converted = ShapeToEditMesh( m );
    ASSERT_TRUE( converted.IsSuccess() ) << converted.GetError();
    auto render = ToRenderMesh( converted.GetValue() );
    ASSERT_TRUE( render.IsSuccess() ) << render.GetError();
    size_t drawn = 0;
    for ( const Submesh& sub : render.GetValue().Submeshes )
        drawn += sub.IndexCount / 3;
    EXPECT_EQ( drawn, m.Indices.size() );
}

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

    const auto stairs = MakeStairs( Stairs( StairsType::Linear ) ).Bounds();
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
    const std::vector<ShapeMesh> shapes = {
         MakeBox( { 0.0f, -5.0f, 0.0f }, { 0, -1, 0 } ),
         MakeSphere( 0.0f, 1, 1 ),
         MakeCylinder( -1.0f, 0.0f, 0 ),
         MakeCone( 0.0f, -1.0f, 2 ),
         MakeCapsule( 0.0f, 0.0f, 0, 0 ),
         MakePyramid( glm::vec3( 0.0f ) ),
         MakeStairs( { StairsType::Linear, 0, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f } ),
         MakeStairs( { StairsType::Floating, 0, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f } ),
         MakeStairs( { StairsType::Curved, 0, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f } ),
         MakeStairs( { StairsType::Spiral, -1, -5.0f, 0.0f, 0.0f, -1.0f, 1.0e6f } ),
         MakeArrow( 0.0f, 0.0f, 0.0f, 0.0f, 0 ) };
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

// A torus clamped from silly inputs is still one genus-one shell with its hole open.
TEST( ShapeGenerators, ADegenerateTorusKeepsItsHole )
{
    for ( const ShapeMesh& m : { MakeTorus( 0.0f, 0.0f, 0, 0 ), MakeTorus( 10.0f, 1000.0f, 3, 3 ) } )
    {
        auto converted = ShapeToEditMesh( m );
        ASSERT_TRUE( converted.IsSuccess() ) << converted.GetError();
        const EditMesh& mesh = converted.GetValue();
        EXPECT_EQ( mesh.VertexCount() - mesh.EdgeCount() + mesh.TriangleCount(), 0 );
        EXPECT_GT( SignedVolume( m ), 0.0 );
    }
}

// Each stair type reads only its own fields, as UE's EditConditions show them: a curved tread is as deep as
// the angle makes it, and a straight flight has no radius.
TEST( ShapeGenerators, EachStairTypeReadsOnlyItsOwnFields )
{
    const auto same = []( const ShapeMesh& a, const ShapeMesh& b )
    {
        if ( a.Vertices.size() != b.Vertices.size() )
            return false;
        for ( size_t i = 0; i < a.Vertices.size(); ++i )
            if ( glm::length( a.Vertices[i].Position - b.Vertices[i].Position ) > 1e-4f )
                return false;
        return true;
    };
    for ( const StairsType type : { StairsType::Curved, StairsType::Spiral } )
    {
        StairsShape deeper = Stairs( type );
        deeper.StepDepth *= 3.0f;
        EXPECT_TRUE( same( MakeStairs( Stairs( type ) ), MakeStairs( deeper ) ) ) << ToString( type );
        StairsShape wider = Stairs( type );
        wider.InnerRadius *= 2.0f;
        EXPECT_FALSE( same( MakeStairs( Stairs( type ) ), MakeStairs( wider ) ) ) << ToString( type );
    }
    for ( const StairsType type : { StairsType::Linear, StairsType::Floating } )
    {
        StairsShape curvier = Stairs( type, 270.0f );
        curvier.InnerRadius *= 2.0f;
        EXPECT_TRUE( same( MakeStairs( Stairs( type ) ), MakeStairs( curvier ) ) ) << ToString( type );
    }
}

// The side profile is UE's: a solid flight stands on the floor to its back, a floating one's underside climbs
// with it, two steps below the top (StairGenerator.cpp FFloatingStairGenerator::GenerateVertex). Both climb
// Steps x StepHeight.
TEST( ShapeGenerators, AFloatingFlightsUndersideClimbsWithIt )
{
    for ( const StairsType type : { StairsType::Linear, StairsType::Floating } )
    {
        SCOPED_TRACE( ToString( type ) );
        const ShapeMesh m   = MakeStairs( Stairs( type ) );
        const auto      box = m.Bounds();
        EXPECT_NEAR( box.Max.y - box.Min.y, 6 * 20.0f, 1e-3f );
        float backLowest = 1.0e30f;
        for ( const Vertex& v : m.Vertices )
            if ( v.Position.z > box.Max.z - 1e-3f )
                backLowest = std::min( backLowest, v.Position.y );
        EXPECT_NEAR( backLowest, type == StairsType::Floating ? 4 * 20.0f : 0.0f, 1e-3f );
    }
}

// A curved flight is an annulus sector: every wall vertex is at the inner or the outer radius from ONE
// vertical axis, and the smooth wall normal is horizontal and radial.
TEST( ShapeGenerators, ACurvedFlightStaysBetweenItsRadii )
{
    for ( const float angle : { 90.0f, -120.0f } )
    {
        SCOPED_TRACE( angle );
        const ShapeMesh m = MakeStairs( Stairs( StairsType::Curved, angle ) );
        // The axis: a wall vertex's normal is radial, so the normal lines of two wall vertices cross on the
        // axis. Riser and back corners share the foot and have tangential normals, so take the crossing most
        // pairs agree on (rounded to 0.01 cm).
        std::vector<std::pair<glm::vec2, glm::vec2>> lines;
        for ( const Vertex& v : m.Vertices )
            if ( std::abs( v.Position.y ) < 1e-3f && std::abs( v.Normal.y ) < 1e-3f )
                lines.emplace_back( glm::vec2( v.Position.x, v.Position.z ),
                                    glm::normalize( glm::vec2( v.Normal.x, v.Normal.z ) ) );
        std::map<std::pair<long, long>, int> votes;
        for ( size_t i = 0; i < lines.size(); ++i )
            for ( size_t j = i + 1; j < lines.size(); ++j )
            {
                const auto& [p, d] = lines[i];
                const auto& [q, e] = lines[j];
                const float cross  = d.x * e.y - d.y * e.x;
                if ( std::abs( cross ) < 0.05f )
                    continue;
                const float     t = ( ( q.x - p.x ) * e.y - ( q.y - p.y ) * e.x ) / cross;
                const glm::vec2 x = p + d * t;
                ++votes[{ std::lround( x.x * 100.0f ), std::lround( x.y * 100.0f ) }];
            }
        ASSERT_FALSE( votes.empty() );
        auto best = votes.begin();
        for ( auto it = votes.begin(); it != votes.end(); ++it )
            if ( it->second > best->second )
                best = it;
        const glm::vec2 centre( static_cast<float>( best->first.first ) / 100.0f,
                                static_cast<float>( best->first.second ) / 100.0f );
        float           lo = 1.0e30f;
        float           hi = 0.0f;
        for ( const Vertex& v : m.Vertices )
        {
            const float r = glm::length( glm::vec2( v.Position.x, v.Position.z ) - centre );
            lo            = std::min( lo, r );
            hi            = std::max( hi, r );
        }
        EXPECT_NEAR( lo, 150.0f, 1e-2f ) << "inner radius";
        EXPECT_NEAR( hi, 350.0f, 1e-2f ) << "inner radius + step width";
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
            // NOLINTBEGIN(bugprone-unchecked-optional-access)
            EXPECT_NEAR( drawn.Min[axis], box->Min[axis], 1e-3f ) << "axis " << axis;
            // NOLINTEND(bugprone-unchecked-optional-access)
            // NOLINTBEGIN(bugprone-unchecked-optional-access)
            EXPECT_NEAR( drawn.Max[axis], box->Max[axis], 1e-3f ) << "axis " << axis;
            // NOLINTEND(bugprone-unchecked-optional-access)
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
        auto converted = ShapeToEditMesh( *shape ); // NOLINT(bugprone-unchecked-optional-access)
        ASSERT_TRUE( converted.IsSuccess() ) << converted.GetError();
        if ( type != PrimitiveType::Plane )
            EXPECT_GT( SignedVolume( *shape ), 0.0 );
    }
}

// PLACEMENT ON A SCALED SURFACE. The Create tool puts a shape where Scene::Raycast says the scene is, and
// the ground of a scene is a Cube scaled flat (24 x 0.2 x 24). The box test runs in the entity's space,
// where the ray's direction is renormalised, so its t is in the entity's units; Scene::Raycast used that t
// as the world distance, and a shape aimed at the ground landed 370 cm under it. The hit's world distance
// is what puts the point on the surface.
TEST( ShapeGenerators, AHitOnAScaledBoxIsMeasuredInWorldUnits )
{
    const glm::mat4 ground = glm::scale( glm::translate( glm::mat4( 1.0f ), glm::vec3( 0.0f, -10.0f, 0.0f ) ),
                                         glm::vec3( 24.0f, 0.2f, 24.0f ) );
    const Common::Math::AABB unit{ glm::vec3( -50.0f ), glm::vec3( 50.0f ) };
    const Common::Math::Ray  ray( glm::vec3( 0.0f, 300.0f, 1000.0f ), glm::vec3( 0.0f, -0.5f, -1.0f ) );
    const Common::Math::Ray  local = ray.ToLocalSpace( ground );
    float                    t     = 0.0f;
    ASSERT_TRUE( local.IntersectsAABB( unit, t ) );
    const glm::vec3 onSurface = ray.GetPoint( ray.WorldDistanceOf( local, t, ground ) );
    EXPECT_NEAR( onSurface.y, 0.0f, 1e-2f ) << "the top of the ground box is Y = 0";
    EXPECT_NEAR( onSurface.z, 400.0f, 1e-2f );
    EXPECT_GT( std::abs( ray.GetPoint( t ).y ), 100.0f ) << "the local t is not a world distance under a scale";
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
