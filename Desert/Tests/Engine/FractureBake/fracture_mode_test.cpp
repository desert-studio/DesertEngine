// DST-02: the Fracture mode's command layer (Engine/Destruction/FractureEdit.hpp, FractureAsset::WriteStep) —
// what the editor's Generate, level list, interior material and Explode slider do, without a window.

#include <Engine/Assets/FractureAsset.hpp>
#include <Engine/Destruction/FractureEdit.hpp>
#include <Engine/Destruction/FractureFormat.hpp>
#include <Engine/Geometry/MeshCore/VectorUtil.hpp>

#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace
{
    using namespace Desert::Destruction;
    using Desert::Geometry::DynamicMesh3;

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

    FractureLevelSettings Uniform( int sites )
    {
        FractureLevelSettings l;
        l.Method    = FractureMethod::Uniform;
        l.SiteCount = sites;
        return l;
    }

    FractureSettings Settings( uint64_t seed, std::vector<FractureLevelSettings> levels )
    {
        FractureSettings s;
        s.Seed   = seed;
        s.Levels = std::move( levels );
        return s;
    }

    FractureData Current()
    {
        FractureData d;
        d.SourceMesh       = { 0x5005ull, 0xCBEull };
        d.InteriorMaterial = { 0x1A7E, 0x2108 };
        return d;
    }

    struct Bounds
    {
        glm::dvec3 Min{ 1e30 };
        glm::dvec3 Max{ -1e30 };
    };

    Bounds LeafBounds( const FractureData& d, size_t& leaves )
    {
        Bounds b;
        leaves = 0;
        for ( const FractureNode& n : d.Nodes )
        {
            if ( n.Mesh.Triangles.empty() )
                continue;
            ++leaves;
            for ( size_t i = 0; i + 2 < n.Mesh.Positions.size(); i += 3 )
            {
                const glm::dvec3 p( n.Mesh.Positions[i], n.Mesh.Positions[i + 1], n.Mesh.Positions[i + 2] );
                b.Min = glm::min( b.Min, p );
                b.Max = glm::max( b.Max, p );
            }
        }
        return b;
    }

    std::filesystem::path ScratchFile( const char* name )
    {
        const auto dir = std::filesystem::temp_directory_path() / "DesertFractureMode";
        std::filesystem::create_directories( dir );
        const auto file = dir / name;
        std::filesystem::remove( file );
        return file;
    }

    FractureData ReadBack( const std::filesystem::path& file )
    {
        std::ifstream              in( file, std::ios::binary );
        std::vector<unsigned char> bytes( ( std::istreambuf_iterator<char>( in ) ),
                                          std::istreambuf_iterator<char>() );
        auto                       decoded = DecodeFracture( bytes );
        EXPECT_TRUE( decoded ) << ( decoded ? "" : decoded.GetError() );
        return decoded ? decoded.GetValue() : FractureData{};
    }

    std::string RepoRoot()
    {
        std::string prefix = "./";
        for ( int up = 0; up < 6; ++up )
        {
            std::ifstream probe( prefix + "Desert/Desert/Source/Engine/ECS/Components.hpp" );
            if ( probe )
                return prefix;
            prefix += "../";
        }
        return {};
    }

    std::string ReadText( const std::string& path )
    {
        std::ifstream      in( path );
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }
} // namespace

// Generate N pieces with a seed: the same seed gives the same piece count and the same bounds (the source's),
// and a different seed gives different pieces. Red when Generate stops being a function of (source, settings).
TEST( FractureMode, GenerateWithASeedIsDeterministicInCountAndBounds )
{
    const auto a = GenerateFracture( Cube(), Current().SourceMesh, Current(), Settings( 7, { Uniform( 8 ) } ) );
    const auto b = GenerateFracture( Cube(), Current().SourceMesh, Current(), Settings( 7, { Uniform( 8 ) } ) );
    const auto c = GenerateFracture( Cube(), Current().SourceMesh, Current(), Settings( 8, { Uniform( 8 ) } ) );
    ASSERT_TRUE( a && b && c );

    size_t     leavesA = 0, leavesB = 0, leavesC = 0;
    const auto boundsA = LeafBounds( a.GetValue(), leavesA );
    const auto boundsB = LeafBounds( b.GetValue(), leavesB );
    LeafBounds( c.GetValue(), leavesC );
    EXPECT_GE( leavesA, 2u );
    EXPECT_EQ( leavesA, leavesB );
    EXPECT_EQ( boundsA.Min, boundsB.Min );
    EXPECT_EQ( boundsA.Max, boundsB.Max );
    for ( int k = 0; k < 3; ++k )
    {
        EXPECT_NEAR( boundsA.Min[k], -50.0, 1e-3 );
        EXPECT_NEAR( boundsA.Max[k], 50.0, 1e-3 );
    }
    EXPECT_EQ( EncodeFracturePayload( a.GetValue() ), EncodeFracturePayload( b.GetValue() ) );
    EXPECT_NE( EncodeFracturePayload( a.GetValue() ), EncodeFracturePayload( c.GetValue() ) );

    // Generate records the mesh it cut; the interior material is the current fracture's.
    EXPECT_EQ( a.GetValue().SourceMesh, Current().SourceMesh );
    EXPECT_EQ( a.GetValue().InteriorMaterial, Current().InteriorMaterial );
}

// The level list nests: every level-2 piece hangs under a level-1 piece, and there are more of them.
TEST( FractureMode, LevelsNestUnderThePreviousLevel )
{
    const auto r = GenerateFracture( Cube(), Current().SourceMesh, Current(),
                                     Settings( 3, { Uniform( 4 ), Uniform( 3 ) } ) );
    ASSERT_TRUE( r );
    const FractureData& d = r.GetValue();
    EXPECT_EQ( DeepestLevel( d.Nodes ), 2u );
    size_t level1 = 0, level2 = 0;
    for ( const FractureNode& n : d.Nodes )
    {
        if ( n.Level == 1 )
            ++level1;
        if ( n.Level != 2 )
            continue;
        ++level2;
        ASSERT_GE( n.Parent, 0 );
        EXPECT_EQ( d.Nodes[static_cast<size_t>( n.Parent )].Level, 1u );
    }
    EXPECT_GE( level1, 2u );
    EXPECT_GT( level2, level1 );
}

// The interior material is written to the asset and read back (format v2), and it changes the file.
TEST( FractureMode, InteriorMaterialIsWrittenToTheAssetAndReadBack )
{
    auto generated = GenerateFracture( Cube(), Current().SourceMesh, Current(), Settings( 5, { Uniform( 4 ) } ) );
    ASSERT_TRUE( generated );
    FractureData data = generated.GetValue();

    const auto file = ScratchFile( "Interior.dfrac" );
    ASSERT_TRUE( Desert::Assets::FractureAsset::Save( file, data ) );
    EXPECT_EQ( ReadBack( file ).InteriorMaterial, Current().InteriorMaterial );

    data.InteriorMaterial = { 0xBEEF, 0xF00D };
    const auto step       = Desert::Assets::FractureAsset::WriteStep( file, data );
    ASSERT_TRUE( step );
    EXPECT_NE( step.GetValue().Before, step.GetValue().After );
    const FractureData back = ReadBack( file );
    EXPECT_EQ( back.InteriorMaterial, ( Desert::Common::Content::AssetGuid{ 0xBEEF, 0xF00D } ) );
    EXPECT_EQ( back.Nodes, data.Nodes );
}

// Undo of Generate restores the previous collection exactly (pieces, settings, GUID); redo brings the new one
// back; undoing the first Generate of a new file removes the file.
TEST( FractureMode, UndoOfGenerateRestoresThePreviousCollection )
{
    using Desert::Assets::FractureAsset;
    const auto file = ScratchFile( "Undo.dfrac" );

    auto first = GenerateFracture( Cube(), Current().SourceMesh, Current(), Settings( 11, { Uniform( 5 ) } ) );
    ASSERT_TRUE( first );
    const auto step1 = FractureAsset::WriteStep( file, first.GetValue() );
    ASSERT_TRUE( step1 );
    EXPECT_TRUE( step1.GetValue().Before.empty() );
    const FractureData previous = ReadBack( file );

    auto second = GenerateFracture( Cube(), Current().SourceMesh, previous, Settings( 12, { Uniform( 9 ) } ) );
    ASSERT_TRUE( second );
    const auto step2 = FractureAsset::WriteStep( file, second.GetValue() );
    ASSERT_TRUE( step2 );
    EXPECT_EQ( ReadBack( file ).Guid, previous.Guid ); // a re-bake keeps the asset's identity
    EXPECT_NE( ReadBack( file ).Nodes, previous.Nodes );

    ASSERT_TRUE( FractureAsset::RestoreBytes( file, step2.GetValue().Before ) );
    EXPECT_EQ( ReadBack( file ), previous );

    ASSERT_TRUE( FractureAsset::RestoreBytes( file, step2.GetValue().After ) );
    EXPECT_EQ( ReadBack( file ).Settings, second.GetValue().Settings );

    ASSERT_TRUE( FractureAsset::RestoreBytes( file, step1.GetValue().Before ) );
    EXPECT_FALSE( std::filesystem::exists( file ) );
}

// UE keeps Explode Amount / Fracture Level in UFractureSettings (editor tool settings) and ApplyExplodedView moves
// only the component's display transforms, never the collection. Ours too: the view settings are not in the
// `.dfrac` format nor in the fracture data, and the offsets are a pure function of (nodes, view).
TEST( FractureMode, TheExplodeSliderIsPreviewStateNotSerialized )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );
    for ( const char* path : { "Desert/Desert/Source/Engine/Destruction/FractureFormat.hpp",
                               "Desert/Desert/Source/Engine/Destruction/FractureFormat.cpp",
                               "Desert/Desert/Source/Engine/Destruction/FractureBake.hpp" } )
    {
        const std::string text = ReadText( root + path );
        ASSERT_FALSE( text.empty() ) << path;
        EXPECT_EQ( text.find( "Explode" ), std::string::npos ) << path << " serializes the preview";
        EXPECT_EQ( text.find( "ViewLevel" ), std::string::npos ) << path << " serializes the preview";
    }

    auto r = GenerateFracture( Cube(), Current().SourceMesh, Current(),
                               Settings( 3, { Uniform( 4 ), Uniform( 3 ) } ) );
    ASSERT_TRUE( r );
    const FractureData& d      = r.GetValue();
    const auto          before = EncodeFracturePayload( d );

    // Amount 0: assembled.
    for ( const glm::dvec3& o : ExplodedOffsets( d.Nodes, {} ) )
        EXPECT_EQ( o, glm::dvec3( 0.0 ) );

    // All levels: each node moves by its parent's offset plus its own centre's distance from the parent's.
    const auto all = ExplodedOffsets( d.Nodes, { 1.0f, -1 } );
    for ( size_t i = 1; i < d.Nodes.size(); ++i )
    {
        const size_t     p        = static_cast<size_t>( d.Nodes[i].Parent );
        const glm::dvec3 expected = all[p] + ( d.Nodes[i].CenterOfMass - d.Nodes[p].CenterOfMass );
        EXPECT_NEAR( glm::length( all[i] - expected ), 0.0, 1e-9 );
    }

    // One level: level-1 nodes move from the root's centre, level-2 nodes ride with their parent, the root stays.
    const auto one = ExplodedOffsets( d.Nodes, { 2.0f, 1 } );
    EXPECT_EQ( one[0], glm::dvec3( 0.0 ) );
    for ( size_t i = 1; i < d.Nodes.size(); ++i )
    {
        const FractureNode& n = d.Nodes[i];
        if ( n.Level == 1 )
            EXPECT_NEAR( glm::length( one[i] - ( n.CenterOfMass - d.Nodes[0].CenterOfMass ) * 2.0 ), 0.0, 1e-9 );
        else if ( n.Level == 2 )
            EXPECT_EQ( one[i], one[static_cast<size_t>( n.Parent )] );
    }

    // Exploding changed nothing that is saved.
    EXPECT_EQ( EncodeFracturePayload( d ), before );
}

// The first Generate of a NEW .dfrac (nothing on disk: the current fracture is empty, its source null) records
// the mesh it cut, and a re-bake of another mesh records that one. Red when Generate copies the source from the
// current fracture (a new asset then names no mesh) or ignores the mesh it was given.
TEST( FractureMode, FirstGenerateRecordsTheMeshItCutAsTheSource )
{
    const Desert::Common::Content::AssetGuid mesh{ 0x5717Cull, 0xAE5Bull };
    const auto first = GenerateFracture( Cube(), mesh, FractureData{}, Settings( 21, { Uniform( 4 ) } ) );
    ASSERT_TRUE( first ) << ( first ? "" : first.GetError() );
    EXPECT_EQ( first.GetValue().SourceMesh, mesh );
    EXPECT_FALSE( first.GetValue().SourceMesh.IsNull() );

    const auto file = ScratchFile( "FirstGenerate.dfrac" );
    ASSERT_TRUE( Desert::Assets::FractureAsset::WriteStep( file, first.GetValue() ) );
    EXPECT_EQ( ReadBack( file ).SourceMesh, mesh );

    const Desert::Common::Content::AssetGuid other{ 0x07E2ull, 0x0B1Eull };
    const auto rebake = GenerateFracture( Cube(), other, ReadBack( file ), Settings( 21, { Uniform( 4 ) } ) );
    ASSERT_TRUE( rebake );
    EXPECT_EQ( rebake.GetValue().SourceMesh, other );
}
