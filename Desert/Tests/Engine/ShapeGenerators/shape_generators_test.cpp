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
        // Group counts are UE's, from its generators: a sweep (cylinder, cone, arrow, torus) groups PerFace
        // by cap and by length section, PerQuad by side quad plus one per cap; Lat Long takes PerFace as
        // PerQuad; the capsule, disc and rectangle have one group unless PerQuad.
        return {
             { "Box", []( const ShapeOptions& o ) { return MakeBox( { 200.0f, 100.0f, 50.0f }, { 2, 3, 1 }, o ); },
               6, 2 * ( 2 * 3 + 3 * 1 + 1 * 2 ), true, 200.0f * 100.0f * 50.0f, 1e-4f },
             { "Plane", []( const ShapeOptions& o ) { return MakePlane( { 100.0f, 60.0f }, { 3, 2 }, o ); }, 1, 6,
               false, 0.0f, 0.0f, 1, 2 * ( 3 + 2 ) },
             { "Sphere Lat Long",
               []( const ShapeOptions& o ) { return MakeSphere( { 60.0f, SphereType::LatLong, 1, 12, 16 }, o ); },
               16 * 12, 16 * 12, true, 4.0f / 3.0f * pi * 60.0f * 60.0f * 60.0f, 0.1f },
             { "Sphere Box", []( const ShapeOptions& o ) { return MakeSphere( { 60.0f, SphereType::Box, 4 }, o ); },
               6, 6 * 16, true, 4.0f / 3.0f * pi * 60.0f * 60.0f * 60.0f, 0.1f },
             { "Cylinder", []( const ShapeOptions& o ) { return MakeCylinder( { 40.0f, 150.0f, 12, 1 }, o ); }, 3,
               12 + 2, true, pi * 40.0f * 40.0f * 150.0f, 0.06f },
             { "Cylinder 3 high", []( const ShapeOptions& o ) { return MakeCylinder( { 40.0f, 150.0f, 12, 3 }, o ); },
               2 + 3, 3 * 12 + 2, true, pi * 40.0f * 40.0f * 150.0f, 0.06f },
             { "Cone", []( const ShapeOptions& o ) { return MakeCone( { 40.0f, 150.0f, 12, 1 }, o ); }, 3, 12 + 2,
               true, pi * 40.0f * 40.0f * 150.0f / 3.0f, 0.06f },
             { "Capsule", []( const ShapeOptions& o ) { return MakeCapsule( { 30.0f, 140.0f, 4, 16, 1 }, o ); }, 1,
               16 * ( 7 - 1 ) + 2 * 16, true,
               pi * 30.0f * 30.0f * 140.0f + 4.0f / 3.0f * pi * 30.0f * 30.0f * 30.0f, 0.06f },
             { "Pyramid", []( const ShapeOptions& o ) { return MakePyramid( { 100.0f, 150.0f, 80.0f }, o ); }, 5,
               5, true, 100.0f * 80.0f * 150.0f / 3.0f, 1e-4f },
             { "Stairs Linear",
               []( const ShapeOptions& o ) { return MakeStairs( Stairs( StairsType::Linear ), o ); }, 4 + 2 * 6,
               6 * 7 + 4 * 6, true, 200.0f * 30.0f * 20.0f * 21.0f, 1e-4f },
             { "Stairs Floating",
               []( const ShapeOptions& o ) { return MakeStairs( Stairs( StairsType::Floating ), o ); }, 4 + 2 * 6,
               2 * 11 + 4 * 6, true, 200.0f * 30.0f * 20.0f * 11.0f, 1e-4f },
             { "Stairs Curved",
               []( const ShapeOptions& o ) { return MakeStairs( Stairs( StairsType::Curved ), o ); }, 4 + 2 * 6,
               6 * 7 + 4 * 6, true, CurvedStepArea( 150.0f, 200.0f, 90.0f, 6 ) * 20.0f * 21.0f, 1e-4f },
             { "Stairs Curved CCW", []( const ShapeOptions& o )
               { return MakeStairs( Stairs( StairsType::Curved, -120.0f ), o ); }, 4 + 2 * 6, 6 * 7 + 4 * 6, true,
               CurvedStepArea( 150.0f, 200.0f, 120.0f, 6 ) * 20.0f * 21.0f, 1e-4f },
             { "Stairs Spiral", []( const ShapeOptions& o )
               { return MakeStairs( Stairs( StairsType::Spiral, 400.0f ), o ); }, 4 + 2 * 6, 2 * 11 + 4 * 6, true,
               CurvedStepArea( 150.0f, 200.0f, 400.0f, 6 ) * 20.0f * 11.0f, 1e-4f },
             // The faceted torus: a 12-gon of area 6 sin(2 pi / 12) r^2 swept along a 16-gon of radius R.
             { "Torus", []( const ShapeOptions& o ) { return MakeTorus( { 45.0f, 15.0f, 16, 12 }, o ); }, 16, 16 * 12,
               true,
               16.0f * std::sin( 2.0f * pi / 16.0f ) * 6.0f * std::sin( 2.0f * pi / 12.0f ) * 15.0f * 15.0f *
                    45.0f,
               1e-3f, 0 },
             { "Arrow", []( const ShapeOptions& o ) { return MakeArrow( { 20.0f, 200.0f, 60.0f, 120.0f, 12, 1 }, o ); },
               2 + 3, 3 * 12 + 2, true, pi * 20.0f * 20.0f * 200.0f + pi * 60.0f * 60.0f * 120.0f / 3.0f, 0.06f },
             { "Arrow 2 high",
               []( const ShapeOptions& o ) { return MakeArrow( { 20.0f, 200.0f, 60.0f, 120.0f, 12, 2 }, o ); }, 2 + 6,
               6 * 12 + 2, true, pi * 20.0f * 20.0f * 200.0f + pi * 60.0f * 60.0f * 120.0f / 3.0f, 0.06f },
             { "Disc", []( const ShapeOptions& o ) { return MakeDisc( { DiscType::Disc, 50.0f, 16, 3 }, o ); }, 1,
               16 * 3, false, 0.0f, 0.0f, 1, 16 },
             { "Punctured Disc", []( const ShapeOptions& o )
               { return MakeDisc( { DiscType::PuncturedDisc, 50.0f, 16, 3, 20.0f }, o ); }, 1, 16 * 2, false, 0.0f,
               0.0f, 0, 2 * 16 },
             { "Rectangle", []( const ShapeOptions& o )
               { return MakeRectangle( { RectangleType::Rectangle, 100.0f, 60.0f, 3, 2 }, o ); }, 1, 6, false, 0.0f,
               0.0f, 1, 2 * ( 3 + 2 ) },
             // Rounded: a (2 + 3) x (2 + 4)-vertex lattice, 20 cells of which the 4 corners are fans of
             // CornerSlices - 1 + 1 = 4 triangles; the rim is 2 * (2 + 3) straight edges + 4 arcs of 4.
             { "Rounded Rectangle", []( const ShapeOptions& o )
               { return MakeRectangle( { RectangleType::RoundedRectangle, 100.0f, 60.0f, 3, 2, true, 10.0f, 4 }, o ); },
               1, 20, false, 0.0f, 0.0f, 1, 2 * ( 2 + 3 ) + 4 * 4 },
        };
    }

    constexpr ShapePolygroupMode kModes[] = { ShapePolygroupMode::PerShape, ShapePolygroupMode::PerFace,
                                              ShapePolygroupMode::PerQuad };

    int ExpectedGroups( const ShapeCase& c, ShapePolygroupMode mode )
    {
        switch ( mode )
        {
            case ShapePolygroupMode::PerFace:
                return c.PerFaceGroups;
            case ShapePolygroupMode::PerQuad:
                return c.PerQuadGroups;
            case ShapePolygroupMode::PerShape:
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

TEST( ShapeGenerators, SizesAreUEs )
{
    const auto box = MakeBox( { 200.0f, 100.0f, 50.0f } ).Bounds();
    EXPECT_NEAR( box.Max.x - box.Min.x, 200.0f, 1e-3f );
    EXPECT_NEAR( box.Max.y - box.Min.y, 100.0f, 1e-3f );
    EXPECT_NEAR( box.Max.z - box.Min.z, 50.0f, 1e-3f );

    const auto stairs = MakeStairs( Stairs( StairsType::Linear ) ).Bounds();
    EXPECT_NEAR( stairs.Max.z - stairs.Min.z, 6 * 30.0f, 1e-3f ) << "footprint is steps x depth";
    EXPECT_NEAR( stairs.Max.y, 6 * 20.0f, 1e-3f ) << "climbs steps x height";

    const auto capsule = MakeCapsule( { 30.0f, 140.0f, 4, 16, 1 } ).Bounds();
    EXPECT_NEAR( capsule.Max.y - capsule.Min.y, 140.0f + 2 * 30.0f, 1e-2f ) << "CylinderLength + 2 Radius";
    EXPECT_NEAR( capsule.Max.x, 30.0f, 1e-2f );

    const auto rounded =
         MakeRectangle( { RectangleType::RoundedRectangle, 100.0f, 60.0f, 1, 1, true, 10.0f, 16 } ).Bounds();
    EXPECT_NEAR( rounded.Max.x - rounded.Min.x, 100.0f, 1e-3f ) << "Maintain Dimension keeps the outer size";
    EXPECT_NEAR( rounded.Max.z - rounded.Min.z, 60.0f, 1e-3f );
    const auto grown =
         MakeRectangle( { RectangleType::RoundedRectangle, 100.0f, 60.0f, 1, 1, false, 10.0f, 16 } ).Bounds();
    EXPECT_NEAR( grown.Max.x - grown.Min.x, 120.0f, 1e-3f ) << "without it the corners grow the rectangle";

    const auto torus = MakeTorus( { 45.0f, 15.0f, 16, 12 } ).Bounds();
    EXPECT_NEAR( torus.Max.x, 60.0f, 1e-3f ) << "MajorRadius + MinorRadius";
    EXPECT_NEAR( torus.Max.y - torus.Min.y, 30.0f, 1e-3f );

    const auto arrow = MakeArrow( { 20.0f, 200.0f, 60.0f, 120.0f, 16, 1 } ).Bounds();
    EXPECT_NEAR( arrow.Max.y - arrow.Min.y, 320.0f, 1e-3f ) << "ShaftHeight + HeadHeight";
    EXPECT_NEAR( arrow.Max.x, 60.0f, 1e-3f );
}

// Vertex counts after the weld are UE's own formulas (the NumVertices each generator allocates).
TEST( ShapeGenerators, WeldedVertexCountsAreUEs )
{
    const auto count = []( const ShapeMesh& m )
    {
        auto converted = ShapeToEditMesh( m );
        return converted.IsSuccess() ? converted.GetValue().VertexCount() : -1;
    };
    // FSphereGenerator: (NumPhi - 2) * NumTheta + 2, NumPhi = HorizontalSlices + 1.
    EXPECT_EQ( count( MakeSphere( { 60.0f, SphereType::LatLong, 1, 12, 16 } ) ), ( 13 - 2 ) * 16 + 2 );
    // FGridBoxMeshGenerator: 8 + 12 (N - 2) + 6 (N - 2)^2 with N = Subdivisions + 1 edge vertices.
    EXPECT_EQ( count( MakeSphere( { 60.0f, SphereType::Box, 4 } ) ), 8 + 12 * 3 + 6 * 9 );
    // FCapsuleGenerator: (2 HemisphereSlices - 2 + CylinderSubdivisions) * CylinderSlices + 2.
    EXPECT_EQ( count( MakeCapsule( { 30.0f, 140.0f, 4, 16, 1 } ) ), 7 * 16 + 2 );
    // FCylinderGenerator: two rings plus the two cap midpoints.
    EXPECT_EQ( count( MakeCylinder( { 40.0f, 150.0f, 12, 1 } ) ), 2 * 12 + 2 );
    // FArrowGenerator: four rings (base, shaft top, head base, tip) plus the midpoints.
    EXPECT_EQ( count( MakeArrow( { 20.0f, 200.0f, 60.0f, 120.0f, 12, 1 } ) ), 4 * 12 + 2 );
    // FGeneralizedCylinderGenerator, looped: MajorSlices * MinorSlices.
    EXPECT_EQ( count( MakeTorus( { 45.0f, 15.0f, 16, 12 } ) ), 16 * 12 );
    // FDiscMeshGenerator: AngleSamples * RadialSamples + 1; punctured: AngleSamples * max(RadialSamples, 2).
    EXPECT_EQ( count( MakeDisc( { DiscType::Disc, 50.0f, 16, 3 } ) ), 16 * 3 + 1 );
    EXPECT_EQ( count( MakeDisc( { DiscType::PuncturedDisc, 50.0f, 16, 1, 20.0f } ) ), 16 * 2 );
    // FRoundedRectangleMeshGenerator: W * H + 4 (AngleSamples - 1), W / H = vertex counts + 2.
    EXPECT_EQ( count( MakeRectangle( { RectangleType::RoundedRectangle, 100.0f, 60.0f, 3, 2, true, 10.0f, 4 } ) ),
               5 * 6 + 4 * 2 );
}

// UVs, hand-derived from UE's source. UE -> engine axes: X_UE = -Z, Y_UE = X, Z_UE = Y.
TEST( ShapeGenerators, UVsAreUEs )
{
    // GridBoxMeshGenerator.h:166-217: UVScale = 1 / max dimension (200); the top face (+Z_UE) runs U along
    // X_UE scaled by its depth 50 / 200, V along Y_UE scaled by 200 / 200, both unflipped.
    const ShapeMesh box = MakeBox( { 200.0f, 100.0f, 50.0f } );
    int             top = 0;
    for ( const Vertex& v : box.Vertices )
        if ( v.Normal.y > 0.99f )
        {
            ++top;
            const float xUE = -v.Position.z, yUE = v.Position.x;
            EXPECT_NEAR( v.TexCoord.x, ( xUE + 25.0f ) / 50.0f * 0.25f, 1e-5f );
            EXPECT_NEAR( v.TexCoord.y, ( yUE + 100.0f ) / 200.0f, 1e-5f );
        }
    EXPECT_EQ( top, 4 );

    // SweepGenerator.cpp:505-531 (bUVScaleMatchSidesAndCaps): the side's U = 1 - i / slices times
    // 2 pi R / max, V = 1 at the bottom ring times height / max, the caps 2R / max; max = 2 pi R here.
    const float     theta = 40.0f * glm::two_pi<float>();
    const ShapeMesh cylinder = MakeCylinder( { 40.0f, 150.0f, 12, 1 } );
    for ( const Vertex& v : cylinder.Vertices )
    {
        if ( std::abs( v.Normal.y ) > 0.99f )
        {
            // FlatMidpointFan: the unit circle * 0.5 + 0.5, then the cap scale.
            const glm::vec2 xy( -v.Position.z / 40.0f, v.Position.x / 40.0f );
            EXPECT_NEAR( v.TexCoord.x, ( xy.x * 0.5f + 0.5f ) * 80.0f / theta, 1e-4f );
            EXPECT_NEAR( v.TexCoord.y, ( xy.y * 0.5f + 0.5f ) * 80.0f / theta, 1e-4f );
        }
        else
            EXPECT_NEAR( v.TexCoord.y, ( v.Position.y < 1.0f ? 1.0f : 0.0f ) * 150.0f / theta, 1e-4f );
    }

    // SphereGenerator.h:62-95: V = ring / (NumPhi - 1) from the north pole, U = 1 - t / NumTheta with t
    // counted from +X_UE (= -Z) towards +Y_UE (= +X). A quarter turn is U = 0.75.
    const ShapeMesh sphere = MakeSphere( { 60.0f, SphereType::LatLong, 1, 12, 16 } );
    int             quarter = 0;
    for ( const Vertex& v : sphere.Vertices )
    {
        const glm::vec3 p = v.Position - glm::vec3( 0.0f, 60.0f, 0.0f );
        const float     phi = std::acos( std::clamp( p.y / 60.0f, -1.0f, 1.0f ) );
        EXPECT_NEAR( v.TexCoord.y, phi / glm::pi<float>(), 1e-4f );
        if ( std::abs( p.z ) < 1e-3f && p.x > 1.0f )
        {
            ++quarter;
            EXPECT_NEAR( v.TexCoord.x, 0.75f, 1e-5f );
        }
    }
    EXPECT_EQ( quarter, 11 ) << "one per ring";

    // DiscMeshGenerator.cpp:62-66: UV = 0.5 + 0.5 (cos, sin) * r / R.
    for ( const Vertex& v : MakeDisc( { DiscType::Disc, 50.0f, 16, 3 } ).Vertices )
    {
        EXPECT_NEAR( v.TexCoord.x, 0.5f + 0.5f * -v.Position.z / 50.0f, 1e-5f );
        EXPECT_NEAR( v.TexCoord.y, 0.5f + 0.5f * v.Position.x / 50.0f, 1e-5f );
    }

    // SweepGenerator.cpp:40-117 (bEvenlySpaceUVs): U = 1 - i / MinorSlices, V = 1 - j / MajorSlices; the
    // seam columns reach both 0 and 1.
    float lowU = 1.0f, highU = 0.0f, lowV = 1.0f, highV = 0.0f;
    for ( const Vertex& v : MakeTorus( { 45.0f, 15.0f, 16, 12 } ).Vertices )
    {
        lowU  = std::min( lowU, v.TexCoord.x );
        highU = std::max( highU, v.TexCoord.x );
        lowV  = std::min( lowV, v.TexCoord.y );
        highV = std::max( highV, v.TexCoord.y );
        const float steps = v.TexCoord.x * 12.0f;
        EXPECT_NEAR( steps, std::round( steps ), 1e-4f );
    }
    EXPECT_NEAR( lowU, 0.0f, 1e-6f );
    EXPECT_NEAR( highU, 1.0f, 1e-6f );
    EXPECT_NEAR( lowV, 0.0f, 1e-6f );
    EXPECT_NEAR( highV, 1.0f, 1e-6f );
}

// Normals, as UE shares or splits them.
TEST( ShapeGenerators, NormalsAreUEs )
{
    // FArrowGenerator marks the shaft top and the head base sharp (SweepGenerator.cpp:596): the shaft ring
    // at y = 200 carries the shaft's radial normal and the underside's straight-down one, never a blend.
    const ShapeMesh arrow = MakeArrow( { 20.0f, 200.0f, 60.0f, 120.0f, 12, 1 } );
    int             radial = 0, down = 0;
    for ( const Vertex& v : arrow.Vertices )
        if ( std::abs( v.Position.y - 200.0f ) < 1e-3f &&
             std::abs( glm::length( glm::vec2( v.Position.x, v.Position.z ) ) - 20.0f ) < 1e-3f )
        {
            if ( std::abs( v.Normal.y ) < 1e-5f )
                ++radial;
            else if ( v.Normal.y < -0.99999f )
                ++down;
            else
                ADD_FAILURE() << "a blended normal at the sharp shaft top";
        }
    EXPECT_GT( radial, 0 );
    EXPECT_GT( down, 0 );

    // FBoxSphereGenerator: the normal is the projected direction, smooth across the cube's seams.
    for ( const Vertex& v : MakeSphere( { 60.0f, SphereType::Box, 4 } ).Vertices )
    {
        const glm::vec3 fromCentre = v.Position - glm::vec3( 0.0f, 60.0f, 0.0f );
        EXPECT_NEAR( glm::length( fromCentre ), 60.0f, 1e-3f );
        EXPECT_NEAR( glm::dot( v.Normal, glm::normalize( fromCentre ) ), 1.0f, 1e-5f );
    }

    // The torus tube: radial in the tube's own plane (FPolygon2d::GetNormal_FaceAvg of a regular polygon).
    for ( const Vertex& v : MakeTorus( { 45.0f, 15.0f, 16, 12 } ).Vertices )
    {
        const glm::vec3 axis   = glm::normalize( glm::vec3( v.Position.x, 0.0f, v.Position.z ) ) * 45.0f;
        const glm::vec3 centre = axis + glm::vec3( 0.0f, 15.0f, 0.0f );
        EXPECT_NEAR( glm::dot( v.Normal, glm::normalize( v.Position - centre ) ), 1.0f, 1e-4f );
    }
}

// Polygroups: which triangles share a group, as UE's generators assign them.
TEST( ShapeGenerators, PolygroupsAreUEs )
{
    const auto sizes = []( const ShapeMesh& m )
    {
        std::map<int, int> perGroup;
        for ( const int g : m.Groups )
            ++perGroup[g];
        std::multiset<int> out;
        for ( const auto& [g, n] : perGroup )
            out.insert( n );
        return out;
    };
    // FCylinderGenerator PerFace: two caps of 12 fan triangles and the side of 12 quads.
    EXPECT_EQ( sizes( MakeCylinder( { 40.0f, 150.0f, 12, 1 }, { ShapePolygroupMode::PerFace } ) ),
               ( std::multiset<int>{ 12, 12, 24 } ) );
    // The torus groups by path segment: 16 rings of 12 quads.
    EXPECT_EQ( sizes( MakeTorus( { 45.0f, 15.0f, 16, 12 }, { ShapePolygroupMode::PerFace } ) ),
               std::multiset<int>( { 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24 } ) );
    // Lat Long's PerFace is PerQuad: each pole triangle is a group of its own.
    const auto sphere = sizes( MakeSphere( { 60.0f, SphereType::LatLong, 1, 12, 16 }, { ShapePolygroupMode::PerFace } ) );
    EXPECT_EQ( sphere.count( 1 ), 2u * 16u );
    EXPECT_EQ( sphere.count( 2 ), 10u * 16u );
    // The disc in PerQuad: the centre fan's triangles alone, the rings' quads in pairs.
    const auto disc = sizes( MakeDisc( { DiscType::Disc, 50.0f, 16, 3 }, { ShapePolygroupMode::PerQuad } ) );
    EXPECT_EQ( disc.count( 1 ), 16u );
    EXPECT_EQ( disc.count( 2 ), 32u );
}

// UE does not stop a tube from reaching the axis or a head as narrow as the shaft; the mesh it makes then
// folds onto itself, and ShapeToEditMesh refuses it by name rather than hand the scene a broken shell.
TEST( ShapeGenerators, UEsDegenerateSettingsAreRefusedNotPlaced )
{
    EXPECT_FALSE( ShapeToEditMesh( MakeTorus( { 10.0f, 10.0f, 16, 12 } ) ).IsSuccess() );
    EXPECT_FALSE( ShapeToEditMesh( MakeArrow( { 20.0f, 200.0f, 20.0f, 60.0f, 12, 1 } ) ).IsSuccess() );
}

// A zero or negative size and silly segment counts are clamped to UE's ranges and still give a shell.
TEST( ShapeGenerators, DegenerateInputsAreClampedIntoAShell )
{
    const std::vector<ShapeMesh> shapes = {
         MakeBox( { 0.0f, -5.0f, 0.0f }, { 0, -1, 0 } ),
         MakeSphere( { 0.0f, SphereType::LatLong, 0, 1, 1 } ),
         MakeSphere( { -1.0f, SphereType::Box, 0 } ),
         MakeCylinder( { -1.0f, 0.0f, 0, 0 } ),
         MakeCone( { 0.0f, -1.0f, 2, -3 } ),
         MakeCapsule( { 0.0f, 0.0f, 0, 0, -1 } ),
         MakePyramid( glm::vec3( 0.0f ) ),
         MakeStairs( { StairsType::Linear, 0, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f } ),
         MakeStairs( { StairsType::Floating, 0, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f } ),
         MakeStairs( { StairsType::Curved, 0, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f } ),
         MakeStairs( { StairsType::Spiral, -1, -5.0f, 0.0f, 0.0f, -1.0f, 1.0e6f } ),
         MakeArrow( { 0.0f, 0.0f, 1.0f, 0.0f, 0, 0 } ),
         MakeTorus( { 1.0f, 0.0f, 0, 0 } ) };
    for ( const ShapeMesh& m : shapes )
    {
        auto converted = ShapeToEditMesh( m );
        ASSERT_TRUE( converted.IsSuccess() ) << converted.GetError();
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
