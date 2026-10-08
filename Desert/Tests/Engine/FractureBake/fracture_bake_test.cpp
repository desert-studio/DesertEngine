// DST-01: the fracture bake and its `.dfrac` file. The relations pinned: the pieces tile the source (their
// volumes sum to its volume), every piece is closed, the seed decides the bake bit for bit, the methods yield
// the counts their geometry implies, and the file reads back to exactly what was written.

#include <Engine/Destruction/FractureBake.hpp>
#include <Engine/Destruction/FractureFormat.hpp>
#include <Engine/Geometry/DynamicMeshSerialization.hpp>
#include <Engine/Geometry/MeshCore/VectorUtil.hpp>

#include <gtest/gtest.h>

#include <array>

namespace
{
    using namespace Desert::Destruction;
    using Desert::Geometry::DynamicMesh3;

    // A closed box [Min, Min + Size], 12 triangles facing out (the port's winding, as the MeshBoolean suite).
    DynamicMesh3 MakeBox( const glm::dvec3& Min, const glm::dvec3& Size )
    {
        DynamicMesh3 Mesh;
        for ( int i = 0; i < 8; i++ )
            Mesh.AppendVertex( Min + Size * glm::dvec3( i & 1, ( i >> 1 ) & 1, ( i >> 2 ) & 1 ) );
        const glm::dvec3                        Center = Min + 0.5 * Size;
        const std::array<std::array<int, 4>, 6> Faces{
             { { 0, 2, 6, 4 }, { 1, 3, 7, 5 }, { 0, 1, 5, 4 }, { 2, 3, 7, 6 }, { 0, 1, 3, 2 }, { 4, 5, 7, 6 } } };
        for ( const auto& F : Faces )
        {
            glm::dvec3 FaceCenter( 0 );
            for ( const int v : F )
                FaceCenter += 0.25 * Mesh.GetVertex( v );
            const glm::dvec3 Out = FaceCenter - Center;
            for ( const auto [a, b, c] : { std::array{ F[0], F[1], F[2] }, std::array{ F[0], F[2], F[3] } } )
            {
                const glm::dvec3 n = Desert::Geometry::VectorUtil::Normal(
                     Mesh.GetVertex( a ), Mesh.GetVertex( b ), Mesh.GetVertex( c ) );
                if ( glm::dot( n, Out ) < 0 )
                    Mesh.AppendTriangle( a, c, b );
                else
                    Mesh.AppendTriangle( a, b, c );
            }
        }
        return Mesh;
    }

    const DynamicMesh3& Cube()
    {
        static const DynamicMesh3 kCube = MakeBox( glm::dvec3( -50.0 ), glm::dvec3( 100.0 ) );
        return kCube;
    }

    FractureSettings Voronoi( uint64_t seed, int sites )
    {
        FractureSettings s;
        s.Seed = seed;
        FractureLevelSettings l;
        l.Method    = FractureMethod::Uniform;
        l.SiteCount = sites;
        s.Levels.push_back( l );
        return s;
    }

    std::vector<const FractureNode*> Leaves( const FractureBakeResult& r )
    {
        std::vector<const FractureNode*> out;
        for ( const FractureNode& n : r.Nodes )
            if ( !n.Mesh.Triangles.empty() )
                out.push_back( &n );
        return out;
    }

    FractureData DataOf( const FractureSettings& s, const FractureBakeResult& r )
    {
        FractureData d;
        d.Guid               = { 0xD57001ull, 0xF4ACull };
        d.SourceMesh         = { 0x5005ull, 0xCBEull };
        d.Settings           = s;
        d.InteriorMaterialId = r.InteriorMaterialId;
        d.Nodes              = r.Nodes;
        return d;
    }

    TEST( FractureBake, VoronoiPiecesTileTheCubeAndAreClosed )
    {
        auto baked = BakeFracture( Cube(), Voronoi( 7, 8 ) );
        ASSERT_TRUE( baked ) << baked.GetError();
        const FractureBakeResult& r      = baked.GetValue();
        const auto                leaves = Leaves( r );
        ASSERT_GE( leaves.size(), 2u );

        const double cube = EnclosedVolume( Cube() );
        EXPECT_NEAR( cube, 1e6, 1e-6 );
        double sum = 0.0;
        for ( const FractureNode* n : leaves )
        {
            auto mesh = Desert::Geometry::DynamicMeshFromSerialized( n->Mesh, "piece" );
            ASSERT_TRUE( mesh ) << mesh.GetError();
            EXPECT_TRUE( mesh.GetValue().IsClosed() );
            EXPECT_GT( n->Volume, 0.0 );
            EXPECT_NEAR( EnclosedVolume( mesh.GetValue() ), n->Volume, 1e-6 * cube );
            EXPECT_GE( n->HullFaces.size(), 4u );
            sum += n->Volume;
        }
        EXPECT_NEAR( sum, cube, 1e-3 * cube );
        EXPECT_NEAR( r.Nodes[0].Volume, cube, 1e-3 * cube );
        EXPECT_EQ( r.InteriorMaterialId, 1 ); // the cube carries material 0 only
    }

    TEST( FractureBake, TheSeedDecidesTheBakeBitForBit )
    {
        const auto a = BakeFracture( Cube(), Voronoi( 11, 6 ) );
        const auto b = BakeFracture( Cube(), Voronoi( 11, 6 ) );
        const auto c = BakeFracture( Cube(), Voronoi( 12, 6 ) );
        ASSERT_TRUE( a && b && c );
        EXPECT_EQ( EncodeFracturePayload( DataOf( Voronoi( 11, 6 ), a.GetValue() ) ),
                   EncodeFracturePayload( DataOf( Voronoi( 11, 6 ), b.GetValue() ) ) );
        EXPECT_NE( EncodeFracturePayload( DataOf( Voronoi( 11, 6 ), a.GetValue() ) ),
                   EncodeFracturePayload( DataOf( Voronoi( 11, 6 ), c.GetValue() ) ) );
    }

    TEST( FractureBake, TwoPlanesCutFourPieces )
    {
        FractureSettings      s;
        FractureLevelSettings l;
        l.Method = FractureMethod::Planar;
        l.Planes = { CutPlane{ { 1.0, 0.0, 0.0 }, { 3.0, 0.0, 0.0 } },
                     CutPlane{ { 0.0, 1.0, 0.0 }, { 0.0, -7.0, 0.0 } } };
        s.Levels.push_back( l );
        auto baked = BakeFracture( Cube(), s );
        ASSERT_TRUE( baked ) << baked.GetError();
        EXPECT_EQ( Leaves( baked.GetValue() ).size(), 4u );
    }

    TEST( FractureBake, BricksCountTheBond )
    {
        // 100 cm cube, stack bond, 51 x 51 cm face: four bricks. UE tiles brick CENTRES from the cell box's Min
        // in length and depth (FractureEngineFracturing.cpp:844-900), so the first depth row is centred ON the
        // box's front face and covers only half a depth into it: a single row needs half the depth past the
        // 103.7 cm cell box (cube + 1.8 cm margin a side). 200 cm depth left a 1.8 cm second row, shifted by half
        // a length in stack bond: 4 + 6 = 10 pieces.
        FractureSettings      s;
        FractureLevelSettings l;
        l.Method       = FractureMethod::Brick;
        l.Brick.Bond   = BrickBond::Stack;
        l.Brick.Length = 51.0; // two per course: [Min, Min+51], [Min+51, Min+102] cover the cube's 101.8 cm
        l.Brick.Height = 51.0;
        l.Brick.Depth  = 400.0; // half 200 > 103.7: one depth row
        s.Levels.push_back( l );
        auto baked = BakeFracture( Cube(), s );
        ASSERT_TRUE( baked ) << baked.GetError();
        EXPECT_EQ( Leaves( baked.GetValue() ).size(), 4u );
    }

    TEST( FractureBake, LevelsAndClustersKeepParentsBeforeChildren )
    {
        FractureSettings s = Voronoi( 3, 8 );
        s.Levels.push_back( s.Levels[0] );
        s.Levels[1].SiteCount       = 3;
        s.Levels[1].DamageThreshold = 1000.0f;
        s.AutoCluster.Enabled       = true;
        auto baked                  = BakeFracture( Cube(), s );
        ASSERT_TRUE( baked ) << baked.GetError();
        const auto& nodes = baked.GetValue().Nodes;
        EXPECT_EQ( nodes[0].Parent, -1 );
        double sum = 0.0;
        for ( size_t i = 1; i < nodes.size(); ++i )
        {
            ASSERT_LT( nodes[i].Parent, static_cast<int32_t>( i ) );
            EXPECT_EQ( nodes[i].Level, nodes[static_cast<size_t>( nodes[i].Parent )].Level + 1 );
            if ( !nodes[i].Mesh.Triangles.empty() )
                sum += nodes[i].Volume;
        }
        EXPECT_NEAR( sum, 1e6, 1e3 );
    }

    TEST( FractureFormat, TheFileReadsBackToWhatWasWritten )
    {
        const FractureSettings s     = Voronoi( 5, 5 );
        auto                   baked = BakeFracture( Cube(), s );
        ASSERT_TRUE( baked ) << baked.GetError();
        const FractureData written = DataOf( s, baked.GetValue() );
        auto               file    = EncodeFracture( written );
        ASSERT_TRUE( file ) << file.GetError();
        auto read = DecodeFracture( file.GetValue() );
        ASSERT_TRUE( read ) << read.GetError();
        EXPECT_EQ( read.GetValue(), written );

        std::vector<unsigned char> cut = file.GetValue();
        cut.resize( cut.size() - 3 );
        EXPECT_FALSE( DecodeFracture( cut ) );
    }

    TEST( FractureFormat, ANullGuidIsRefused )
    {
        const FractureData d{};
        EXPECT_FALSE( EncodeFracture( d ) );
    }
} // namespace
